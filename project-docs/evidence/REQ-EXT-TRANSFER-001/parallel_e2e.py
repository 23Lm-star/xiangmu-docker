# REQ-EXT-TRANSFER-001 多文件有限并发：e2e（服务端可观测面 T-01..T-18）。
# 运行形态遵循 project-docs/evidence/E2E-RUNFORM.md（cwd 任意；脚本自管理 dist/Server.exe，串行独占 5000 端口）。
# 断言口径：本套件为裸 socket 驱动，覆盖 per-transfer 状态表、独占上限、公平调度、107/108、
# 空闲回收、断线清理、id=0 旧语义与冻结面 §9 逐字一致。纯客户端行为（多选 UI / 队列不发起
# 第 4 个 29 / 暂停继续取消按钮 / 大文件摘要不卡 UI）属 design §6.3 人工清单，见文件末尾 NOTE。
# 例外：T-19 逐字节复现真实客户端的并行上传握手序列（97→98→29→30→31/32）——裸 29 看不见
# "97 秒传占位吞掉上传名额"（AUD-1 回归锚点）；T-20 为空闲/写盘竞态不变量（AUD-4 回归锚点）；
# T-21 为 98 回带请求标识契约（AUD-3 服务端锚点）；T-22 为客户端队列静态契约（AUD-2/F-3）；
# T-23 为 97 纯占位纳入空闲回收（N-1 回归锚点，复用 T-13 静默窗口）。
#
# 与 design §6.2 的历史差异（已收口）：
#   D-a T-06：原 design 写"8MB×1 + 64KB×3"共 4 条下载流，与主人裁定的 D6（下载上限 3）冲突；
#       主人 2026-10-01 仲裁后 repair 节点已把 design §6.2 T-06 文本修订为 3 条流（512KB×1 +
#       1KB×2），与本套件一致。本套件仍按 1 大 + 2 小断言同一公平性性质。
import hashlib, os, re, shutil, socket, struct, subprocess, sys, time

HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_REG, T_CAP, T_CAP_R, T_LOGIN, T_LOGIN_R, T_PING, T_PONG = 1, 44, 45, 3, 4, 50, 51
T_MKDIR, T_MKDIR_R, T_FLUSH = 19, 20, 23
T_UPINIT, T_UPDATA, T_UPINIT_R, T_UPDATA_R = 29, 31, 30, 32
T_FC, T_FC_R = 97, 98   # 秒传预检查（仅 T-19 用：复现真实客户端握手）
T_DOWN, T_DOWN_R, T_DATA, T_FINISH = 40, 41, 42, 43
T_ABORT, T_ABORT_R = 107, 108
T_GCREATE, T_GCREATE_R = 60, 61
T_GF_LIST, T_GF_LIST_R, T_GF_INIT, T_GF_INIT_R = 99, 100, 101, 102
T_GF_DOWN, T_GF_DOWN_R = 105, 106
MAGIC = 0x5253554d
ID_OFF = 48            # 统一落位（D2）
DL_ID_OFF = 16         # 仅 41
MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
DIST = os.path.join(REPO, "dist")
EXE = os.path.join(DIST, "Server.exe")
sys.path.insert(0, os.path.join(REPO, "project-docs", "evidence", "_harness"))
import server_ctl
_SRV = server_ctl.ServerCtl(EXE, DIST, HOST, PORT)
U = "e2etx1"
DST = f"./filesys/{U}/tx1"
SHA = lambda b: hashlib.sha256(b).hexdigest()
NONCE = f"tx{int(time.time())}".encode()   # 群上传按内容去重：载荷带随机前缀避免跨轮命中

def f32(s): return s.encode("utf-8")[:31].ljust(32, b"\0")
def creds(n, p="123456"): return f32(n) + hashlib.md5(p.encode()).hexdigest().encode()
def pdu(t, msg=b"", dat=b""):
    dat = dat[:64].ljust(64, b"\0")
    return struct.pack("<III", HDR + len(msg), len(msg), t) + dat + msg
def u16(x): return struct.pack("<H", x)
def u32(x): return struct.pack("<I", x)
def u64(x): return struct.pack("<Q", x)
def i64(x): return struct.pack("<q", x)
def string(x): return u32(len(x)) + x
def u32at(b, o): return struct.unpack("<I", b[o:o + 4])[0]
def u64at(b, o): return struct.unpack("<Q", b[o:o + 8])[0]
def i64at(b, o): return struct.unpack("<q", b[o:o + 8])[0]
def eid(dat, off=ID_OFF):
    v = u64at(dat, off)
    return v - (1 << 64) if v >= (1 << 63) else v
def envelope(gid, body=b""):
    return u16(1) + u16(20) + u64(int(time.time_ns() & ((1 << 63) - 1))) + u64(gid) + body

class Conn:
    def __init__(s):
        s.sock = socket.create_connection((HOST, PORT), timeout=10)
        s.buf = b""; s.pending = []
    def pump(s, t):
        s.sock.settimeout(t)
        try:
            d = s.sock.recv(65536)
            if d: s.buf += d
        except (socket.timeout, OSError):
            pass
    def packets(s):
        out = []
        while len(s.buf) >= HDR:
            tl, ml, ty = struct.unpack("<III", s.buf[:12])
            if tl < HDR or tl != HDR + ml or len(s.buf) < tl: break
            out.append((ty, s.buf[12:76], s.buf[76:tl])); s.buf = s.buf[tl:]
        s.pending.extend(out)
        return out
    def wait(s, ty, timeout=8.0):
        dl = time.time() + timeout
        while time.time() < dl:
            for i, p in enumerate(s.pending):
                if p[0] == ty:
                    return s.pending.pop(i)
            s.pump(0.2)
            s.packets()
        return None
    def take(s, t=0.1):
        """抽取当前全部待处理包（含此前 wait 期间已到达但未消费者）"""
        s.pump(t); s.packets()
        out = s.pending[:]; s.pending = []
        return out
    def send(s, pkt): s.sock.sendall(pkt)
    def close(s):
        try: s.sock.close()
        except OSError: pass

def sql(q):
    return subprocess.run([MYSQL] + DB + [q], capture_output=True, text=True).stdout.strip()

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
    c.send(pdu(T_CAP)); r = c.wait(T_CAP_R)
    code = r[2].split(b"\0")[0] if r else b""
    c.send(pdu(T_REG, msg=code + b"\0", dat=creds(name))); c.wait(T_REG + 1, 4)
    c.close()
    c = Conn()
    c.send(pdu(T_CAP)); r = c.wait(T_CAP_R); code = r[2].split(b"\0")[0] if r else b""
    c.send(pdu(T_LOGIN, msg=code + b"\0", dat=creds(name))); r = c.wait(T_LOGIN_R, 6)
    ret = struct.unpack("<i", r[1][:4])[0] if r else -99
    return c, ret

# ---------- 传输原语 ----------
def up_init(c, tid, dirPath, name, size):
    c.send(pdu(T_UPINIT, msg=dirPath.encode(),
               dat=f32(name) + i64(size) + u32(MAGIC) + b"\0" * 4 + u64(tid)))
    return c.wait(T_UPINIT_R)

def up_chunk(c, tid, off, data):
    c.send(pdu(T_UPDATA, msg=data,
               dat=i64(off) + b"\0" * 32 + u32(MAGIC) + b"\0" * 4 + u64(tid)))
    return c.wait(T_UPDATA_R)

def upload(c, tid, dirPath, name, payload, chunk=200):
    r = up_init(c, tid, dirPath, name, len(payload))
    if r is None or r[1][0] == 0: return False
    for i in range(0, len(payload), chunk):
        a = up_chunk(c, tid, i, payload[i:i + chunk])
        if a is None or a[1][0] == 0: return False
    return True

def abort(c, tid):
    c.send(pdu(T_ABORT, dat=b"\0" * ID_OFF + u64(tid)))
    return c.wait(T_ABORT_R)

def dl_req(c, tid, path, name):
    c.send(pdu(T_DOWN, msg=path.encode(), dat=f32(name) + b"\0" * 16 + u64(tid)))
    return c.wait(T_DOWN_R)

def dl_req_raw(c, tid, path, name):
    """只发出 40，不读取——用于需要在多条流同时"在途"时断言上限的场景"""
    c.send(pdu(T_DOWN, msg=path.encode(), dat=f32(name) + b"\0" * 16 + u64(tid)))

def collect(c, ids, timeout=25.0):
    got = {i: {"data": b"", "finish": None, "ok": None} for i in ids}
    dl = time.time() + timeout
    while time.time() < dl:
        for ty, dat, msg in c.take(0.2):
            tid = eid(dat, DL_ID_OFF if ty == T_DOWN_R else ID_OFF)
            if tid not in got: continue
            if ty == T_DOWN_R: got[tid]["ok"] = dat[0] != 0
            elif ty == T_DATA: got[tid]["data"] += msg
            elif ty == T_FINISH: got[tid]["finish"] = dat[0] != 0
        if all(v["finish"] is not None for v in got.values()): break
    return got

def gf_init(c, tid, gid, size, sha, name):
    c.send(pdu(T_GF_INIT, msg=envelope(gid, u64(size) + string(sha.encode()) + string(name.encode())),
               dat=b"\0" * ID_OFF + u64(tid)))
    return c.wait(T_GF_INIT_R)

def gf_records(c, gid):
    c.send(pdu(T_GF_LIST, msg=envelope(gid, u32(0) + u16(200))))
    r = c.wait(T_GF_LIST_R)
    if r is None: return []
    msg = r[2]; n = u32at(msg, 24); off = 32; out = []
    for _ in range(n):
        gfid = u64at(msg, off); off += 24
        ul = u32at(msg, off); off += 4 + ul
        nl = u32at(msg, off); nm = msg[off + 4:off + 4 + nl].decode("utf-8", "replace"); off += 4 + nl
        out.append({"id": gfid, "name": nm})
    return out

def mkdir(c, parent, name):
    c.send(pdu(T_MKDIR, msg=parent.encode(), dat=f32(name)))
    return c.wait(T_MKDIR_R)

# ================= 静态：§9 批准稿（I6） =================
print("=== 静态：冻结面 §9 ===")
def read(p): return open(os.path.join(REPO, p), encoding="utf-8").read()
def blk(txt):
    m = re.search(r"// ===== REQ-EXT-TRANSFER-001：transfer_id 落位.*?MAX_PARALLEL_DOWNLOADS = 3;.*\n", txt, re.S)
    return m.group(0) if m else None
cb, sb = blk(read("Client/protocol.h")), blk(read("Server/protocol.h"))
check("P1-frozen-block", "两端 protocol.h 新增常量块逐字节一致", cb is not None and cb == sb, f"len={len(cb or '')}")
vals = {}
for p in ("Client/protocol.h", "Server/protocol.h"):
    body = read(p).split("enum ENUM_MSG_TYPE {", 1)[1].split("};", 1)[0]
    nxt = 0
    for line in body.splitlines():
        line = line.split("//")[0].strip().rstrip(",")
        m = re.match(r"(ENUM_MSG_TYPE_[A-Za-z_]+)\s*(?:=\s*(\d+))?$", line)
        if not m: continue
        nxt = int(m.group(2)) if m.group(2) else nxt + 1
        vals.setdefault(m.group(1), set()).add(nxt)
check("P2-107-108", "ABORT 枚举 = 107/108 且两端一致",
      vals.get("ENUM_MSG_TYPE_UPLOADFILE_ABORT_REQUEST") == {107}
      and vals.get("ENUM_MSG_TYPE_UPLOADFILE_ABORT_RESPOND") == {108}, "")
check("P3-no-renumber", "旧编号未被重编号",
      all(vals.get(n) == {v} for n, v in [("ENUM_MSG_TYPE_UPLOADFILE_INIT_REQUEST", 29),
       ("ENUM_MSG_TYPE_UPLOADFILE_DATA_REQUEST", 31), ("ENUM_MSG_TYPE_DOWNLOADFILE_REQUEST", 40),
       ("ENUM_MSG_TYPE_DOWNLOADFILE_DATA", 42), ("ENUM_MSG_TYPE_DOWNLOADFILE_FINISH", 43),
       ("ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_REQUEST", 101), ("ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_REQUEST", 105),
       ("ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_RESPOND", 106)]), "")

# ================= 环境准备 =================
print("=== 环境准备 ===")
if not start_server():
    print("FATAL: Server 启动失败"); sys.exit(2)
for tbl in ("user_file", "upload_session"):
    sql(f"DELETE FROM {tbl} WHERE owner_name='{U}';")
sql(f"DELETE FROM group_file WHERE uploader_name='{U}';")
sql(f"DELETE FROM group_member WHERE user_name='{U}';")
sql(f"DELETE FROM group_info WHERE creator_name='{U}';")
# staging 令牌按 (owner|parent|display|size) 确定性派生，跨轮残留会让续传起点失真 → 清空
shutil.rmtree(os.path.join(DIST, "filesys", ".staging"), ignore_errors=True)

c, ret = login_conn(U)
check("E0", "登录成功", ret == 1, f"ret={ret}")
if ret != 1: sys.exit(2)
check("E0b", "建测试目录", mkdir(c, f"./filesys/{U}", "tx1") is not None, DST)

c.send(pdu(T_GCREATE, msg=envelope(0, string(b"tx-group"))))
r = c.wait(T_GCREATE_R)
GID = u64at(r[2], 24) if r and u32at(r[2], 20) == 0 else 0
check("E1", "建群成功", GID > 0, f"gid={GID}")

# ================= T-01：3 路个人上传并发（交错分块） =================
print("=== T-01 ===")
p1, p2, p3 = b"A" * 1500, b"B" * 700, b"C" * 2300
upids = [11, 12, 13]
ok = True
for i, n, p in zip(upids, "abc", (p1, p2, p3)):
    r = up_init(c, i, DST, f"t01_{n}.bin", len(p))
    ok = ok and r is not None and r[1][0] != 0
check("T-01a", "3 路 INIT 均 ret=true", ok, "")
payloads = {11: p1, 12: p2, 13: p3}
off = {i: 0 for i in upids}; complete = {i: False for i in upids}
while not all(complete.values()):
    for i in upids:
        if complete[i]: continue
        ch = payloads[i][off[i]:off[i] + 200]
        a = up_chunk(c, i, off[i], ch)
        if a is None or a[1][0] == 0 or eid(a[1]) != i:
            complete[i] = True; ok = False; break
        off[i] += len(ch)
        if a[1][16] != 0: complete[i] = True
check("T-01b", "3 路交错上传全部完成且无串扰", ok and all(off[i] == len(payloads[i]) for i in upids), f"{off}")
for i, n in zip((21, 22, 23), "abc"):
    dl_req(c, i, f"{DST}/t01_{n}.bin", f"t01_{n}.bin")
got = collect(c, [21, 22, 23])
check("T-01c", "3 条 entry 内容与源逐字节一致",
      all(got[i]["finish"] and SHA(got[i]["data"]) == SHA(payloads[j])
          for i, j in zip((21, 22, 23), (11, 12, 13))),
      f"lens={[len(got[i]['data']) for i in (21,22,23)]}")

# ================= T-19：客户端握手保真（97→98→29→30→31/32；AUD-1 回归锚点） =================
# T-01/T-03 直发裸 29，绕过真实客户端的 97 秒传预检查，因此看不见 AUD-1（97 在 m_uploads[0]
# 留下的占位被 activeUploadCount() 计入 → 第 3 路新个人上传 INIT 被误拒）。本用例按
# transfertask.cpp/file.cpp 的真实序列交错发送：97#1,29#1, 97#2,29#2, 97#3,29#3。
print("=== T-19 ===")
FASTCHECK_MAGIC = 0x4643484b

def fastcheck(c, sha, name, dirPath, size, reqId):
    c.send(pdu(T_FC, msg=sha.encode() + name.encode() + dirPath.encode() + reqId.encode(),
               dat=u32(1) + u32(FASTCHECK_MAGIC) + i64(size) + u32(64)
                   + u32(len(name.encode())) + u32(len(dirPath.encode())) + u32(len(reqId.encode()))))
    return c.wait(T_FC_R, 10)

c19, _ = login_conn(U)
t19 = [(901, "t19_a.bin", NONCE + b"T19" + b"A" * 900),
       (902, "t19_b.bin", NONCE + b"T19" + b"B" * 1300),
       (903, "t19_c.bin", NONCE + b"T19" + b"C" * 500)]
fc_ret, init_ret = [], []
for i, n, pl in t19:
    r = fastcheck(c19, SHA(pl), n, DST, len(pl), f"{int(time.time_ns()) % 10**16:016x}{i:016x}")
    fc_ret.append(r is not None and r[1][0] != 0 and r[1][1] == 0)   # ret=true 且未命中
    r2 = up_init(c19, i, DST, n, len(pl))
    init_ret.append(r2 is not None and r2[1][0] != 0)
check("T-19a", "3 路 97→98 均 ret=true 且未命中（hit=false）", all(fc_ret), f"{fc_ret}")
check("T-19b", "3 路 29→30 均 ret=true（AUD-1 回归：97 占位不得吞上传名额）",
      init_ret == [True, True, True], f"{init_ret}")
pl19 = {i: pl for i, _, pl in t19}
off19 = {i: 0 for i, _, _ in t19}
done19 = {i: False for i, _, _ in t19}
ok19 = True
while not all(done19.values()):
    for i, _, _ in t19:
        if done19[i]: continue
        ch = pl19[i][off19[i]:off19[i] + 300]
        a = up_chunk(c19, i, off19[i], ch)
        if a is None or a[1][0] == 0 or eid(a[1]) != i:
            ok19 = False; done19[i] = True; break
        off19[i] += len(ch)
        if a[1][16] != 0: done19[i] = True
for i, n, _ in t19:
    dl_req(c19, i + 100, f"{DST}/{n}", n)
got19 = collect(c19, [1001, 1002, 1003])
check("T-19c", "3 条 entry 内容与源逐字节一致（握手保真下无串扰）",
      ok19 and all(got19[i + 100]["finish"] and SHA(got19[i + 100]["data"]) == SHA(pl19[i])
                   for i, _, _ in t19),
      f"lens={[len(got19[i + 100]['data']) for i, _, _ in t19]}")
c19.close()

# ================= T-03：上传硬上限 + 释放后补位（D7/P4） =================
print("=== T-03 ===")
c2, _ = login_conn(U)
lim = []
for i in (31, 32, 33, 34):
    r = up_init(c2, i, DST, f"t03_{i}.bin", 400)
    lim.append(r is not None and r[1][0] != 0)
check("T-03a", "前 3 个 INIT ret=true，第 4 个 ret=false（不建状态）", lim == [True, True, True, False], f"{lim}")
b = up_chunk(c2, 34, 0, b"x" * 100)
check("T-03b", "被拒 id 的 31 → 32{ok=false}（未知 id 不回退）", b is not None and b[1][0] == 0, "")
r = abort(c2, 31)
check("T-03c", "107 释放槽位 ret=true 且回带 id", r is not None and r[1][0] != 0 and eid(r[1]) == 31, "")
r = up_init(c2, 35, DST, "t03_35.bin", 400)
check("T-03d", "释放后第 4 个 INIT 成功且回带 id", r is not None and r[1][0] != 0 and eid(r[1]) == 35, "")
for i in (32, 33, 35): abort(c2, i)
c2.close()

# ================= T-17：重复 id 冲突 =================
print("=== T-17 ===")
c3, _ = login_conn(U)
a = up_init(c3, 41, DST, "t17.bin", 400)
b = up_init(c3, 41, DST, "t17_other.bin", 800)
check("T-17a", "对活跃 id 重发 INIT → ret=false", a[1][0] != 0 and b[1][0] == 0, "")
ch = up_chunk(c3, 41, 0, b"z" * 200)
check("T-17b", "原上下文未被覆盖（仍可写入）", ch is not None and ch[1][0] != 0, "")
abort(c3, 41)

# ================= T-04：32 响应归属（M1/P1） =================
print("=== T-04 ===")
pa, pb = b"P" * 900, b"Q" * 600
up_init(c3, 51, DST, "t04a.bin", len(pa)); up_init(c3, 52, DST, "t04b.bin", len(pb))
seq = True
for i, pl in ((51, pa), (52, pb)):
    for o in range(0, len(pl), 200):
        r = up_chunk(c3, i, o, pl[o:o + 200])
        if r is None or r[1][0] == 0 or eid(r[1]) != i or i64at(r[1], 8) != o + len(pl[o:o + 200]):
            seq = False; break
check("T-04", "逐 id 的 nextOffset 单调、无跳变、无串扰", seq, "")

# ================= T-05：个人 + 群上传并发 =================
print("=== T-05 ===")
gp, pp = NONCE + b"G" * 1200, b"H" * 1000
gi = gf_init(c3, 61, GID, len(gp), SHA(gp), "t05_group.bin")
pi = up_init(c3, 62, DST, "t05_personal.bin", len(pp))
check("T-05a", "群/个人 INIT 均成功", gi is not None and u32at(gi[2], 20) == 0 and pi[1][0] != 0, "")
if gi is not None and u32at(gi[2], 20) == 0:
    nxt = i64at(gi[2], 52)
    for o in range(int(nxt), len(gp), 300):
        up_chunk(c3, 61, o, gp[o:o + 300])
for o in range(0, len(pp), 250):
    up_chunk(c3, 62, o, pp[o:o + 250])
time.sleep(0.8)
recs = [x for x in gf_records(c3, GID) if x["name"] == "t05_group.bin"]
check("T-05b", "群引用落定（group_file_id 非 0）", len(recs) > 0 and recs[-1]["id"] > 0, f"{recs}")
check("T-05c", "个人上传同时落定",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{U}' AND display_name='t05_personal.bin'") == "1", "")

# ================= T-16：边界（0 字节 / 块整倍） =================
print("=== T-16 ===")
z = upload(c3, 71, DST, "t16_zero.bin", b"")
m = upload(c3, 72, DST, "t16_mult.bin", b"M" * 600, chunk=200)
check("T-16", "0 字节与块整倍文件均正确落定",
      z and m and sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{U}' "
                      "AND display_name IN ('t16_zero.bin','t16_mult.bin')") == "2", "")

# ================= T-09：个人 暂停→恢复 =================
print("=== T-09 ===")
tp = b"R" * 1000
up_init(c3, 81, DST, "t09.bin", len(tp))
for o in range(0, 600, 200):
    up_chunk(c3, 81, o, tp[o:o + 200])
r = abort(c3, 81)
check("T-09a", "暂停 107 → ret=true", r is not None and r[1][0] != 0, "")
r = up_init(c3, 82, DST, "t09.bin", len(tp))
offr = i64at(r[1], 32) if r else -1
check("T-09b", "30 回带 resumeOffset == 已收字节（前缀未重传）",
      r is not None and r[1][0] != 0 and offr == 600, f"offset={offr}")
for o in range(600, len(tp), 200):
    up_chunk(c3, 82, o, tp[o:o + 200])

# ================= T-10：群 暂停→恢复 =================
print("=== T-10 ===")
gq = NONCE + b"S" * 900
gi = gf_init(c3, 83, GID, len(gq), SHA(gq), "t10_group.bin")
nxt = int(i64at(gi[2], 52))
for o in range(nxt, nxt + 600, 200):
    up_chunk(c3, 83, o, gq[o:o + 200])
abort(c3, 83)
gi2 = gf_init(c3, 84, GID, len(gq), SHA(gq), "t10_group.bin")
nxt2 = i64at(gi2[2], 52) if gi2 else -1
check("T-10", "群 102 回带 nextOffset == 已收字节", gi2 is not None and u32at(gi2[2], 20) == 0 and nxt2 == 600, f"next={nxt2}")
for o in range(int(nxt2), len(gq), 200):
    up_chunk(c3, 84, o, gq[o:o + 200])

# ================= T-11：取消 =================
print("=== T-11 ===")
up_init(c3, 91, DST, "t11_cancel.bin", 500)
up_chunk(c3, 91, 0, b"K" * 200)
r = abort(c3, 91)
sid = sql(f"SELECT session_id FROM upload_session WHERE owner_name='{U}' AND display_name='t11_cancel.bin' ORDER BY session_id DESC LIMIT 1")
st = sql(f"SELECT status FROM upload_session WHERE session_id={sid}") if sid else ""
sp = sql(f"SELECT staging_path FROM upload_session WHERE session_id={sid}") if sid else ""
check("T-11a", "108 ret=true", r is not None and r[1][0] != 0, "")
check("T-11b", "会话置 expired", st == "expired", f"status={st}")
check("T-11c", "文件列表无新条目",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{U}' AND display_name='t11_cancel.bin'") == "0", "")
check("T-11d", "staging 文件仍在盘上", bool(sp) and os.path.exists(sp), f"staging={sp}")

# ================= T-12：未知 id 拒绝不回退 =================
print("=== T-12 ===")
r = abort(c3, 9999)
check("T-12a", "未知 id 的 107 → 108 ret=false", r is not None and r[1][0] == 0, "")
b = up_chunk(c3, 8888, 0, b"x" * 100)
check("T-12b", "未知 id 的 31 → 32 ok=false", b is not None and b[1][0] == 0, "")
c3.send(pdu(T_PING))
check("T-12c", "连接存活（心跳正常）", c3.wait(T_PONG, 3) is not None, "")

# ================= T-18：两方向预算独立（D6） =================
print("=== T-18 ===")
c4, _ = login_conn(U)
big18 = b"W" * (512 * 1024)
upload(c4, 111, DST, "t18_big.bin", big18, chunk=8192)
# 3 路上传保持活跃（声明 2000，先发 200）
up_ok = [up_init(c4, i, DST, f"t18_u{i}.bin", 2000)[1][0] != 0 for i in (101, 102, 103)]
for i in (101, 102, 103):
    up_chunk(c4, i, 0, b"u" * 200)
# 3 路下载同一大文件（>256KB 出站高水位 → 保持活跃）；先不读，保持 3 条流在途
for i in (121, 122, 123):
    dl_req_raw(c4, i, f"{DST}/t18_big.bin", "t18_big.bin")
extra_up = up_init(c4, 104, DST, "t18_u4.bin", 300)
dl_req_raw(c4, 124, f"{DST}/t18_big.bin", "t18_big.bin")
res41, data18, fin18 = {}, {121: b"", 122: b"", 123: b""}, {}
dl = time.time() + 20
while time.time() < dl and (len(res41) < 4 or len(fin18) < 3):
    for ty, dat, msg in c4.take(0.1):
        if ty == T_DOWN_R:
            res41[eid(dat, DL_ID_OFF)] = dat[0] != 0
        elif ty == T_DATA and eid(dat) in data18:
            data18[eid(dat)] += msg
        elif ty == T_FINISH and eid(dat) in data18:
            fin18[eid(dat)] = dat[0] != 0
check("T-18a", "3 上传 + 3 下载同时受理（各算各的预算）", all(up_ok), f"up={up_ok} r41={res41}")
check("T-18b", "上传占满时第 4 个上传被上传预算拒（下载不占上传名额）",
      extra_up is not None and extra_up[1][0] == 0, "")
check("T-18c", "下载占满时第 4 个下载被下载预算拒（41 ret=false）",
      res41.get(124) is False and all(res41.get(i) is True for i in (121, 122, 123)), f"r41={res41}")
# 完成 3 路上传（补发剩余块）
for i in (101, 102, 103):
    for o in range(200, 2000, 200):
        up_chunk(c4, i, o, b"u" * 200)
check("T-18d", "3 路下载内容完整（并发不受上传影响）",
      all(fin18.get(i) and SHA(data18[i]) == SHA(big18) for i in (121, 122, 123)),
      f"{[len(data18[i]) for i in (121,122,123)]}")
c4.close()

# ================= T-06：下载公平性（M4；3 流，见文件头 D-a） =================
print("=== T-06 ===")
c5, _ = login_conn(U)
big = b"B" * (512 * 1024)
sma = [b"s" * 1024, b"t" * 1024]
upload(c5, 201, DST, "t06_big.bin", big, chunk=4096)
for i in range(2):
    upload(c5, 202 + i, DST, f"t06_s{i}.bin", sma[i])
dl_req(c5, 211, f"{DST}/t06_big.bin", "t06_big.bin")
for i in range(2):
    dl_req(c5, 212 + i, f"{DST}/t06_s{i}.bin", f"t06_s{i}.bin")
got = {i: {"data": b"", "finish": None} for i in (211, 212, 213)}
order = []
dl = time.time() + 40
while time.time() < dl:
    for ty, dat, msg in c5.take(0.1):
        tid = eid(dat)
        if tid not in got: continue
        if ty == T_DATA: got[tid]["data"] += msg
        elif ty == T_FINISH:
            got[tid]["finish"] = dat[0] != 0; order.append(tid)
    if len(order) == 3: break
check("T-06a", "小文件的 43 均早于大文件的 43（round-robin 公平）",
      len(order) == 3 and order[-1] == 211 and sorted(order[:2]) == [212, 213], f"order={order}")
check("T-06b", "大文件数据完整", len(got[211]["data"]) == len(big) and SHA(got[211]["data"]) == SHA(big),
      f"{len(got[211]['data'])}")

# ================= T-07：并发下载按 id 归属 =================
print("=== T-07 ===")
pls = {301: b"1" * 3000, 302: b"2" * 4500, 303: b"3" * 1500}
for i, pl in pls.items():
    upload(c5, i, DST, f"t07_{i}.bin", pl)
for i in pls:
    dl_req(c5, i + 100, f"{DST}/t07_{i}.bin", f"t07_{i}.bin")
got = collect(c5, [401, 402, 403])
check("T-07", "3 路并发下载内容各自正确、无交叉",
      all(got[i + 100]["finish"] and SHA(got[i + 100]["data"]) == SHA(pls[i]) for i in pls),
      f"{[len(got[i+100]['data']) for i in pls]}")

# ================= T-08：心跳不被下载饿死（P5） =================
print("=== T-08 ===")
hb = b"H" * (1024 * 1024)
upload(c5, 501, DST, "t08_big.bin", hb, chunk=4096)
dl_req(c5, 511, f"{DST}/t08_big.bin", "t08_big.bin")
rt = []
dl = time.time() + 20
while time.time() < dl and len(rt) < 6:
    c5.take(0.0)
    t0 = time.time()
    c5.send(pdu(T_PING))
    p = c5.wait(T_PONG, 5)
    if p is not None: rt.append(time.time() - t0)
maxrtt = max(rt) if rt else 99
check("T-08", "下载期间 PONG 往返均在阈值内（控制消息不被饿死）",
      len(rt) >= 4 and maxrtt < 2.0, f"n={len(rt)} max={maxrtt:.3f}s")

# ================= T-15：id=0 单流全流程（回归锚点） =================
print("=== T-15 ===")
c6, _ = login_conn(U)
leg = b"L" * 1200
ok = upload(c6, 0, DST, "t15_legacy.bin", leg)
dl_req(c6, 0, f"{DST}/t15_legacy.bin", "t15_legacy.bin")
got = collect(c6, [0])
check("T-15", "id=0 的 29/31/40/42/43 全流程与今日一致",
      ok and got[0]["finish"] and SHA(got[0]["data"]) == SHA(leg), f"len={len(got[0]['data'])}")

# ================= T-14：断线清理 =================
print("=== T-14 ===")
c7, _ = login_conn(U)
up_init(c7, 601, DST, "t14_a.bin", 800); up_chunk(c7, 601, 0, b"a" * 200)
up_init(c7, 602, DST, "t14_b.bin", 800)
dl_req(c7, 611, f"{DST}/t15_legacy.bin", "t15_legacy.bin")
time.sleep(0.4)
c7.close()
time.sleep(1.2)
c8, _ = login_conn(U)
okn = upload(c8, 621, DST, "t14_after.bin", b"N" * 500)
recs = gf_records(c8, GID)
check("T-14", "断线后重连新传输正常（旧上下文已清理）", okn and recs is not None, "")
c8.close()

# ================= T-13：空闲回收（D5） =================
print("=== T-13 ===（约 80s）")
c9, _ = login_conn(U)
for i in (701, 702, 703):
    up_init(c9, i, DST, f"t13_{i}.bin", 800)
r = up_init(c9, 704, DST, "t13_704.bin", 800)
check("T-13a", "3 槽占满时第 4 个 INIT 被拒", r is not None and r[1][0] == 0, "")
# T-23（N-1 回归锚点，复用本用例 82s 静默窗口，零额外耗时）：97 未命中的 id=0 纯占位建槽后
# 必须起空闲计时，静默 >60s 由 D5 回收——否则残留暂存槽/会话行只能等 3600s TTL。
_pl23 = NONCE + b"T23" + b"N" * 600
_r23 = fastcheck(c9, SHA(_pl23), "t23_n1.bin", DST, len(_pl23), "d4" * 16)
_q23 = ("SELECT status FROM upload_session WHERE owner_name='"
        + U + "' AND display_name='t23_n1.bin' ORDER BY session_id DESC LIMIT 1")
_st23_before = sql(_q23)
time.sleep(82)
r = up_init(c9, 704, DST, "t13_704.bin", 800)
st = sql(f"SELECT COUNT(*) FROM upload_session WHERE owner_name='{U}' AND status='expired'")
check("T-13b", "静默 > 60s 后槽位回收（第 4 个 INIT 成功）", r is not None and r[1][0] != 0, "")
check("T-13c", "被回收会话置 expired", int(st) >= 3, f"expired={st}")
_st23_after = sql(_q23)
check("T-23", "97 纯占位静默 >60s 被空闲回收（N-1 回归锚点）",
      _r23 is not None and _r23[1][0] != 0 and _r23[1][1] == 0
      and _st23_before == "uploading" and _st23_after == "expired",
      f"97ret={_r23[1][0] if _r23 else None} before={_st23_before} after={_st23_after}")
abort(c9, 704)
c9.close()

# ================= T-20：107 × 在途写竞态（AUD-4 回归锚点） =================
# 断言不变量：任意时刻 staging 文件大小 == upload_session.confirmed_offset（= 服务端已确认字节）。
# 触发窗口：31 发出后**不等 32 ack** 立刻发 107，使 107 可能落在 UploadWriteTask 写盘在途期间。
# 无 AUD-4 修复时该不变量必失败（confirmed_offset 停留 INIT 值、暂存被在途写越尾）。
print("=== T-20 ===")
c20, _ = login_conn(U)
t20_hits, t20_db_ok, t20_resume_ok = 0, [], []
for k in range(6):
    nm = f"t20_{k}.bin"
    rid = 860 + k
    total = 120000
    up_init(c20, rid, DST, nm, total)
    up_chunk(c20, rid, 0, b"P" * 1000)                     # 先落一块并等 ack（receivedSize=1000）
    # 在途写 + 立即 107：大块（60KB）拉长写盘时间，107 紧随其后无等待
    c20.send(pdu(T_UPDATA, msg=b"Q" * 60000,
                 dat=i64(1000) + b"\0" * 32 + u32(MAGIC) + b"\0" * 4 + u64(rid)))
    c20.send(pdu(T_ABORT, dat=b"\0" * ID_OFF + u64(rid)))
    r = c20.wait(T_ABORT_R, 10)
    t20_hits += 1 if (r is not None and r[1][0] != 0) else 0
    time.sleep(0.6)                                        # 等在途写落定 + 延迟释放收尾
    row = sql(f"SELECT confirmed_offset, staging_path FROM upload_session WHERE owner_name='{U}' "
              f"AND display_name='{nm}' ORDER BY session_id DESC LIMIT 1")
    if "\t" in row:
        off_s, sp = row.split("\t", 1)
        size = os.path.getsize(sp) if os.path.exists(sp) else -1
        t20_db_ok.append(int(off_s) == size and size >= 0)
    else:
        t20_db_ok.append(False)
    # 续传起点（30 回带）必须等于暂存大小——服务端 beginStaged 与暂存一致
    r2 = up_init(c20, rid + 100, DST, nm, total)
    roff = i64at(r2[1], 32) if r2 else -1
    sz2 = os.path.getsize(sp) if "\t" in row and os.path.exists(sp) else -1
    t20_resume_ok.append(r2 is not None and r2[1][0] != 0 and roff == sz2)
    abort(c20, rid + 100)
check("T-20a", "107 在途写竞态×6：108 均 ret=true", t20_hits == 6, f"hits={t20_hits}")
check("T-20b", "107 后 upload_session.confirmed_offset == 暂存物理大小（AUD-4 不变量）",
      all(t20_db_ok) and len(t20_db_ok) == 6, f"{t20_db_ok}")
check("T-20c", "续传 30 回带的 resumeOffset == 暂存物理大小", all(t20_resume_ok), f"{t20_resume_ok}")
c20.close()

# ================= T-21：98 回带请求标识（AUD-3 服务端契约锚点） =================
# 客户端按 98 回带的标识归属迟到响应；服务端必须逐字节回带 97 的 reqId，且不改变既有
# nameLen/名字段（msg[:nameLen] 仍是服务端最终文件名；回带标识追加在其后）。
print("=== T-21 ===")
c21, _ = login_conn(U)
def fc_raw(c, sha, name, dirPath, size, reqId):
    c.send(pdu(T_FC, msg=sha.encode() + name.encode() + dirPath.encode() + reqId.encode(),
               dat=u32(1) + u32(FASTCHECK_MAGIC) + i64(size) + u32(64)
                   + u32(len(name.encode())) + u32(len(dirPath.encode())) + u32(len(reqId.encode()))))
    return c.wait(T_FC_R, 10)
def fc_tail(r, rid_hex):
    """返回 (标识逐字节相符, 名字段是否被标识污染)"""
    nl = u32at(r[1], 32)
    if len(r[2]) < nl:
        return False, True
    return r[2][nl:].decode("latin-1") == rid_hex, (len(r[2]) != nl + len(rid_hex))
# (1) 未命中路径：98 无名字（nameLen=0），标识占满 caMsg
t21_miss = []
for n, p, rh in ((901, b"W" * 700, "a1" * 16), (902, b"W" * 900, "b2" * 16)):
    r = fc_raw(c21, SHA(p), f"t21_{n}.bin", DST, len(p), rh)
    ok, dirty = fc_tail(r, rh) if r else (False, True)
    t21_miss.append(r is not None and r[1][0] != 0 and r[1][1] == 0 and ok and not dirty)
check("T-21a", "未命中 98：回带标识逐字节相符且不污染既有字段", all(t21_miss), f"{t21_miss}")
# (2) 命中路径：先整文件上传落定，再用同 sha 发 97 → hit=true，名字段仍是落定名，标识追加其后
pl21 = NONCE + b"T21HIT" + b"H" * 800
nm21 = "t21_hit.bin"
up21 = upload(c21, 905, DST, nm21, pl21, chunk=400)
r = fc_raw(c21, SHA(pl21), nm21, DST, len(pl21), "c3" * 16)
if r:
    nl = u32at(r[1], 32)
    nm_ok = r[2][:nl].decode("utf-8", "replace") == nm21
    tok_ok = r[2][nl:].decode("latin-1") == "c3" * 16
else:
    nm_ok = tok_ok = False
check("T-21b", "命中 98：nameLen/名字段语义不变，回带标识紧随其后（附加上限=不破坏旧读法）",
      up21 and r is not None and r[1][0] != 0 and r[1][1] != 0 and nm_ok and tok_ok,
      f"upload={up21} nameLen={u32at(r[1], 32) if r else -1}")
c21.close()

# ================= T-22：客户端本地队列契约（静态；AUD-2/F-3） =================
# 本仓无客户端测试宿主（pipeline/test.ps1 注册 0 单测；Client 为 GUI 进程无 headless 入口），
# 故客户端队列只能做**静态契约**断言——动态"第 4 个不发 29"仍需 §6.3 人工项。
# 服务端等价面由 T-03（上限拒绝 + 释放后补位）与 T-19（真实握手）覆盖。
print("=== T-22 ===")
_tt = os.path.join(REPO, "Client", "transfertask.cpp")
src = open(_tt, encoding="utf-8").read() if os.path.exists(_tt) else ""
gate = re.search(r"while \(m_activeUploads < int\(MAX_PARALLEL_UPLOADS\) && !m_uploadQueue\.isEmpty\(\)\)", src)
requeue = re.search(r"t->state = Queued;\s*\n\s*t->needFastcheck = false;\s*\n\s*t->failReason\.clear\(\);\s*\n\s*m_uploadQueue\.append\(t->key\);", src)
nofail_immediate = "服务端拒绝上传初始化" in src and "m_activeUploads > 0" in src
check("T-22a", "pump() 以 MAX_PARALLEL_UPLOADS 为闸且只启动 Queued（静态）", gate is not None, "")
check("T-22b", "INIT 被拒 → 回队（needFastcheck=false，事件驱动重试，非 Failed）（静态）",
      requeue is not None and nofail_immediate, "")

# ================= 汇总 =================
print("\n=== 汇总 ===")
okn = sum(1 for _, _, cc in results if cc)
for sid, desc, cc in results:
    if not cc: print(f"  FAIL -> {sid} {desc}")
print("NOTE: 纯客户端行为（多选选择器/任务列表/暂停继续取消按钮/**客户端队列不发起第 4 个 29**/"
      "大文件摘要不卡 UI）属 design §6.3 人工清单，非本套件可断言范围；"
      "wire 级握手保真（T-19：97→98→29→30→31/32）与客户端队列的**静态契约**（T-22）已由本套件覆盖，"
      "但客户端队列的**动态**断言（第 4 个 29 确实未发出）仍需人工项——本仓无客户端测试宿主。")
# NOTE 必须打印在汇总行之前：test-gate.py 以输出中**最后一个** `\d+/\d+` 作为断言行数，
# NOTE 里的 "31/32" 会盖掉真正的 "54/54"（实测 observed 31/declared 54 的假红）。
print(f"{okn}/{len(results)} passed")
stop_server()
sys.exit(0 if okn == len(results) else 1)
