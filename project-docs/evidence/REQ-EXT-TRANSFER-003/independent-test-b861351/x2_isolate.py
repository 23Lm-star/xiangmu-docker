# X-2 隔离复现：31 整包 = MAX_UPLOAD_DATA_PDU_SIZE + 1 时，服务端是"有意断连"还是"崩溃"。
# 事实面：连接是否被 reset；服务端 PID 是否仍 LISTENING；新一轮连接能否建立；5 次越界后是否按阈值断连。
import hashlib, os, socket, struct, subprocess, sys, time

HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_CAP, T_CAP_R, T_REG, T_LOGIN, T_LOGIN_R = 44, 45, 1, 3, 4
T_PING, T_PONG = 50, 51
T_UPINIT, T_UPDATA, T_UPINIT_R, T_UPDATA_R = 29, 31, 30, 32
MAGIC = 0x5253554d
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DIST = os.path.join(REPO, "dist")
sys.path.insert(0, os.path.join(REPO, "project-docs", "evidence", "_harness"))
import server_ctl
_SRV = server_ctl.ServerCtl(os.path.join(DIST, "Server.exe"), DIST, HOST, PORT)
import atexit
atexit.register(lambda: _SRV.stop())   # 进程异常退出也要收尾（上一轮探针崩溃留下过孤儿服务端）

MAX_CHUNK = 1024 * 1024
MAX_PDU = MAX_CHUNK + 4096
U = "e2etx3"
DST = f"./filesys/{U}/tx3"


def f32(s): return s.encode("utf-8")[:31].ljust(32, b"\0")
def creds(n, p="123456"): return f32(n) + hashlib.md5(p.encode()).hexdigest().encode()
def pdu(t, msg=b"", dat=b""):
    dat = dat[:64].ljust(64, b"\0")
    return struct.pack("<III", HDR + len(msg), len(msg), t) + dat + msg
def u32(x): return struct.pack("<I", x)
def u64(x): return struct.pack("<Q", x)
def i64(x): return struct.pack("<q", x)


def open_conn():
    s = socket.create_connection((HOST, PORT), timeout=10)
    s.settimeout(4)
    return s


def login(s=None):
    s = open_conn() if s is None else s
    s.sendall(pdu(T_CAP))
    time.sleep(0.3)
    try: buf = s.recv(1 << 16)
    except OSError: buf = b""
    code = buf[76:].split(b"\0")[0] if len(buf) >= 76 else b""
    s.sendall(pdu(T_REG, msg=code + b"\0", dat=creds(U)))
    time.sleep(0.3)
    try: s.recv(1 << 16)
    except OSError: pass
    s.close()
    s = open_conn()
    s.sendall(pdu(T_CAP)); time.sleep(0.3)
    try: buf = s.recv(1 << 16)
    except OSError: buf = b""
    code = buf[76:].split(b"\0")[0] if len(buf) >= 76 else b""
    s.sendall(pdu(T_LOGIN, msg=code + b"\0", dat=creds(U)))
    time.sleep(0.4)
    try: r = s.recv(1 << 16)
    except OSError: r = b""
    ret = struct.unpack("<i", r[12:16])[0] if len(r) >= 16 else -99
    return s, ret


def alive_pids():
    return sorted(server_ctl.find_listener_pids(PORT))


print("=== X-2 隔离复现 ===")
if not _SRV.start():
    print(f"FATAL: {_SRV.last_error}"); sys.exit(2)
pid0 = alive_pids()
print(f"server pid={_SRV.pid} listeners={pid0}")

s, ret = login(None)
print(f"login ret={ret}")
s.sendall(pdu(T_UPINIT, msg=DST.encode(), dat=f32("x2_iso.bin") + i64(2 * MAX_CHUNK) + u32(MAGIC) + b"\0" * 4 + u64(4001)))
time.sleep(0.4)
try:
    r = s.recv(1 << 16)
    print(f"INIT ack bytes={len(r)} ok={r[12] if len(r) > 12 else '?'}")
except OSError as e:
    print(f"INIT read err={e}")

over_body = b"O" * (MAX_PDU - HDR + 1)
over_pkt = pdu(T_UPDATA, msg=over_body, dat=i64(0) + b"\0" * 32 + u32(MAGIC) + b"\0" * 4 + u64(4001))
print(f"send over-cap 31: total={len(over_pkt)} cap={MAX_PDU}")
try:
    s.sendall(over_pkt)
    print("sendall ok")
except OSError as e:
    print(f"sendall err={e}")
time.sleep(2.0)
print(f"listeners after over-cap = {alive_pids()}  (same as before: {alive_pids() == pid0})")
# 连接是否还活着
try:
    s.sendall(pdu(T_PING))
    s.settimeout(3)
    d = s.recv(1 << 16)
    print(f"PING after over-cap: got {len(d)} bytes (alive={len(d) > 0})")
except OSError as e:
    print(f"PING after over-cap: socket error {e!r} -> connection was reset/closed")
try:
    s.close()
except OSError:
    pass

# 新连接能否建立（证明是"连接级"还是"进程级"）
try:
    s2 = open_conn(); print("new connection: established")
    s2.sendall(pdu(T_PING))
    try:
        d = s2.recv(1 << 16)
        print(f"fresh conn PING: got {len(d)} bytes")
    except OSError as e:
        print(f"fresh conn PING err={e!r}")
    s2.close()
except OSError as e:
    print(f"new connection FAILED: {e!r} -> server not accepting (crashed?)")

# 阈值面：同一条连接上连续 5 次越界（中途不发任何合法 PDU，否则计数器会重置）
s3 = open_conn()
s3.sendall(pdu(T_CAP)); time.sleep(0.3)
try:
    buf = s3.recv(1 << 16)
except OSError:
    buf = b""
code = buf[76:].split(b"\0")[0] if len(buf) >= 76 else b""
s3.sendall(pdu(T_LOGIN, msg=code + b"\0", dat=creds(U))); time.sleep(0.4)
try:
    s3.recv(1 << 16)
except OSError:
    pass
for i in range(5):
    try:
        s3.sendall(over_pkt)
        print(f"consecutive invalid #{i + 1}: sent ({len(over_pkt)} B)")
    except OSError as e:
        print(f"consecutive invalid #{i + 1}: send err={e!r}"); break
    time.sleep(0.5)
try:
    s3.sendall(pdu(T_PING)); s3.settimeout(2.0)
    d = s3.recv(1 << 16)
    print(f"after 5 consecutive invalid: connection alive={len(d) > 0} ({len(d)} bytes)")
except OSError as e:
    print(f"after 5 consecutive invalid: disconnected ({e!r})")
print(f"listeners at end = {alive_pids()}  (same pid set: {alive_pids() == pid0})")
try: s3.close()
except OSError: pass
_SRV.stop()
