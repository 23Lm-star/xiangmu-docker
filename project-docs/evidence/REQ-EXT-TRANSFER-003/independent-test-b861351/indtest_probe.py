# 独立测试节点（REQ-EXT-TRANSFER-003 @b861351）补充探针 X-1..X-5。
# 不复用 dev 的 verify.py（避免同一份代码自证）：自写最小 wire 原语，只测 dev 套件未覆盖的边界。
#
# X-2 的语义已按隔离复现（build/x2_isolate.py）修正为**事实**：31 整包 > MAX_UPLOAD_DATA_PDU_SIZE 时，
# 服务端走既有"非法 PDU"路径；一个 1 MiB 级越界包会被内核分成多次 recv，每次残段都按垃圾头再计入一次
# 非法计数（到 5 断连），因此**连接被丢弃、进程存活**（新连接可建）——不是 32{ok=false}，也不是崩溃。
# 该口径与 design §2.1「旧服务端遇大分片走既有非法 PDU 路径（计数后断连）」一致；D5 只覆盖正文 ≤ 1 MiB。
import glob, hashlib, os, socket, struct, subprocess, sys, time

HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_CAP, T_CAP_R, T_REG, T_LOGIN, T_LOGIN_R = 44, 45, 1, 3, 4
T_PING, T_PONG = 50, 51
T_ABORT, T_ABORT_R = 107, 108
T_UPINIT, T_UPDATA, T_UPINIT_R, T_UPDATA_R = 29, 31, 30, 32
T_DOWN, T_DOWN_R, T_DATA, T_FINISH = 40, 41, 42, 43
MAGIC = 0x5253554d
ID_OFF = 48
MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DIST = os.path.join(REPO, "dist")
sys.path.insert(0, os.path.join(REPO, "project-docs", "evidence", "_harness"))
import server_ctl
_SRV = server_ctl.ServerCtl(os.path.join(DIST, "Server.exe"), DIST, HOST, PORT)
import atexit
atexit.register(lambda: _SRV.stop())

MAX_CHUNK = 1024 * 1024
MAX_PDU = MAX_CHUNK + 4096
U, U2 = "e2etx3", "e2etx3b"
DST = f"./filesys/{U}/tx3"
DST2 = f"./filesys/{U2}/tx3b"
TAG = f"{int(time.time()) % 100000}"


def f32(s): return s.encode("utf-8")[:31].ljust(32, b"\0")
def creds(n, p="123456"): return f32(n) + hashlib.md5(p.encode()).hexdigest().encode()
def pdu(t, msg=b"", dat=b""):
    dat = dat[:64].ljust(64, b"\0")
    return struct.pack("<III", HDR + len(msg), len(msg), t) + dat + msg
def u32(x): return struct.pack("<I", x)
def u64(x): return struct.pack("<Q", x)
def i64(x): return struct.pack("<q", x)
def i64at(b, o): return struct.unpack("<q", b[o:o + 8])[0]


class Conn:
    def __init__(s):
        s.sock = socket.create_connection((HOST, PORT), timeout=15)
        s.buf = b""

    def pump(s, t):
        s.sock.settimeout(t)
        try:
            d = s.sock.recv(1 << 20)
            if d: s.buf += d
        except (socket.timeout, OSError):
            pass

    def packets(s):
        out = []
        while len(s.buf) >= HDR:
            tl, ml, ty = struct.unpack("<III", s.buf[:12])
            if tl < HDR or tl != HDR + ml or len(s.buf) < tl: break
            out.append((ty, s.buf[12:76], s.buf[76:tl])); s.buf = s.buf[tl:]
        return out

    def wait(s, ty, timeout=8.0):
        dl = time.time() + timeout
        while time.time() < dl:
            for p in s.packets():
                if p[0] == ty: return p
            s.pump(0.2)
        return None

    def send(s, pkt): s.sock.sendall(pkt)

    def alive(s):
        try:
            s.sock.sendall(pdu(T_PING))
            return s.wait(T_PONG, 4) is not None
        except OSError:
            return False

    def close(s):
        try: s.sock.close()
        except OSError: pass


def sql(q):
    return subprocess.run([MYSQL] + DB + [q], capture_output=True, text=True).stdout.strip()


def login(name):
    c = Conn()
    c.send(pdu(T_CAP)); r = c.wait(T_CAP_R); code = r[2].split(b"\0")[0] if r else b""
    c.send(pdu(T_REG, msg=code + b"\0", dat=creds(name))); c.wait(T_REG + 1, 4)
    rt = struct.unpack("<i", (r[2][:0] or b"\0\0\0\0"))[0] if False else 0
    c.close()
    c = Conn()
    c.send(pdu(T_CAP)); r = c.wait(T_CAP_R); code = r[2].split(b"\0")[0] if r else b""
    c.send(pdu(T_LOGIN, msg=code + b"\0", dat=creds(name))); r = c.wait(T_LOGIN_R, 6)
    return c, (struct.unpack("<i", r[1][:4])[0] if r else -99)


def up_init(c, tid, dst, name, size):
    c.send(pdu(T_UPINIT, msg=dst.encode(),
               dat=f32(name) + i64(size) + u32(MAGIC) + b"\0" * 4 + u64(tid)))
    return c.wait(T_UPINIT_R)


def chunk_pdu(tid, off, data):
    return pdu(T_UPDATA, msg=data, dat=i64(off) + b"\0" * 32 + u32(MAGIC) + b"\0" * 4 + u64(tid))


def up_chunk(c, tid, off, data, t=15):
    c.send(chunk_pdu(tid, off, data))
    return c.wait(T_UPDATA_R, t)


def ack_ok(r): return r is not None and r[1][0] != 0
def ack_next(r): return i64at(r[1], 8) if r else -1
def init_off(r): return i64at(r[1], 32) if r else -1


def staging_size(owner, display):
    out = sql(f"SELECT staging_path FROM upload_session WHERE owner_name='{owner}' "
              f"AND display_name='{display}' ORDER BY session_id DESC LIMIT 1")
    if not out:
        return -1
    p = out.splitlines()[0]
    try:
        return os.path.getsize(p) if os.path.isabs(p) else os.path.getsize(os.path.join(DIST, p))
    except OSError:
        return -1


def row_state(owner, display):
    return sql(f"SELECT session_id,status,confirmed_offset FROM upload_session WHERE owner_name='{owner}' "
               f"AND display_name='{display}' ORDER BY session_id")


def download(c, path, name, tid=9101):
    c.send(pdu(T_DOWN, msg=path.encode(), dat=f32(name) + b"\0" * 16 + u64(tid)))
    data, ok, fin = b"", None, None
    dl = time.time() + 30
    while time.time() < dl:
        for ty, dat, msg in c.packets():
            if ty == T_DOWN_R: ok = dat[0] != 0
            elif ty == T_DATA: data += msg
            elif ty == T_FINISH: fin = dat[0] != 0
        if fin is not None: break
        c.pump(0.2)
    return {"ok": ok, "data": data, "fin": fin}


results = []
def check(sid, desc, cond, detail=""):
    results.append((sid, desc, bool(cond)))
    print(f"[{'PASS' if cond else 'FAIL'}] {sid} {desc} :: {detail}", flush=True)


print(f"=== 环境（tag={TAG}）===")
if not _SRV.start():
    print(f"FATAL: {_SRV.last_error}"); sys.exit(2)
for u, d in ((U, "tx3"), (U2, "tx3b")):
    os.makedirs(os.path.join(DIST, "filesys", u, d), exist_ok=True)
    sql(f"DELETE FROM upload_session WHERE owner_name='{u}' AND display_name LIKE 'x1_{TAG}%';")
c, ret = login(U)
check("X0", "登录成功", ret == 1, f"ret={ret}")
if ret != 1: sys.exit(2)

# ---------- X-1：1 MiB 块分 256 片慢发（累计缓冲路径；dev 套件全部整体 send） ----------
size = 5 * MAX_CHUNK
name = f"x1_{TAG}_frag.bin"
payload = (TAG.encode() + b"X1" + bytes(range(256)) * (size // 256 + 2))[:size]
r = up_init(c, 2001, DST, name, size)
check("X1-init", "5 MiB INIT ok", ack_ok(r), f"off={init_off(r)}")
t0 = time.time()
worst = ""
for off in range(0, 4 * MAX_CHUNK, MAX_CHUNK):
    pkt = chunk_pdu(2001, off, payload[off:off + MAX_CHUNK])
    for i in range(0, len(pkt), 4096):        # 4 KiB 一片，逼出跨 recv 的累计缓冲
        c.send(pkt[i:i + 4096])
        time.sleep(0.001)
    a = up_chunk_wait = c.wait(T_UPDATA_R, 20)
    if not ack_ok(a):
        worst = f"off={off} ack={a}"
        break
check("X1a", "1 MiB 块 256×4 KiB 慢发：4 块全接受（缓冲上限必须 ≥ 整包）", worst == "",
      f"{worst or f'next={ack_next(a)} t={time.time() - t0:.1f}s'}")
check("X1b", "暂存物理大小 == 4 MiB（5 MiB 文件只发了 4 MiB，会话仍未完成）",
      staging_size(U, name) == 4 * MAX_CHUNK, f"{staging_size(U, name)}")
r = None
c.send(pdu(T_ABORT, dat=b"\0" * ID_OFF + u64(2001))); r = c.wait(T_ABORT_R)
check("X1c", "107 收尾（108 ret=true）", r is not None and r[1][0] != 0, "")

# ---------- X-2：31 整包 = 上限 + 1 → 既有非法 PDU 路径：连接被丢弃、进程存活、会话态不变 ----------
name2 = f"x1_{TAG}_over.bin"
r = up_init(c, 2002, DST, name2, 2 * MAX_CHUNK)
check("X2-init", "INIT ok", ack_ok(r), f"off={init_off(r)}")
before = row_state(U, name2)
pids_before = sorted(server_ctl.find_listener_pids(PORT))
c.send(chunk_pdu(2002, 0, b"O" * (MAX_PDU - HDR + 1)))     # 整包 = MAX_PDU + 1
a = c.wait(T_UPDATA_R, 1.5)
check("X2a", "越界 31 包无 32 回执（不进 D5 显式拒绝路径）", a is None, f"ack={'yes' if a else 'no'}")
time.sleep(1.5)
check("X2b", "该连接被丢弃（非法 PDU 计数经多次残段读取达阈值 5）", not c.alive(), "")
c.close()
check("X2c", "服务端进程存活（监听 PID 不变，非崩溃）",
      sorted(server_ctl.find_listener_pids(PORT)) == pids_before, f"{pids_before}")
c2, ret2 = login(U)
check("X2d", "可重建新连接并登录", ret2 == 1, f"ret={ret2}")
check("X2e", "会话账目未变（同一行、confirmed_offset 未动）", row_state(U, name2) == before,
      f"{before!r} -> {row_state(U, name2)!r}")
c.close(); c = c2

# ---------- X-3：31 整包 == 上限（正文 1 MiB + 4020）→ 解析通过但被 D5 以 32{ok=false} 拒绝 ----------
name3 = f"x1_{TAG}_cap.bin"
r = up_init(c, 2003, DST, name3, 2 * MAX_CHUNK)
check("X3-init", "INIT ok", ack_ok(r), f"off={init_off(r)}")
c.send(chunk_pdu(2003, 0, b"P" * (MAX_PDU - HDR)))          # 整包 == MAX_PDU（恰好到顶）
a = c.wait(T_UPDATA_R, 8)
check("X3a", "恰好到顶的 31 包被显式拒绝（32 ok=false，非断连）", a is not None and a[1][0] == 0,
      f"totallen={MAX_PDU} ack_ok={a[1][0] if a else 'none'}")

# ---------- X-4：拒绝之后账目不错乱，且该会话仍能正常完成 ----------
blk = payload[:MAX_CHUNK]
a = up_chunk(c, 2003, 0, blk)
check("X4a", "拒绝之后合法块仍被接受（next == 1 MiB）", ack_ok(a) and ack_next(a) == MAX_CHUNK,
      f"next={ack_next(a)}")
check("X4b", "暂存物理大小 == 1 MiB（无残留脏写）", staging_size(U, name3) == MAX_CHUNK,
      f"{staging_size(U, name3)}")
a = up_chunk(c, 2003, MAX_CHUNK, payload[MAX_CHUNK:2 * MAX_CHUNK])
check("X4c", "补末块后完成（32 complete）", a is not None and a[1][16] != 0, "")
time.sleep(0.8)
st = row_state(U, name3)
d = download(c, f"{DST}/{name3}", name3)
check("X4d", "会话 done 且下载逐字节一致", st.splitlines()[-1].split("\t")[1] == "done"
      and d["data"] == payload[:2 * MAX_CHUNK],
      f"status={st.splitlines()[-1].split(chr(9))[1] if st else '?'} len={len(d['data'])}")

# ---------- X-5：两条连接（两个用户）各传一个大文件（跨连接并发；dev 套件只做单连接 3 路） ----------
print("=== X-5 ===")
pls, cs, oks = {}, {}, {}
for k, (u, dst) in enumerate(((U, DST), (U2, DST2))):
    cc, rr = login(u)
    cs[k] = cc
    pls[k] = (TAG.encode() + f"X5{k}".encode() + bytes([80 + k]) * (3 * MAX_CHUNK))[:3 * MAX_CHUNK]
    nm = f"x1_{TAG}_par{k}.bin"
    r = up_init(cc, 5001 + k, dst, nm, len(pls[k]))
    if not ack_ok(r):
        check(f"X5-init{k}", "INIT ok", False, f"{r}")
offs = {k: 0 for k in cs}
ok_all = True
while any(offs[k] < len(pls[k]) for k in cs):
    for k in cs:
        if offs[k] >= len(pls[k]): continue
        off = offs[k]
        a = up_chunk(cs[k], 5001 + k, off, pls[k][off:off + MAX_CHUNK])
        ok_all = ok_all and ack_ok(a)
        offs[k] = off + MAX_CHUNK
time.sleep(0.8)
for k, (u, dst) in enumerate(((U, DST), (U2, DST2))):
    nm = f"x1_{TAG}_par{k}.bin"
    oks[k] = row_state(u, nm).count("\n") + 1 if row_state(u, nm) else 0
    dls = download(cs[k], f"{dst}/{nm}", nm)
    check(f"X5a{k}", f"跨连接并发第 {k} 路逐字节正确", dls["data"] == pls[k],
          f"len={len(dls['data'])} ok={ok_all}")
    check(f"X5b{k}", f"跨连接并发第 {k} 路单行会话", oks[k] == 1, f"rows={oks[k]}")
    cs[k].close()

okn = sum(1 for _, _, cc in results if cc)
print(f"\n=== 汇总 {okn}/{len(results)} ===")
for sid, desc, cc in results:
    if not cc: print(f"  FAIL -> {sid} {desc}")
if len(sys.argv) > 1 and sys.argv[1] == "--keep":
    pass
else:
    sql(f"DELETE FROM user_file WHERE owner_name IN ('{U}','{U2}') AND display_name LIKE 'x1_{TAG}%';")
    sql(f"DELETE FROM upload_session WHERE owner_name IN ('{U}','{U2}') AND display_name LIKE 'x1_{TAG}%';")
c.close()
_SRV.stop()
sys.exit(0 if okn == len(results) else 1)
