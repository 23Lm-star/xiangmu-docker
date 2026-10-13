# REQ-EXT-GROUP-003 群文件目录与访问控制：验收 1-6 端到端脚本。
# 前置：DB 已执行 Server/migrate_ext_group_003.sql（schema_version=2026100103）。
# 断言三处：网络 PDU（99-106 与既有 31/32、42/43）、协议静态一致性、DB（group_file/file_object/user_file）。
#
# ---- 统一 e2e 运行形态（见 project-docs/evidence/E2E-RUNFORM.md）----
# 运行：py -3.13 project-docs/evidence/REQ-EXT-GROUP-003/groupfile_e2e.py   （cwd 任意）
#   exe <repo>/dist/Server.exe，cwd=<repo>/dist，filesPath=./filesys，脚本自管理 Server 生命周期。
import hashlib, os, re, socket, struct, subprocess, sys, time

HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_REG, T_REG_R = 1, 2
T_LOGIN, T_LOGIN_R = 3, 4
T_CAP, T_CAP_R = 44, 45
T_UPDATA, T_UPDATA_R = 31, 32
T_DATA, T_FINISH = 42, 43
T_CREATE, T_CREATE_R = 60, 61
T_INVITE, T_INVITE_R = 62, 63
T_ACCEPT, T_ACCEPT_R = 64, 65
T_REMOVE, T_REMOVE_R = 70, 71
T_GF_LIST, T_GF_LIST_R = 99, 100
T_GF_INIT, T_GF_INIT_R = 101, 102
T_GF_DEL, T_GF_DEL_R = 103, 104
T_GF_DOWN, T_GF_DOWN_R = 105, 106
MAGIC = 0x5253554d
PWD = "123456"
MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
OWNER, MEMB, OUTS = "gf3owner", "gf3memb", "gf3outs"
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
DIST = os.path.join(REPO, "dist")
EXE = os.path.join(DIST, "Server.exe")
sys.path.insert(0, os.path.join(REPO, "project-docs", "evidence", "_harness"))
import server_ctl
_SRV = server_ctl.ServerCtl(EXE, DIST, HOST, PORT)
SBX = os.path.join(DIST, "filesys")

P1 = b"GROUPFILE-REQ003-ALPHA-" * 50
P2 = b"GROUPFILE-REQ003-BETA-" * 40
S1, S2 = (hashlib.sha256(p).hexdigest() for p in (P1, P2))
TEST_SHAS = ",".join("'%s'" % s for s in (S1, S2))
GROUPLIKE = "gf3%"

def f32(s): return s.encode("utf-8")[:31].ljust(32, b"\0")
def creds(n, p=PWD): return f32(n) + hashlib.md5(p.encode()).hexdigest().encode()
def pdu(t, msg=b"", dat=b""):
    return struct.pack("<III", HDR + len(msg), len(msg), t) + dat[:64].ljust(64, b"\0") + msg
def u16(x): return struct.pack("<H", x)
def u32(x): return struct.pack("<I", x)
def u64(x): return struct.pack("<Q", x)
def string(x):
    b = x.encode() if isinstance(x, str) else x
    return u32(len(b)) + b
def envelope(gid=0, body=b""):
    return u16(1) + u16(20) + u64(int(time.time_ns() & ((1 << 63) - 1))) + u64(gid) + body
def u32at(b, o): return struct.unpack("<I", b[o:o + 4])[0]
def u64at(b, o): return struct.unpack("<Q", b[o:o + 8])[0]
def code_of(d): return u32at(d, 20) if len(d) >= 24 else None   # 信封20 + code u32
def as_signed(x): return x - (1 << 32) if x >= (1 << 31) else x

class Conn:
    def __init__(s):
        s.s = socket.create_connection((HOST, PORT), timeout=10); s.buf = b""
    def pump(s, t=.4):
        s.s.settimeout(t)
        try:
            d = s.s.recv(65536)
            if d: s.buf += d
        except socket.timeout: pass
    def packets(s):
        out = []
        while len(s.buf) >= HDR:
            tl, ml, ty = struct.unpack("<III", s.buf[:12])
            if tl < HDR or tl != HDR + ml or len(s.buf) < tl: break
            out.append((ty, s.buf[12:76], s.buf[76:tl])); s.buf = s.buf[tl:]
        return out
    def ask(s, pkt, expected=None, timeout=5.0):
        s.s.sendall(pkt); got = []; dl = time.time() + timeout
        while time.time() < dl:
            s.pump(.3); got.extend(s.packets())
            if expected is not None and any(x[0] == expected for x in got): break
            if expected is None and got: break
        return got
    def close(s):
        try: s.s.close()
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
    r = c.ask(pdu(T_CAP), T_CAP_R); code = r[0][2].split(b"\0")[0] if r else b""
    c.ask(pdu(T_REG, msg=code + b"\0", dat=creds(name)), T_REG_R); c.close()
    c = Conn()
    r = c.ask(pdu(T_CAP), T_CAP_R); code = r[0][2].split(b"\0")[0] if r else b""
    r = c.ask(pdu(T_LOGIN, msg=code + b"\0", dat=creds(name)), T_LOGIN_R)
    ret = struct.unpack("<i", r[0][1][:4])[0] if r else -99
    return c, ret

def group(c, typ, rt, gid=0, body=b""):
    """发群信封请求，返回 (code_signed, full_payload_bytes)。"""
    ps = c.ask(pdu(typ, msg=envelope(gid, body)), rt)
    for ty, dat, msg in ps:
        if ty == rt:
            return as_signed(code_of(msg) or 0), msg
    return None, b""

def gf_init(c, gid, size, sha, name):
    return group(c, T_GF_INIT, T_GF_INIT_R, gid, u64(size) + string(sha) + string(name))

def gf_list(c, gid, offset=0, limit=200):
    return group(c, T_GF_LIST, T_GF_LIST_R, gid, u32(offset) + u16(limit))

def gf_delete(c, gid, gfid):
    return group(c, T_GF_DEL, T_GF_DEL_R, gid, u64(gfid))

def send_chunks(c, payload, start=0):
    for i in range(start, len(payload), 600):
        ch = payload[i:i + 600]
        a = c.ask(pdu(T_UPDATA, msg=ch, dat=struct.pack("<q", i) + b"\0" * 32 + struct.pack("<I", MAGIC)), T_UPDATA_R)
        if not a or a[0][0] != T_UPDATA_R or a[0][1][0] == 0: return False
    return True

def gf_upload(c, gid, payload, name):
    """101 预检查 + 未命中顺序分块。返回 (code, hit, group_file_id, session_id)。
    未命中时 102 的 group_file_id 恒为 0（尚未落定），完成后从列表回读引用。"""
    code, d = gf_init(c, gid, len(payload), hashlib.sha256(payload).hexdigest(), name)
    if code != 0: return code, None, 0, 0
    hit = d[24] != 0
    gfid = u64at(d, 36); sid = u64at(d, 44); nxt = u64at(d, 52)
    if not hit:
        if not send_chunks(c, payload, int(nxt)): return -1, None, 0, 0
        _, recs = gf_records(c, gid)
        found = [x for x in recs if x["name"] == name]
        gfid = found[-1]["id"] if found else 0
    return code, hit, gfid, sid

def gf_download(c, gid, gfid):
    """105 → 106 元数据 + 既有 42/43 正文。返回 (code, name, size, data, finish_ok)。"""
    c.s.sendall(pdu(T_GF_DOWN, msg=envelope(gid, u64(gfid))))
    code = None; name = ""; size = 0; data = b""; fin = None
    dl = time.time() + 10
    while time.time() < dl:
        c.pump(.3)
        for ty, dat, msg in c.packets():
            if ty == T_GF_DOWN_R:
                code = as_signed(code_of(msg) or 0)
                if code == 0:
                    size = u64at(msg, 24); nl = u32at(msg, 32); name = msg[36:36 + nl].decode("utf-8", "replace")
            elif ty == T_DATA:
                data += msg
            elif ty == T_FINISH:
                fin = dat[0] != 0
        if code is not None and (code != 0 or fin is not None): break
    return code, name, size, data, fin

def gf_records(c, gid):
    code, d = gf_list(c, gid)
    if code != 0: return code, []
    count = u32at(d, 24); total = u32at(d, 28); off = 32; recs = []
    for _ in range(count):
        gfid = u64at(d, off); size = u64at(d, off + 8); ts = u64at(d, off + 16); off += 24
        ul = u32at(d, off); up = d[off + 4:off + 4 + ul].decode(); off += 4 + ul
        nl = u32at(d, off); nm = d[off + 4:off + 4 + nl].decode(); off += 4 + nl
        recs.append({"id": gfid, "size": size, "ts": ts, "uploader": up, "name": nm})
    return code, recs

# ---------- 静态：协议块逐字节一致 + 仅追加 99-106 ----------
print("=== 协议静态检查 ===")
def block(path):
    txt = open(path, encoding="utf-8").read()
    m = re.search(r"// ===== REQ-EXT-GROUP-003：群文件 PDU 布局常量.*?GROUP_FILE_MAX_LIST_LIMIT.*?\n", txt, re.S)
    return m.group(0) if m else None
bc = block(os.path.join(REPO, "Client", "protocol.h"))
bs = block(os.path.join(REPO, "Server", "protocol.h"))
check("P1-same-block", "两端 protocol.h 群文件常量块逐字节一致", bc is not None and bc == bs, f"len={len(bc or '')}")
vals = {}
for path in (os.path.join(REPO, "Client", "protocol.h"), os.path.join(REPO, "Server", "protocol.h")):
    txt = open(path, encoding="utf-8").read()
    body = txt.split("enum ENUM_MSG_TYPE {", 1)[1].split("};", 1)[0]
    nxt = 0
    for line in body.splitlines():
        line = line.split("//")[0].strip().rstrip(",")
        m = re.match(r"(ENUM_MSG_TYPE_[A-Za-z_]+)\s*(?:=\s*(\d+))?$", line)
        if not m: continue
        nxt = int(m.group(2)) if m.group(2) else nxt + 1
        vals.setdefault(m.group(1), set()).add(nxt)
check("P2-new-enum", "群文件新枚举 = 99..106 且两端一致",
      all(vals.get(n) == {v} for n, v in [
          ("ENUM_MSG_TYPE_GROUP_FILE_LIST_REQUEST", 99), ("ENUM_MSG_TYPE_GROUP_FILE_LIST_RESPOND", 100),
          ("ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_REQUEST", 101), ("ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_RESPOND", 102),
          ("ENUM_MSG_TYPE_GROUP_FILE_DELETE_REQUEST", 103), ("ENUM_MSG_TYPE_GROUP_FILE_DELETE_RESPOND", 104),
          ("ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_REQUEST", 105), ("ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_RESPOND", 106)]),
      "99-106")
check("P3-no-renumber", "既有编号未重编号（抽样 1/15/31/40/88/98）",
      vals.get("ENUM_MSG_TYPE_REGIST_REQUEST") == {1} and vals.get("ENUM_MSG_TYPE_CHAT_REQUEST") == {15}
      and vals.get("ENUM_MSG_TYPE_UPLOADFILE_DATA_REQUEST") == {31}
      and vals.get("ENUM_MSG_TYPE_DOWNLOADFILE_REQUEST") == {40}
      and vals.get("ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_REQUEST") == {88}
      and vals.get("ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_RESPOND") == {98}, "ok")

# ---------- 环境准备 ----------
print("=== 环境准备 ===")
if not start_server():
    print("FATAL: Server 启动失败"); sys.exit(2)
for u in (OWNER, MEMB, OUTS):
    sql(f"DELETE FROM user_file WHERE owner_name='{u}';")
    sql(f"DELETE FROM upload_session WHERE owner_name='{u}';")
sql(f"DELETE gf FROM group_file gf JOIN file_object fo ON fo.object_id=gf.object_id WHERE fo.sha256 IN ({TEST_SHAS});")
sql(f"DELETE FROM group_file WHERE uploader_name LIKE '{GROUPLIKE}';")
sql(f"DELETE FROM user_file WHERE owner_name IN (SELECT name FROM user_info WHERE name LIKE '{GROUPLIKE}');")
sql(f"DELETE FROM file_object WHERE sha256 IN ({TEST_SHAS}) AND refcount<=0;")
sql(f"DELETE FROM file_object WHERE sha256 IN ({TEST_SHAS});")
sql("DELETE FROM group_invitation WHERE group_id IN (SELECT group_id FROM group_info WHERE creator_name LIKE 'gf3%');")
sql("DELETE FROM group_member WHERE user_name LIKE 'gf3%';")
sql("DELETE FROM group_info WHERE creator_name LIKE 'gf3%';")
check("E0", "环境隔离：本测试摘要无残留对象",
      sql(f"SELECT COUNT(*) FROM file_object WHERE sha256 IN ({TEST_SHAS})") == "0", "")

co, ret_o = login_conn(OWNER)
cm, ret_m = login_conn(MEMB)
cx, ret_x = login_conn(OUTS)
check("L0", "三方登录成功", ret_o == 1 and ret_m == 1 and ret_x == 1, f"{ret_o}/{ret_m}/{ret_x}")
if ret_o != 1 or ret_m != 1 or ret_x != 1: sys.exit(2)

code, d = group(co, T_CREATE, T_CREATE_R, 0, string("GF3 Group"))
GID = u64at(d, 24) if code == 0 else 0
check("E1", "建群成功", code == 0 and GID > 0, f"code={code} gid={GID}")
code, d = group(co, T_INVITE, T_INVITE_R, GID, string(MEMB) + string("gf3-i1"))
IID = u64at(d, 24) if code == 0 else 0
code, d = group(cm, T_ACCEPT, T_ACCEPT_R, 0, u64(IID))
check("E2", "成员加入成功", code == 0, f"code={code}")

# ---------- 验收1：成员可列出元数据；非成员失败 ----------
print("=== 验收1：列表元数据 / 非成员拒绝 ===")
code, recs0 = gf_records(cm, GID)
check("A1a-member-list", "成员可列群文件（空目录 code=0）", code == 0, f"code={code}")
code, _ = gf_list(cx, GID)
check("A1b-nonmember-denied", "非成员列表被拒（-2101）", code == -2101, f"code={code}")

# ---------- 验收2：上传出现逻辑记录；同内容复用物理对象；无越权引用 ----------
print("=== 验收2：上传落逻辑记录 / 同内容复用物理对象 ===")
code, hit, gf1, sid1 = gf_upload(cm, GID, P1, "alpha.bin")
check("A2a-upload-miss", "首次上传未命中并顺序分块完成", code == 0 and hit is False and gf1 > 0, f"code={code} hit={hit}")
check("A2a-gfrow", "群目录出现一条 active 逻辑记录",
      sql(f"SELECT COUNT(*) FROM group_file WHERE group_id={GID} AND status='active' AND display_name='alpha.bin'") == "1", "")
code, recs = gf_records(cm, GID)
r = next((x for x in recs if x["id"] == gf1), None)
check("A2a-meta", "记录含名称/大小/上传者/时间四元数据",
      r is not None and r["name"] == "alpha.bin" and r["size"] == len(P1)
      and r["uploader"] == MEMB and r["ts"] > 0, str(r))
check("A2a-personal", "上传者个人根同步落逻辑条目（文件标识）",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{MEMB}' AND status='active' AND display_name='alpha.bin'") == "1", "")

code, hit2, gf2, sid2 = gf_upload(cm, GID, P1, "alpha2.bin")
check("A2b-dedup-hit", "同内容异名 -> 秒传命中（hit=true）", code == 0 and hit2 is True and gf2 > 0, f"code={code} hit={hit2}")
check("A2b-single-object", "同内容复用同一物理对象",
      sql(f"SELECT COUNT(*) FROM file_object WHERE sha256='{S1}'") == "1", "")
refc = sql(f"SELECT refcount FROM file_object WHERE sha256='{S1}'")
ufc = sql(f"SELECT COUNT(*) FROM user_file WHERE status='active' AND object_id=(SELECT object_id FROM file_object WHERE sha256='{S1}')")
gfc = sql(f"SELECT COUNT(*) FROM group_file WHERE status='active' AND object_id=(SELECT object_id FROM file_object WHERE sha256='{S1}')")
check("A2b-refcount", "引用计数 = active user_file ∪ group_file 引用数", refc == str(int(ufc) + int(gfc)),
      f"refcount={refc} uf={ufc} gf={gfc}")

# ---------- 验收3：下载；退出/被移除后老+新连接全失效 ----------
print("=== 验收3：下载 / 移除后即时失权 ===")
code, nm, sz, data, fin = gf_download(cm, GID, gf1)
check("A3a-download", "成员下载成功且正文一致", code == 0 and fin is True and data == P1 and sz == len(P1),
      f"code={code} fin={fin} got={len(data)}/{len(P1)} name={nm}")

code, d = group(co, T_REMOVE, T_REMOVE_R, GID, string(MEMB))
check("A3b-remove-ok", "群主移除成员成功", code == 0, f"code={code}")
code, _ = gf_list(cm, GID)
check("A3c-oldconn-list", "被移除后老连接列表被拒（-2101）", code == -2101, f"code={code}")
code, d = gf_init(cm, GID, len(P1), S1, "x.bin")
check("A3c-oldconn-upload", "被移除后老连接上传被拒", code != 0, f"code={code}")
code, _, _, _, _ = gf_download(cm, GID, gf1)
check("A3c-oldconn-download", "被移除后老连接下载被拒", code != 0, f"code={code}")
code, d = gf_delete(cm, GID, gf1)
check("A3c-oldconn-delete", "被移除后老连接删除被拒", code != 0, f"code={code}")

cm2, _ = login_conn(MEMB)
code, _ = gf_list(cm2, GID)
check("A3d-newconn-list", "被移除后新连接列表被拒（-2101）", code == -2101, f"code={code}")
code, _, _, _, _ = gf_download(cm2, GID, gf1)
check("A3d-newconn-download", "被移除后新连接下载被拒", code != 0, f"code={code}")
cm2.close(); cm.close()   # cm 已被单点登录踢掉，后续用全新连接

# ---------- 验收4：删除授权 + 删群引用不影响他处 ----------
print("=== 验收4：删除授权 / 引用隔离 ===")
# 重新加回成员（新连接：旧连接已被 A3d 的登录踢下线）
code, d = group(co, T_INVITE, T_INVITE_R, GID, string(MEMB) + string("gf3-i2"))
IID2 = u64at(d, 24) if code == 0 else 0
cm3, _ = login_conn(MEMB)
code, d = group(cm3, T_ACCEPT, T_ACCEPT_R, 0, u64(IID2))
check("A4-setup", "成员重新加入", code == 0, f"code={code}")

code, owner_hit, gf_owner, _ = gf_upload(co, GID, P2, "owner.bin")
check("A4a-owner-upload", "群主上传成功", code == 0 and gf_owner > 0, f"code={code} hit={owner_hit}")

code, _ = gf_delete(cm3, GID, gf_owner)
check("A4b-member-cannot-delete-other", "普通成员不能删除他人文件", code != 0, f"code={code}")

code, hit3, gf_memb, _ = gf_upload(cm3, GID, P2, "memb.bin")
check("A4c-member-hit-upload", "同内容异名命中（成员上传）", code == 0 and hit3 is True and gf_memb > 0, f"code={code}")

code, _ = gf_delete(cm3, GID, gf_memb)
check("A4d-uploader-delete-own", "上传者可删除自己上传的群文件", code == 0, f"code={code}")

obj_before = sql(f"SELECT COUNT(*) FROM file_object WHERE sha256='{S2}'")
ref_before = sql(f"SELECT refcount FROM file_object WHERE sha256='{S2}'")
code, _ = gf_delete(co, GID, gf_owner)
obj_after = sql(f"SELECT COUNT(*) FROM file_object WHERE sha256='{S2}'")
ref_after = sql(f"SELECT refcount FROM file_object WHERE sha256='{S2}'")
check("A4e-owner-delete-any", "群主可删除任意群文件", code == 0, f"code={code}")
check("A4f-object-survives", "删除群引用不删除共享物理对象",
      obj_after == "1" and obj_before == "1", f"obj={obj_before}->{obj_after}")
check("A4f-refcount-dec", "删除群引用引用计数减一（不为 0）",
      int(ref_after) == int(ref_before) - 1, f"ref={ref_before}->{ref_after}")
check("A4g-group-row-removed", "被删群记录转 removed（软删）",
      sql(f"SELECT COUNT(*) FROM group_file WHERE group_file_id IN ({gf_memb},{gf_owner}) AND status='removed'") == "2", "")

# ---------- 验收5：路径/名称/归属负向面 ----------
print("=== 验收5：路径与越权负向面 ===")
code, _ = gf_delete(co, GID, 999999999)
check("A5-bogus-id", "伪造 group_file_id 被拒且不泄露存在性", code == -2103, f"code={code}")

# 群 A 的 group_file_id + 他群 group_id（同 B 群）-> 拒
code, d = group(co, T_CREATE, T_CREATE_R, 0, string("GF3 Other"))
GID2 = u64at(d, 24) if code == 0 else 0
code, hitX, gf_alpha, _ = gf_upload(co, GID2, P1, "otheralpha.bin")
code, _ = gf_delete(co, GID, gf_alpha)   # 用 A 群 group_id 删 B 群的文件
check("A5-cross-group", "跨群 group_file_id 一律拒绝（P4）", code == -2103, f"code={code}")

# 名称带 '/' -> 解析拒绝
code, d = gf_init(co, GID2, len(P2), S2, "a/b.bin")
check("A5-slash-name", "展示名含 '/' 被拒", code == -2002, f"code={code}")
# 超长名（300B）-> 拒绝
code, d = gf_init(co, GID2, len(P2), S2, "L" * 300)
check("A5-long-name", "超长展示名（300B）被拒", code == -2002, f"code={code}")
# 非法摘要（非 hex）
code, d = gf_init(co, GID2, len(P2), "z" * 64, "ok.bin")
check("A5-bad-sha", "非法摘要（非 [0-9a-f]）被拒", code == -2002, f"code={code}")
# 越界 limit
code, _ = gf_list(co, GID2, 0, 0)
check("A5-bad-limit-0", "limit=0 越界被拒", code == -2002, f"code={code}")
code, _ = gf_list(co, GID2, 0, 500)
check("A5-bad-limit-big", "limit=500 越界被拒", code == -2002, f"code={code}")
# 尾随字节（注入面：无路径字段可注入）-> 拒绝
trail = u64(len(P2)) + string(S2) + string("trail.bin") + b"/etc/passwd"
ps = co.ask(pdu(T_GF_INIT, msg=envelope(GID2, trail)), T_GF_INIT_R)
code = as_signed(code_of(ps[0][2])) if ps else None
check("A5-trailing-bytes", "载荷尾随字节（潜在路径注入）被拒", code == -2002, f"code={code}")

# 未认证连接：分派登记生效，返回类型正确且 code=-1001
raw = Conn()
ps = raw.ask(pdu(T_GF_LIST, msg=envelope(GID2, u32(0) + u16(10))), T_GF_LIST_R)
rawcode = as_signed(code_of(ps[0][2])) if ps and ps[0][0] == T_GF_LIST_R else None
check("A5-unauth", "未认证群文件请求收到类型正确的失败响应", rawcode == -1001, f"code={rawcode} type={ps[0][0] if ps else 'NORESP'}")
raw.close()

# 无路径字段：请求仅由 数值/摘要/名称 组成（静态确认入口不接受路径）
srv = open(os.path.join(REPO, "Server", "msghandler.cpp"), encoding="utf-8").read()
seg = srv.split("PDU* MsgHandler::groupFileRequest", 1)[1].split("\nPDU* MsgHandler::", 1)[0] if "groupFileRequest" in srv else ""
check("A5-no-path-input", "群文件请求无客户端路径入口（仅 offset/limit/size/sha/name/id）",
      "normalizeParent" not in seg and "QDir(" not in seg and "QFileInfo(" in seg, f"seglen={len(seg)}")

# ---------- 验收6：既有面回归（组内 + 个人上传下载仍可用） ----------
print("=== 验收6：既有面回归 ===")
code, d = group(co, T_INVITE, T_INVITE_R, GID2, string(OUTS) + string("gf3-i3"))
IID3 = u64at(d, 24) if code == 0 else 0
code, d = group(cx, T_ACCEPT, T_ACCEPT_R, 0, u64(IID3))
check("A6-group-ops", "建群/邀请/接受等既有群操作仍可用", code == 0, f"code={code}")
code, _ = gf_list(cx, GID2)
check("A6-newmember-list", "新加入成员可列 B 群文件", code == 0, f"code={code}")

co.close(); cx.close(); cm3.close()
stop_server()
print("\n=== 汇总 ===")
passed = sum(1 for _, _, ok in results if ok)
for sid, desc, ok in results:
    if not ok: print(f"  FAIL -> {sid} {desc}")
print(f"{passed}/{len(results)} passed")
sys.exit(0 if passed == len(results) else 1)
