# REQ-EXT-TRANSFER-002 下载暂停/取消与断点续传：e2e（裸 socket 驱动服务端可观测面）。
# 断言口径：design.md §6（F7）——静态面 S-01..S-04（4 条，S-04 为 FINDING-AUDIT-1 回归钉）+
# 动态面 T2-01..T2-14（14 行，含 a/b 子断言，共 19 条）+ 环境锚点 E0/E0b/E1（3 条）= 26 条。GUI 8 项（design §6.3）属
# NEEDS-HUMAN，不在本套件范围。纯客户端状态机（暂停按钮生效/分片保留/快照恢复）由 wire 级护栏
# 与静态契约覆盖到可自动化边界，动态人工项见 design §6.3 清单。
# 运行形态（E2E-RUNFORM）：py -3.13 project-docs/evidence/REQ-EXT-TRANSFER-002/verify.py
#   exe=<repo>/dist/Server.exe cwd=<repo>/dist；filesPath=./filesys；生命周期自管（_harness/server_ctl，PID-only）。
import hashlib, os, re, shutil, socket, struct, subprocess, sys, time

HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_REG, T_CAP, T_CAP_R, T_LOGIN, T_LOGIN_R, T_PING, T_PONG = 1, 44, 45, 3, 4, 50, 51
T_MKDIR, T_MKDIR_R = 19, 20
T_UPINIT, T_UPDATA, T_UPINIT_R, T_UPDATA_R = 29, 31, 30, 32
T_DOWN, T_DOWN_R, T_DATA, T_FINISH = 40, 41, 42, 43
T_ABORT, T_ABORT_R = 107, 108
T_GCREATE, T_GCREATE_R = 60, 61
T_GF_INIT, T_GF_INIT_R, T_GF_LIST, T_GF_LIST_R = 101, 102, 99, 100
T_GF_DOWN, T_GF_DOWN_R = 105, 106
MAGIC = 0x5253554d
ID_OFF, DL_ID_OFF, RS_OFF, AC_OFF = 48, 16, 32, 24   # 40/105 起点 [32..39]；41/106 采纳 [24..31]
MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
DIST = os.path.join(REPO, "dist")
EXE = os.path.join(DIST, "Server.exe")
sys.path.insert(0, os.path.join(REPO, "project-docs", "evidence", "_harness"))
import server_ctl
_SRV = server_ctl.ServerCtl(EXE, DIST, HOST, PORT)
U = "e2etx2"
DST = f"./filesys/{U}/tx2"
SHA = lambda b: hashlib.sha256(b).hexdigest()
NONCE = f"tx{int(time.time())}".encode()

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

# ---------- 传输原语（起点参数化 = 本需求增量） ----------
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

def dl_req(c, tid, path, name, off=0, wait_resp=True):
    c.send(pdu(T_DOWN, msg=path.encode(),
               dat=f32(name) + i64(off) + b"\0" * 8 + u64(tid)))
    return c.wait(T_DOWN_R) if wait_resp else None

def gf_init(c, tid, gid, size, sha, name):
    c.send(pdu(T_GF_INIT, msg=envelope(gid, u64(size) + string(sha.encode()) + string(name.encode())),
               dat=b"\0" * ID_OFF + u64(tid)))
    return c.wait(T_GF_INIT_R)

def gf_down(c, tid, gid, gfid, off=0, wait_resp=True):
    c.send(pdu(T_GF_DOWN, msg=envelope(gid, u64(gfid)),
               dat=b"\0" * RS_OFF + i64(off) + b"\0" * 8 + u64(tid)))
    return c.wait(T_GF_DOWN_R) if wait_resp else None

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

def collect(c, ids, timeout=30.0):
    got = {i: {"data": b"", "finish": None, "ok": None, "acc": None} for i in ids}
    dl = time.time() + timeout
    while time.time() < dl:
        for ty, dat, msg in c.take(0.2):
            tid = eid(dat, DL_ID_OFF if ty == T_DOWN_R else ID_OFF)
            if tid not in got: continue
            if ty == T_DOWN_R: got[tid]["ok"] = dat[0] != 0; got[tid]["acc"] = i64at(dat, AC_OFF)
            elif ty == T_DATA: got[tid]["data"] += msg
            elif ty == T_FINISH: got[tid]["finish"] = dat[0] != 0
        if all(v["finish"] is not None for v in got.values()): break
    return got

def drain_quiet(c, keep, quiet=1.2, total=15.0):
    """把 keep 内各 id 的在途 42/43 收干：连续 quiet 秒无相关帧即认定服务端已停发。
    返回 {tid: (data_bytes, saw_finish)}。keep 为 id 集合。"""
    acc = {i: b"" for i in keep}
    fin = {i: False for i in keep}
    last_hit = time.time(); dl = time.time() + total
    while time.time() < dl:
        hit = False
        for ty, dat, msg in c.take(0.15):
            if ty in (T_DATA, T_FINISH):
                tid = eid(dat)
                if tid in acc:
                    hit = True
                    if ty == T_DATA: acc[tid] += msg
                    else: fin[tid] = True
        if hit: last_hit = time.time()
        elif time.time() - last_hit > quiet: return acc, fin
    return acc, fin

# ================= 静态面（§6.1） =================
print("=== 静态面：S-01..S-04 ===")
def read(p): return open(os.path.join(REPO, p), encoding="utf-8").read()
cph, sph = read("Client/protocol.h"), read("Server/protocol.h")
pat002 = re.compile(r"// ===== REQ-EXT-TRANSFER-002.*?DOWNLOAD_ACCEPTED_OFFSET = 24;.*\n", re.S)
b002c, b002s = pat002.search(cph), pat002.search(sph)
check("S-01", "两端 protocol.h 的 REQ-EXT-TRANSFER-002 常量块逐字节一致（含注释与偏移值）",
      b002c is not None and b002c.group(0) == b002s.group(0), f"len={len(b002c.group(0)) if b002c else 0}")
pat001 = re.compile(r"// ===== REQ-EXT-TRANSFER-001：transfer_id 落位.*?MAX_PARALLEL_DOWNLOADS = 3;.*\n", re.S)
b001c, b001s = pat001.search(cph), pat001.search(sph)
check("S-02", "新块位于 REQ-EXT-TRANSFER-001 块之后，且既有块文本零改动（parallel_e2e P1 面不受影响）",
      b001c is not None and b001c.group(0) == b001s.group(0)
      and cph.find("REQ-EXT-TRANSFER-001") < cph.find("REQ-EXT-TRANSFER-002")
      and sph.find("REQ-EXT-TRANSFER-001") < sph.find("REQ-EXT-TRANSFER-002"), "")
def offval(txt, name):
    m = re.search(rf"static const int {name} = (\d+);", txt)
    return int(m.group(1)) if m else -1
check("S-03", "40/105 起点偏移 == 32、41/106 采纳起点偏移 == 24（双端同值）",
      offval(cph, "DOWNLOAD_RESUME_OFFSET") == 32 and offval(sph, "DOWNLOAD_RESUME_OFFSET") == 32
      and offval(cph, "DOWNLOAD_ACCEPTED_OFFSET") == 24 and offval(sph, "DOWNLOAD_ACCEPTED_OFFSET") == 24, "")
# S-04（FINDING-AUDIT-1 回归钉）：§4.3 必做防御第 4 项——finishTask 下载分支必须摘除 id 映射，
# 否则护栏 Failed 后在途 43 经 taskByTransferId 命中将终态翻转为 Done（静默损坏件）。
ttxt = read("Client/transfertask.cpp")
_ft = re.search(r"void TransferManager::finishTask\(.*?\n\}", ttxt, re.S)
_dl = _ft.group(0).split("} else {", 1) if _ft else []
check("S-04", "finishTask 下载分支摘除 m_idToKey（§4.3 第四项，钉住 FINDING-AUDIT-1）",
      len(_dl) == 2 and "m_idToKey.remove(t.id)" in _dl[1], "")

# ================= 环境准备 =================
print("=== 环境准备 ===")
if not start_server():
    print("FATAL: Server 启动失败"); sys.exit(2)
for tbl in ("user_file", "upload_session"):
    sql(f"DELETE FROM {tbl} WHERE owner_name='{U}';")
sql(f"DELETE FROM group_file WHERE uploader_name='{U}';")
sql(f"DELETE FROM group_member WHERE user_name='{U}';")
sql(f"DELETE FROM group_info WHERE creator_name='{U}';")
shutil.rmtree(os.path.join(DIST, "filesys", ".staging"), ignore_errors=True)

c, ret = login_conn(U)
check("E0", "登录成功", ret == 1, f"ret={ret}")
if ret != 1: sys.exit(2)
check("E0b", "建测试目录", mkdir(c, f"./filesys/{U}", "tx2") is not None, DST)
c.send(pdu(T_GCREATE, msg=envelope(0, string(b"tx2-group"))))
r = c.wait(T_GCREATE_R)
GID = u64at(r[2], 24) if r and u32at(r[2], 20) == 0 else 0
check("E1", "建群成功", GID > 0, f"gid={GID}")

SRC = NONCE + bytes((i * 7 + 13) & 0xFF for i in range(3000))          # 3KB：基线/续传/边界
BIG = bytes((i * 31 + 5) & 0xFF for i in range(2 * 1024 * 1024))       # 2MB：暂停中流
MID = [NONCE + bytes(((i + k) * 17 + 3) & 0xFF for i in range(512 * 1024)) for k in range(3)]  # 512KB×3
GF = NONCE + b"GF" + bytes((i * 11 + 29) & 0xFF for i in range(512 * 1024))                     # 群 512KB
print("=== 源文件落定（上传） ===")
assert upload(c, 6001, DST, "tx2_src.bin", SRC, chunk=500), "源上传失败"
assert upload(c, 6002, DST, "tx2_big.bin", BIG, chunk=8192), "大文件上传失败"
for k in range(3):
    assert upload(c, 6011 + k, DST, f"tx2_mid{k}.bin", MID[k], chunk=8192), "mid 上传失败"
gi = gf_init(c, 6021, GID, len(GF), SHA(GF), "tx2_group.bin")
assert gi is not None and u32at(gi[2], 20) == 0, "群初始化失败"
nxt = int(i64at(gi[2], 52))
for o in range(nxt, len(GF), 8192):
    up_chunk(c, 6021, o, GF[o:o + 8192])
recs = [x for x in gf_records(c, GID) if x["name"] == "tx2_group.bin"]
assert recs, "群文件列表无记录"
GFID = recs[-1]["id"]
P = f"{DST}/tx2_src.bin"; PBIG = f"{DST}/tx2_big.bin"
PMID = [f"{DST}/tx2_mid{k}.bin" for k in range(3)]

# ================= T2-01 基线（回归锚点，验收 8） =================
print("=== T2-01 ===")
c1, _ = login_conn(U)
r = dl_req(c1, 1001, P, "tx2_src.bin", 0)
got = collect(c1, [1001])
check("T2-01", "起点 0 → 41 ret=true、采纳 0、正文逐字节==源、43 success（旧语义不变）",
      r is not None and r[1][0] != 0 and i64at(r[1], AC_OFF) == 0 and eid(r[1], DL_ID_OFF) == 1001
      and got[1001]["finish"] and got[1001]["data"] == SRC, f"len={len(got[1001]['data'])}")

# ================= T2-02 真续传（验收 1、2） =================
print("=== T2-02 ===")
N = 1000
r = dl_req(c1, 1002, P, "tx2_src.bin", N)
got = collect(c1, [1002])
check("T2-02", f"起点 N={N} → 41 采纳==N；正文逐字节==源[N:]；43 success（前缀不重传）",
      r is not None and r[1][0] != 0 and i64at(r[1], AC_OFF) == N
      and got[1002]["finish"] and got[1002]["data"] == SRC[N:], f"len={len(got[1002]['data'])}")

# ================= T2-03 起点==大小（D5） =================
print("=== T2-03 ===")
r = dl_req(c1, 1003, P, "tx2_src.bin", len(SRC))
got = collect(c1, [1003])
check("T2-03", "起点==文件大小 → 受理：0 字节正文 + 43 success",
      r is not None and r[1][0] != 0 and i64at(r[1], AC_OFF) == len(SRC)
      and got[1003]["finish"] and len(got[1003]["data"]) == 0, "")

# ================= T2-04 起点>大小（验收 6） =================
print("=== T2-04 ===")
r = dl_req(c1, 1004, P, "tx2_src.bin", len(SRC) + 1)
check("T2-04a", "起点>大小 → 41 ret=false 且不建流（无 42/43）",
      r is not None and r[1][0] == 0 and c1.take(1.0) == [], f"ret={r[1][0] if r else None}")
c1.send(pdu(T_PING)); pong = c1.wait(T_PONG, 3)
okb = dl_req(c1, 1005, P, "tx2_src.bin", 0)
gotb = collect(c1, [1005])
check("T2-04b", "拒绝后续传连接存活：心跳正常 + 后续基线下载逐字节完整",
      pong is not None and okb is not None and okb[1][0] != 0
      and gotb[1005]["finish"] and gotb[1005]["data"] == SRC, "")

# ================= T2-05 中途 107 真停发（验收 3） =================
print("=== T2-05 ===")
c2, _ = login_conn(U)
dl_req(c2, 2001, PBIG, "tx2_big.bin", 0, wait_resp=False)
prefix = b""
deadline = time.time() + 20
while len(prefix) < 400_000 and time.time() < deadline:
    for ty, dat, msg in c2.take(0.05):
        if ty == T_DATA and eid(dat) == 2001: prefix += msg
r = abort(c2, 2001)
acc, fin = drain_quiet(c2, {2001})
prefix += acc[2001]
check("T2-05a", "中途 107 → 108 ret=true 且回带 id",
      r is not None and r[1][0] != 0 and eid(r[1]) == 2001, f"ret={r[1][0] if r else None}")
check("T2-05b", "107 后该 id 静默（无 43、在途收干后不再有任何 42）且流未发完",
      not fin[2001] and 0 < len(prefix) < len(BIG), f"prefix={len(prefix)}/{len(BIG)}")

# ================= T2-06 暂停→续传拼接 == 源（验收 1、2） =================
print("=== T2-06 ===")
M = len(prefix)
dl_req(c2, 2002, PBIG, "tx2_big.bin", M, wait_resp=False)
got = collect(c2, [2002], timeout=60.0)
suffix = got[2002]["data"]
check("T2-06", f"以新 id+起点 M={M} 续传：首块首字节==源[M]、前缀不重传、累计正文逐字节==源",
      got[2002]["finish"] and len(suffix) == len(BIG) - M and suffix == BIG[M:]
      and prefix + suffix == BIG, f"suffix={len(suffix)}")

# ================= T2-07 暂停即释放名额（验收 3、4） =================
print("=== T2-07 ===")
c3, _ = login_conn(U)
# 3 路 2MB 流故意不聚合正文：背压使其在途驻留，稳定占满名额（T-18 同源手法）
for k in range(3):
    dl_req(c3, 3001 + k, PBIG, "tx2_big.bin", 0, wait_resp=False)
dl_req(c3, 3011, PBIG, "tx2_big.bin", 0, wait_resp=False)          # 第 4 个（应拒）
r41 = {}
d41 = time.time() + 8
while len(r41) < 4 and time.time() < d41:
    for ty, dat, msg in c3.take(0.1):
        if ty == T_DOWN_R: r41[eid(dat, DL_ID_OFF)] = dat[0] != 0
check("T2-07a", "3 路在途时第 4 路 41 ret=false（并发上限语义不变）",
      r41.get(3011) is False and all(r41.get(i) is True for i in (3001, 3002, 3003)), f"{r41}")
r = abort(c3, 3001)
r2 = dl_req(c3, 3012, PMID[1], "tx2_mid1.bin", 0)
check("T2-07b", "暂停 1 路后名额即时回落：新 41 ret=true",
      r is not None and r[1][0] != 0 and r2 is not None and r2[1][0] != 0, "")
c3.close()

# ================= T2-08 107 边界（验收 8，既有语义不变） =================
print("=== T2-08 ===")
c4, _ = login_conn(U)
r = abort(c4, 7777)
check("T2-08a", "未知 id 的 107 → 108 ret=false（既有语义不变）",
      r is not None and r[1][0] == 0 and eid(r[1]) == 7777, "")
dl_req(c4, 0, PBIG, "tx2_big.bin", 0, wait_resp=False)             # id=0 旧单流在途
r = abort(c4, 0)
check("T2-08b", "id=0 的 107 一律不受理：108 ret=false，旧单流不受影响照常完成",
      r is not None and r[1][0] == 0, f"ret={r[1][0] if r else None}")

# ================= T2-14 id=0 全流程逐字节不变（验收 8，与 parallel_e2e T-15 同源双保险） =================
print("=== T2-14 ===")
got = collect(c4, [0], timeout=40.0)
check("T2-14", "id=0 的 40/41/42/43 全流程不变：正文==源、43 success、全部回带 id=0",
      got[0]["ok"] is True and got[0]["finish"] and got[0]["data"] == BIG,
      f"len={len(got[0]['data'])}")
c4.close()

# ================= T2-09 群真续传（验收 1、2） =================
print("=== T2-09 ===")
c5, _ = login_conn(U)
N = 100_000
r = gf_down(c5, 4001, GID, GFID, N)
code = u32at(r[2], 20) if r else -1
got = collect(c5, [4001], timeout=60.0)
check("T2-09", f"105 起点 N={N} → 106 code=0、采纳==N、size==源大小；正文==源[N:] 且 43 success",
      r is not None and code == 0 and i64at(r[1], AC_OFF) == N and eid(r[1]) == 4001
      and u64at(r[2], 24) == len(GF) and got[4001]["finish"] and got[4001]["data"] == GF[N:],
      f"code={code} len={len(got[4001]['data'])}")

# ================= T2-10 群起点>大小（验收 6） =================
print("=== T2-10 ===")
r = gf_down(c5, 4002, GID, GFID, len(GF) + 1)
code = struct.unpack("<i", r[2][20:24])[0] if r else 0   # 106 的 code 按有符号读（-2108）
silent = [p for p in c5.take(1.0) if p[0] in (T_DATA, T_FINISH)]
c5.send(pdu(T_PING)); pong = c5.wait(T_PONG, 3)
check("T2-10", "群起点>大小 → 106 code=-2108 且不建流；连接存活",
      r is not None and code == -2108 and not silent and pong is not None, f"code={code}")

# ================= T2-11 群暂停→续传（验收 1、3） =================
print("=== T2-11 ===")
c6, _ = login_conn(U)
gf_down(c6, 4011, GID, GFID, 0, wait_resp=False)
gprefix = b""
deadline = time.time() + 20
while len(gprefix) < 200_000 and time.time() < deadline:
    for ty, dat, msg in c6.take(0.05):
        if ty == T_DATA and eid(dat) == 4011: gprefix += msg
r = abort(c6, 4011)
gacc, gfin = drain_quiet(c6, {4011})
gprefix += gacc[4011]
M = len(gprefix)
gf_down(c6, 4012, GID, GFID, M, wait_resp=False)
got = collect(c6, [4012], timeout=60.0)
check("T2-11a", "群 107 → 108 ret=true、释放后在途收干即静默（未发完）",
      r is not None and r[1][0] != 0 and not gfin[4011] and 0 < M < len(GF), f"M={M}")
check("T2-11b", "群续传 == 源后缀且拼接逐字节==源",
      got[4012]["finish"] and gprefix + got[4012]["data"] == GF, f"suffix={len(got[4012]['data'])}")
c6.close()

# ================= T2-12 暂停一路不影响其余两路（验收 4） =================
print("=== T2-12 ===")
c7, _ = login_conn(U)
for k in range(3):
    dl_req(c7, 3101 + k, PMID[k], f"tx2_mid{k}.bin", 0, wait_resp=False)
abort(c7, 3101)
others, ofin = {}, {3102: None, 3103: None}
dl = time.time() + 60
while time.time() < dl and not all(v is not None for v in ofin.values()):
    for ty, dat, msg in c7.take(0.1):
        tid = eid(dat, DL_ID_OFF if ty == T_DOWN_R else ID_OFF)
        if tid not in ofin: continue
        if ty == T_DATA: others.setdefault(tid, b""); others[tid] += msg
        elif ty == T_FINISH: ofin[tid] = dat[0] != 0
pacc, pfin = drain_quiet(c7, {3101})
check("T2-12", "3 路并发暂停 1 路：其余 2 路照常 43 success 且内容完整，被暂停者无 43",
      all(ofin.values()) and others.get(3102) == MID[1] and others.get(3103) == MID[2]
      and not pfin[3101], f"fin={ofin} paused_len={len(pacc[3101])}")
c7.close()

# ================= T2-13 下载暂停/取消期间上传照常（验收 4） =================
print("=== T2-13 ===")
c8, _ = login_conn(U)
dl_req(c8, 3201, PMID[0], "tx2_mid0.bin", 0, wait_resp=False)
dl_req(c8, 3202, PMID[1], "tx2_mid1.bin", 0, wait_resp=False)
up_ok = upload(c8, 3301, DST, "tx2_up.bin", NONCE + b"U" * 2000, chunk=500)   # 与下载同连接
abort(c8, 3201)
dl_req(c8, 3203, PMID[2], "tx2_mid2.bin", 0, wait_resp=False)
abort(c8, 3203)
check("T2-13", "下载暂停进行中的同一连接上，上传 29/31/32 正常走完（upload() 逐 ack 校验）",
      up_ok, f"upload={up_ok}")
c8.close()

# ================= 汇总 =================
print("\n=== 汇总 ===")
okn = sum(1 for _, _, cc in results if cc)
for sid, desc, cc in results:
    if not cc: print(f"  FAIL -> {sid} {desc}")
print(f"{okn}/{len(results)} passed")
print("NOTE: design §6.3 的 8 条 GUI 人工项（暂停按钮生效/断点继续/取消删分片/失败重试续传/"
      "重启恢复/分片被删重下/暂停期上传互不干扰）属 NEEDS-HUMAN，非本套件可断言范围。")
stop_server()
sys.exit(0 if okn == len(results) else 1)
