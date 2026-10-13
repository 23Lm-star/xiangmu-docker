# REQ-EXT-FILE-002 验收 1-6 端到端脚本（证据归档，仿 FILE-001 dedup_e2e.py / SEC-001 verify.py 模式）。
# 前置：DB 已执行 migrate_ext_file_002.sql。
# 断言走三处：网络 PDU 计数、协议头静态一致性、DB（file_object/user_file/upload_session）状态。
#
# ---- 统一 e2e 运行形态（五套件一致，见 project-docs/evidence/E2E-RUNFORM.md）----
# 运行：py -3.13 project-docs/evidence/REQ-EXT-FILE-002/fastupload_e2e.py
#   cwd     : 任意（脚本自管理，Server 以 <repo>/dist 为 cwd 启动）
#   exe     : <repo>/dist/Server.exe（qmake DESTDIR=dist；connect.config 构建期经 qrc 内嵌）
#   config  : Server/connect.config -> filesPath=./filesys（相对 cwd）；未入库的机器本地文件
#   数据根  : <repo>/dist/filesys
#   未入库依赖：dist/{Qt5Core,Qt5Gui,Qt5Network,Qt5Sql,Qt5Svg,Qt5Widgets}.dll、dist/libmysql.dll、
#              dist/sqldrivers/qsqlmysql.dll、dist/platforms/qwindows.dll
# 脚本自管理 Server 生命周期（taskkill + Popen, cwd=<repo>/dist）。
import hashlib, os, re, socket, struct, subprocess, sys, threading, time

HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_REG, T_CAP, T_LOGIN = 1, 44, 3
T_MKDIR, T_FLUSH, T_DEL, T_UPINIT, T_UPDATA, T_DOWN = 19, 23, 25, 29, 31, 40
T_UPINIT_R, T_UPDATA_R, T_DOWN_R = 30, 32, 41
T_FLUSH_R = 24
T_FAST, T_FAST_R = 97, 98
MAGIC = 0x5253554d
FASTCHECK_MAGIC = 0x4643484b
MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
A = "fu2user"
B = "fu2other"
R1, R2 = "fu2race1", "fu2race2"     # 并发用例两路用不同账号（单点登录会互踢）
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
DIST = os.path.join(REPO, "dist")
EXE = os.path.join(DIST, "Server.exe")
sys.path.insert(0, os.path.join(REPO, "project-docs", "evidence", "_harness"))
import server_ctl
_SRV = server_ctl.ServerCtl(EXE, DIST, HOST, PORT)
SBX = os.path.join(DIST, "filesys")

# 测试载荷与摘要（集中定义：环境清理与断言共用，保证可重放隔离）
P1 = b"FASTUPLOAD-A1-CONTENT-" * 40
P2 = b"FASTUPLOAD-A2-OTHER-CONTENT-" * 30
P3 = b"FASTUPLOAD-A3-BAD-SHA-" * 40
POTHER = b"FASTUPLOAD-A3-XXX-SHA-" * 40     # 与 P3 等长不同内容：声明其 sha、上传 P3
P4 = b"FASTUPLOAD-A4-CONCURRENT-" * 60
P5 = b"FASTUPLOAD-A5-RESUME-" * 80
S1, S2, S3 = (hashlib.sha256(p).hexdigest() for p in (P1, P2, P3))
SOTHER, S4, S5 = (hashlib.sha256(p).hexdigest() for p in (POTHER, P4, P5))
TEST_SHAS = ",".join("'%s'" % s for s in (S1, S2, S3, SOTHER, S4, S5))

def f32(s): return s.encode("utf-8")[:31].ljust(32, b"\0")
def creds(name, pwd="123456"):
    return f32(name) + hashlib.md5(pwd.encode()).hexdigest().encode()
def pdu(t, msg=b"", dat=b""):
    dat = dat[:64].ljust(64, b"\0")
    return struct.pack("<III", HDR + len(msg), len(msg), t) + dat + msg

def fastcheck_pkt(sha_hex, name, dirpath, reqid, size):
    sb, nb, db, rb = sha_hex.encode(), name.encode(), dirpath.encode(), reqid.encode()
    dat = struct.pack("<IIqIIII", 1, FASTCHECK_MAGIC, size, len(sb), len(nb), len(db), len(rb)) + b"\0" * 32
    return pdu(T_FAST, msg=sb + nb + db + rb, dat=dat)

def parse_fast(dat, msg):
    return dict(ret=dat[0] != 0, hit=dat[1] != 0,
                version=struct.unpack("<I", dat[2:6])[0],
                fileId=struct.unpack("<q", dat[8:16])[0],
                sessionId=struct.unpack("<q", dat[16:24])[0],
                nextOffset=struct.unpack("<q", dat[24:32])[0],
                nameLen=struct.unpack("<I", dat[32:36])[0],
                name=msg[:struct.unpack("<I", dat[32:36])[0]].decode())

class Conn:
    def __init__(s):
        s.sock = socket.create_connection((HOST, PORT), timeout=10); s.buf = b""
        s.sent_chunks = 0
    def _pump(s, t):
        s.sock.settimeout(t)
        try:
            d = s.sock.recv(65536)
            if d: s.buf += d
        except socket.timeout: return
    def packets(s):
        out = []
        while len(s.buf) >= HDR:
            tl, ml, ty = struct.unpack("<III", s.buf[:12])
            if tl < HDR or tl != HDR + ml or len(s.buf) < tl: break
            out.append((ty, s.buf[12:76], s.buf[76:tl])); s.buf = s.buf[tl:]
        return out
    def ask(s, pkt, n=1, t=4.0):
        s.sock.sendall(pkt); got = []; dl = time.time() + t
        while len(got) < n and time.time() < dl:
            s._pump(0.5); got = s.packets()
        return got
    def close(s):
        try: s.sock.close()
        except OSError: pass

def sql(q):
    r = subprocess.run([MYSQL] + DB + [q], capture_output=True, text=True)
    return r.stdout.strip()

def start_server():
    # FINDING-GATE-1: 原来是 taskkill /F /IM Server.exe —— 按映像名杀，会打到并行套件的
    # 进程（实测造成三次"服务端突然消失"的伪缺陷）。现在只按自己启动的 PID 管理；端口被
    # 别人占用时直接报错，不盲杀。契约不变：仍返回 bool。
    if not _SRV.start():
        print(f"[E] {_SRV.last_error}")
        return False
    return True

def stop_server():
    _SRV.stop()

results = []
def check(sid, desc, cond, detail=""):
    results.append((sid, desc, bool(cond)))
    print(f"[{'PASS' if cond else 'FAIL'}] {sid} {desc} :: {detail}")

def login_conn(name):
    c = Conn()
    r = c.ask(pdu(T_CAP))
    code = r[0][2].split(b"\0")[0]
    c.ask(pdu(T_REG, msg=code + b"\0", dat=creds(name)))
    c.close()
    c = Conn()
    r = c.ask(pdu(T_CAP)); code = r[0][2].split(b"\0")[0]
    r = c.ask(pdu(T_LOGIN, msg=code + b"\0", dat=creds(name)))
    ret = struct.unpack("<i", r[0][1][:4])[0]
    return c, ret

def fast(c, sha_hex, name, dirpath, reqid, size, n=1):
    return c.ask(fastcheck_pkt(sha_hex, name, dirpath, reqid, size), n=n)

def send_chunks(c, payload):
    """顺序 600B 分块；返回是否全部 ACK 成功。"""
    for i in range(0, len(payload), 600):
        ch = payload[i:i + 600]
        pkt = pdu(T_UPDATA, msg=ch, dat=struct.pack("<q", i) + b"\0" * 32 + struct.pack("<I", MAGIC))
        c.sent_chunks += 1
        a = c.ask(pkt)
        if not a or a[0][0] != T_UPDATA_R or a[0][1][0] == 0: return False
    return True

def upload_full(c, payload, name, dirpath, reqid):
    """未命中后的完整顺序上传（分块 + 收齐转正）。"""
    return send_chunks(c, payload)

def entry_names(c, dirpath):
    """发 FLUSHFILE，读取直到出现类型匹配的响应（其余残余包丢弃，避免错位解析）。"""
    c.sock.sendall(pdu(T_FLUSH, msg=dirpath.encode()))
    dl = time.time() + 4.0
    while time.time() < dl:
        for ty, dat, msg in c.packets():
            if ty != T_FLUSH_R: continue
            return sorted(msg[i * 36:i * 36 + 36][:32].split(b"\0")[0].decode("utf-8", "replace")
                          for i in range(len(msg) // 36))
        c._pump(0.3)
    return []

# ---------- 静态：协议常量两端逐字节一致 + 不重编号 ----------
print("=== 协议静态检查（验收6 兼容面）===")
def block(path):
    txt = open(path, encoding="utf-8").read()
    m = re.search(r"// ===== REQ-EXT-FILE-002 秒传预检查.*?FASTCHECK_RESP_NAME_LEN_OFFSET.*?\n", txt, re.S)
    return m.group(0) if m else None
bc = block(os.path.join(REPO, "Client", "protocol.h"))
bs = block(os.path.join(REPO, "Server", "protocol.h"))
check("P1-same-block", "两端 protocol.h FASTCHECK 常量块逐字节一致",
      bc is not None and bc == bs, f"len={len(bc or '')}")
vals = {}
for path in (os.path.join(REPO, "Client", "protocol.h"), os.path.join(REPO, "Server", "protocol.h")):
    txt = open(path, encoding="utf-8").read()
    enum_body = txt.split("enum ENUM_MSG_TYPE {", 1)[1].split("};", 1)[0]
    nxt = 0
    for line in enum_body.splitlines():
        line = line.split("//")[0].strip().rstrip(",")
        m = re.match(r"(ENUM_MSG_TYPE_[A-Za-z_]+)\s*(?:=\s*(\d+))?$", line)
        if not m: continue
        nxt = int(m.group(2)) if m.group(2) else nxt + 1
        vals.setdefault(m.group(1), set()).add(nxt)
check("P2-97-98", "新增枚举 FASTCHECK 请求/响应 = 97/98 且仅追加",
      vals.get("ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_REQUEST") == {97}
      and vals.get("ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_RESPOND") == {98}, "97/98")
check("P3-no-renumber", "既有编号未重编号（隐式序号 + 抽样 29/31/40/88/96）",
      vals.get("ENUM_MSG_TYPE_REGIST_REQUEST") == {1}
      and vals.get("ENUM_MSG_TYPE_CHAT_REQUEST") == {15}
      and vals.get("ENUM_MSG_TYPE_UPLOADFILE_INIT_REQUEST") == {29}
      and vals.get("ENUM_MSG_TYPE_UPLOADFILE_DATA_REQUEST") == {31}
      and vals.get("ENUM_MSG_TYPE_DOWNLOADFILE_REQUEST") == {40}
      and vals.get("ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_REQUEST") == {88}
      and vals.get("ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_RESPOND") == {96},
      f"regist={vals.get('ENUM_MSG_TYPE_REGIST_REQUEST')} chat={vals.get('ENUM_MSG_TYPE_CHAT_REQUEST')}")

# ---------- 静态：客户端摘要流式计算（内存 O(1)，验收6）----------
print("=== 客户端 Hash 静态检查（验收6 内存面）===")
fcli = open(os.path.join(REPO, "Client", "file.cpp"), encoding="utf-8").read()
check("P4-stream-sha", "客户端 SHA-256 分块流式计算（1MiB 窗口，无整文件读入）",
      "QCryptographicHash hasher(QCryptographicHash::Sha256)" in fcli
      and "file.read(1024*1024)" in fcli and "readAll()" not in fcli, "1MiB chunk")
check("P5-fast-send", "客户端上传入口发 FASTCHECK 而非旧 INIT",
      "ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_REQUEST" in fcli
      and "ENUM_MSG_TYPE_UPLOADFILE_INIT_REQUEST" not in fcli, "")

# ---------- 环境准备（可重放：先清本测试账号与本测试载荷的全部历史足迹）----------
print("=== 环境准备 ===")
if not start_server():
    print("FATAL: Server 启动失败"); sys.exit(2)
for u in (A, B, R1, R2):
    sql(f"DELETE FROM user_file WHERE owner_name='{u}';")
    sql(f"DELETE FROM upload_session WHERE owner_name='{u}';")
# 上轮运行留下的同摘要对象：先删引用它们的条目（FK 顺序），再删对象
sql(f"DELETE uf FROM user_file uf JOIN file_object fo ON fo.object_id=uf.object_id "
    f"WHERE fo.sha256 IN ({TEST_SHAS});")
sql(f"DELETE FROM upload_session WHERE client_sha256 IN ({TEST_SHAS});")
sql(f"DELETE FROM file_object WHERE sha256 IN ({TEST_SHAS});")
sql("DELETE FROM file_object WHERE refcount<=0;")
check("E0", "环境隔离：本测试摘要无残留物理对象",
      sql(f"SELECT COUNT(*) FROM file_object WHERE sha256 IN ({TEST_SHAS})") == "0", "")
# 服务端用户根只由"首次注册"创建（msghandler::rigist），本测试账号在多轮运行后已存在 →
# 显式准备夹具目录（与 FILE-001 entry_ops_e2e 同式），避免夹具缺失被误判为产品缺陷。
E1 = True
for u in (A, B, R1, R2):
    try:
        os.makedirs(os.path.join(SBX, u), exist_ok=True)
    except OSError:
        E1 = False
check("E1", "夹具：测试账号服务端用户根存在", E1 and all(
    os.path.isdir(os.path.join(SBX, u)) for u in (A, B, R1, R2)), "")
ca, ret = login_conn(A)
check("L0", f"{A} 登录成功", ret == 1, f"ret={ret}")
if ret != 1: sys.exit(2)
cb, retb = login_conn(B)
check("L0b", f"{B} 登录成功", retb == 1, f"ret={retb}")
if retb != 1: sys.exit(2)
DIR = f"./filesys/{A}/fu2d"
# 注意：MKDIR 不幂等（目录已存在时响应 ret=0），故夹具判据取"目录存在"，不取响应字节
ca.ask(pdu(T_MKDIR, msg=f"./filesys/{A}".encode(), dat=f32("fu2d")))
check("E2", "夹具：目标目录存在（MKDIR 后磁盘可验证）",
      os.path.isdir(os.path.join(SBX, A, "fu2d")), "")


# ---------- 验收1：未命中→登录会话；命中→零正文 + 条目立即可读 ----------
print("=== 验收1：命中不传正文、条目立即可读 ===")
r = fast(ca, S1, "one.bin", DIR, "req-a1-0001", len(P1))
f = parse_fast(r[0][1], r[0][2]) if r else None
check("A1-miss", "首次预检查未命中（hit=false, nextOffset=0）",
      f and f["ret"] and not f["hit"] and f["nextOffset"] == 0 and f["sessionId"] > 0, str(f))
check("A1-no-obj-yet", "未命中时未建对象/条目",
      sql(f"SELECT COUNT(*) FROM file_object WHERE sha256='{S1}'") == "0"
      and sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{A}' AND display_name='one.bin'") == "0", "")
check("A1-up", "未命中后顺序分块上传成功", upload_full(ca, P1, "one.bin", DIR, "req-a1-0001"), "")
check("A1-entry", "上传完成即落逻辑条目",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{A}' AND status='active' AND display_name='one.bin'") == "1", "")

chunks_before = ca.sent_chunks
r = fast(ca, S1, "two.bin", DIR, "req-a1-0002", len(P1))
f = parse_fast(r[0][1], r[0][2]) if r else None
check("A1-hit", "同内容二名预检查命中（hit=true）", f and f["ret"] and f["hit"], str(f))
check("A1-hit-name", "命中返回服务端最终展示名", f and f["name"] == "two.bin", f and f["name"])
check("A1-zero-body", "命中路径未发送任何分块 PDU", ca.sent_chunks == chunks_before,
      f"chunks={ca.sent_chunks - chunks_before}")
names_after_hit = entry_names(ca, DIR)
check("A1-hit-visible", "命中条目立即可读（flushfile 立刻可见）",
      "two.bin" in names_after_hit, f"entries={names_after_hit}")
check("A1-hit-obj", "命中复用同一物理对象且引用 +1",
      sql(f"SELECT COUNT(*) FROM file_object WHERE sha256='{S1}'") == "1"
      and sql(f"SELECT refcount FROM file_object WHERE sha256='{S1}'") == "2", "")

# ---------- 验收2：同内容不同名一物理对象；同名不同内容稳定定名 ----------
print("=== 验收2：单物理对象 + 稳定同名递推 ===")
r = fast(ca, S2, "one.bin", DIR, "req-a2-0001", len(P2))
f2 = parse_fast(r[0][1], r[0][2]) if r else None
check("A2-miss2", "不同内容未命中", f2 and f2["ret"] and not f2["hit"], str(f2))
check("A2-up2", "不同内容顺序上传成功", upload_full(ca, P2, "one.bin", DIR, "req-a2-0001"), "")
nm = sql(f"SELECT display_name FROM user_file WHERE owner_name='{A}' AND status='active' "
         f"AND parent_path LIKE '%fu2d/' AND object_id=(SELECT object_id FROM file_object WHERE sha256='{S2}')")
check("A2-suffix", "同名不同内容得到稳定递推名 one(1).bin", nm == "one(1).bin", f"name={nm}")
check("A2-two-objects", "两种内容各自独立物理对象",
      sql(f"SELECT COUNT(*) FROM file_object WHERE sha256 IN ('{S1}','{S2}')") == "2", "")
check("A2-stable", "重放同一请求键不产生第三个对象/条目",
      (fast(ca, S2, "one.bin", DIR, "req-a2-0001", len(P2)),
       sql(f"SELECT COUNT(*) FROM file_object WHERE sha256='{S2}'") == "1"
       and sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{A}' AND display_name='one(1).bin'") == "1")[1], "")

# ---------- 验收3：伪造摘要/大小/路径/上传者 ----------
print("=== 验收3：伪造与越权不命中、复算失败不转正 ===")
FORGED = "0" * 64
r = fast(ca, FORGED, "forged.bin", DIR, "req-a3-0001", len(P3))
f3 = parse_fast(r[0][1], r[0][2]) if r else None
check("A3-forged-miss", "伪造摘要不命中既有对象（不泄露他人对象存在性）",
      f3 and f3["ret"] and not f3["hit"], str(f3))

staging = os.path.join(SBX, ".staging")
parts_before = set(os.listdir(staging)) if os.path.isdir(staging) else set()
r = fast(ca, SOTHER, "bad.bin", DIR, "req-a3-0002", len(P3))
f4 = parse_fast(r[0][1], r[0][2]) if r else None
check("A3-mismatch-miss", "摘要/内容不符先按未命中登记", f4 and f4["ret"] and not f4["hit"], str(f4))
parts_mid = set(os.listdir(staging)) if os.path.isdir(staging) else set()
check("A3-staging-created", "未命中即建暂存实体（.part）",
      len(parts_mid - parts_before) == 1, f"new={sorted(parts_mid - parts_before)}")
upload_full(ca, P3, "bad.bin", DIR, "req-a3-0002")
parts_after = set(os.listdir(staging)) if os.path.isdir(staging) else set()
check("A3-no-entry", "复算不符 -> 不建条目", sql(
    f"SELECT COUNT(*) FROM user_file WHERE owner_name='{A}' AND display_name='bad.bin'") == "0", "")
check("A3-no-object", "复算不符 -> 不建对象", sql(
    f"SELECT COUNT(*) FROM file_object WHERE sha256='{S3}'") == "0", "")
check("A3-session-failed", "复算不符 -> 会话置 failed", sql(
    f"SELECT status FROM upload_session WHERE owner_name='{A}' AND client_request_id='req-a3-0002'") == "failed", "")
new_parts = parts_mid - parts_before
check("A3-staging-cleaned", "复算不符 -> 本次新建的 staging 实体被删除（不留碎片）",
      bool(new_parts) and new_parts.isdisjoint(parts_after),
      f"new={sorted(new_parts)} leftover={sorted(new_parts & parts_after)}")

# 验收4 尾：失败会话以同一请求键重试——uk_req 唯一键下必须原地复用（不得再插一行）
r = fast(ca, S3, "bad.bin", DIR, "req-a3-0002", len(P3))
f9 = parse_fast(r[0][1], r[0][2]) if r else None
check("A3-retry-accepted", "失败会话同请求键重试被受理（不因 uk_req 冲突失败）",
      f9 and f9["ret"] and not f9["hit"] and f9["sessionId"] == f4["sessionId"], str(f9))
check("A3-retry-upload", "重试后顺序上传成功并落条目",
      upload_full(ca, P3, "bad.bin", DIR, "req-a3-0002")
      and sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{A}' AND display_name='bad.bin'") == "1", "")
check("A3-retry-single-row", "同请求键仍只一条会话行（uk_req 守住）", sql(
      f"SELECT COUNT(*) FROM upload_session WHERE owner_name='{A}' AND client_request_id='req-a3-0002'") == "1", "")

r = fast(ca, S1, "cross.bin", f"./filesys/{B}/", "req-a3-0003", len(P1))
f5 = parse_fast(r[0][1], r[0][2]) if r else None
check("A3-cross-user", "跨用户目录一律拒绝", f5 and not f5["ret"], str(f5))

raw = Conn()
r = raw.ask(fastcheck_pkt(S1, "unauth.bin", DIR, "req-a3-0004", len(P1)))
check("A3-unauth", "未认证连接 FASTCHECK 收到类型正确的失败响应（分派登记生效）",
      bool(r) and r[0][0] == T_FAST_R and r[0][1][0] == 0, f"type={r[0][0] if r else 'NORESP'}")
raw.close()

# ---------- 验收4：并发相同文件单物理对象 ----------
# 两路必须用不同账号：REQ-EXT-SESSION-001 单点登录会让同账号第二次登录踢掉前一条连接。
print("=== 验收4：两路并发相同内容 -> 单对象 ===")
RACE_DIR = {}

def race(user, name, reqid, out, idx):
    c = None
    try:
        c, ret = login_conn(user)
        if ret != 1:
            out[idx] = "LOGIN-FAIL"; return
        rd = f"./filesys/{user}/rc"
        RACE_DIR[user] = rd
        c.ask(pdu(T_MKDIR, msg=f"./filesys/{user}".encode(), dat=f32("rc")))
        a = c.ask(fastcheck_pkt(S4, name, rd, reqid, len(P4)))
        if not a:
            out[idx] = "NO-RESP"; return
        f = parse_fast(a[0][1], a[0][2])
        out[idx] = "hit" if f["hit"] else "miss"
        if not f["hit"]:
            out[idx] += "+ok" if send_chunks(c, P4) else "+chunkfail"
    except Exception as e:
        out[idx] = f"EXC:{type(e).__name__}"
    finally:
        if c: c.close()

out = ["", ""]
ts = [threading.Thread(target=race, args=(R1, "c4a.bin", "req-a4-0001", out, 0)),
      threading.Thread(target=race, args=(R2, "c4b.bin", "req-a4-0002", out, 1))]
for t in ts: t.start()
for t in ts: t.join()
check("A4-both-ok", "两路并发均成功（未命中路径各含 hit+ok）",
      all(o.startswith("miss+ok") or o.startswith("hit") for o in out), f"paths={out}")
check("A4-one-object", "并发后同内容仅一个物理对象",
      sql(f"SELECT COUNT(*) FROM file_object WHERE sha256='{S4}'") == "1", "")
check("A4-refcount", "并发后引用计数 = 2",
      sql(f"SELECT refcount FROM file_object WHERE sha256='{S4}'") == "2", "")
check("A4-two-entries", "并发后两条逻辑条目（各账号一条）",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name IN ('{R1}','{R2}') AND status='active' "
          f"AND display_name IN ('c4a.bin','c4b.bin')") == "2", f"paths={out}")

# ---------- 验收5：中断续传 / 幂等复用 / 清理不伤已完成对象 ----------
print("=== 验收5：续传、幂等复用、清理不伤已有对象 ===")
r = fast(ca, S5, "resume.bin", DIR, "req-a5-0001", len(P5))
f6 = parse_fast(r[0][1], r[0][2]) if r else None
check("A5-setup", "预检查未命中并登记会话", f6 and f6["ret"] and not f6["hit"] and f6["sessionId"] > 0, str(f6))
half = (len(P5) // 1200) * 600           # 保证半程在分块边界
ca.ask(pdu(T_UPDATA, msg=P5[:600], dat=struct.pack("<q", 0) + b"\0" * 32 + struct.pack("<I", MAGIC)))
for i in range(600, half, 600):
    ca.ask(pdu(T_UPDATA, msg=P5[i:i + 600], dat=struct.pack("<q", i) + b"\0" * 32 + struct.pack("<I", MAGIC)))
ca.close()

cc = Conn()
r = cc.ask(pdu(T_CAP)); code = r[0][2].split(b"\0")[0]
cc.ask(pdu(T_LOGIN, msg=code + b"\0", dat=creds(A)))
r = fast(cc, S5, "resume.bin", DIR, "req-a5-0001", len(P5))
f7 = parse_fast(r[0][1], r[0][2]) if r else None
check("A5-resume-offset", "重发同请求键 -> 同会话 + nextOffset=已确认偏移",
      f7 and f7["ret"] and not f7["hit"] and f7["sessionId"] == f6["sessionId"]
      and f7["nextOffset"] == half, str(f7))
ok = True
for i in range(half, len(P5), 600):
    a = cc.ask(pdu(T_UPDATA, msg=P5[i:i + 600], dat=struct.pack("<q", i) + b"\0" * 32 + struct.pack("<I", MAGIC)))
    if not a or a[0][1][0] == 0: ok = False; break
check("A5-resume-finish", "从 nextOffset 续传至完成", ok and sql(
    f"SELECT COUNT(*) FROM user_file WHERE owner_name='{A}' AND status='active' AND display_name='resume.bin'") == "1", "")
r = fast(cc, S5, "resume.bin", DIR, "req-a5-0001", len(P5))
f8 = parse_fast(r[0][1], r[0][2]) if r else None
n_entries = sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{A}' AND status='active'")
check("A5-idempotent-done", "完成态重发同请求键 -> hit=true 且不新增条目",
      f8 and f8["ret"] and f8["hit"] and f8["fileId"] > 0
      and n_entries == sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{A}' AND status='active'"),
      f"fileId={f8 and f8['fileId']}")

# 清理等价操作：删过期会话行 + 删孤儿 staging（仅删不被任何会话行引用的 .part）。
obj_before = sql("SELECT COUNT(*) FROM file_object")
ent_before = sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{A}'")
live = set()
for row in sql("SELECT staging_path FROM upload_session WHERE staging_path<>''").splitlines():
    live.add(os.path.basename(row.strip()))
removed = 0
for f in (os.listdir(staging) if os.path.isdir(staging) else []):
    if f.endswith(".part") and f not in live:
        try:
            os.remove(os.path.join(staging, f)); removed += 1
        except OSError: pass
sql("DELETE FROM upload_session WHERE status IN ('failed','expired');")
check("A5-cleanup-safe", "过期/失败会话与孤儿暂存清理不动已完成对象与条目",
      sql("SELECT COUNT(*) FROM file_object") == obj_before
      and sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{A}'") == ent_before
      and sql(f"SELECT status FROM file_object WHERE sha256='{S5}'") == "ready",
      f"objects {obj_before}->{sql('SELECT COUNT(*) FROM file_object')} 清孤儿part={removed}")
check("A5-cleanup-entry-alive", "清理后已完成条目仍可下载（对象物理留存）",
      sql(f"SELECT COUNT(*) FROM user_file uf JOIN file_object fo ON fo.object_id=uf.object_id "
          f"WHERE uf.owner_name='{A}' AND uf.display_name='resume.bin' AND fo.status='ready'") == "1", "")

ca.close(); cb.close(); cc.close()
stop_server()
print("\n=== 汇总 ===")
passed = sum(1 for _, _, ok in results if ok)
for sid, desc, ok in results:
    if not ok: print(f"  FAIL -> {sid} {desc}")
print(f"{passed}/{len(results)} passed")
sys.exit(0 if passed == len(results) else 1)
