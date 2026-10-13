#!/usr/bin/env python
# N-1 arbitration probe (one-time diagnostic).
# Reproduces the exact N-1-suspicion condition with time compression:
#   97-miss placeholder (id=0) + 3 idle INIT slots -> reap timer fires -> abortUpload(id=0)
# Server under test: build/wt-n1-ab-cand/dist/Server.exe (REAP_SEC compressed 60->3 in worktree).
# Verdict: if Server.exe survives N iterations, the newly-live reap path does not crash.
import hashlib, os, socket, struct, subprocess, sys, time

HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_REG, T_CAP, T_CAP_R, T_LOGIN, T_LOGIN_R, T_PING, T_PONG = 1, 44, 45, 3, 4, 50, 51
T_UPINIT, T_UPDATA, T_UPINIT_R = 29, 31, 30
T_FC, T_FC_R = 97, 98
MAGIC = 0x5253554D
FCHK_MAGIC = 0x4643484B
FCHK_VERSION = 1
ID_OFF = 48
REPO = r"E:/qt/xiangmu/build/wt-n1-ab-cand"
DIST = os.path.join(REPO, "dist")
EXE = os.path.join(DIST, "Server.exe")
U = "e2etx1"
DST = "./filesys/e2etx1/tx1"
ITERS = int(sys.argv[1]) if len(sys.argv) > 1 else 10
IDLE_WAIT = float(sys.argv[2]) if len(sys.argv) > 2 else 17.0   # reap timer period 15s

SHA = lambda b: hashlib.sha256(b).hexdigest()
def f32(s): return s.encode("utf-8")[:31].ljust(32, b"\0")
def creds(n, p="123456"): return f32(n) + hashlib.md5(p.encode()).hexdigest().encode()
def pdu(t, msg=b"", dat=b""):
    dat = dat[:64].ljust(64, b"\0")
    return struct.pack("<III", HDR + len(msg), len(msg), t) + dat + msg
def u32(x): return struct.pack("<I", x)
def u64(x): return struct.pack("<Q", x)
def i64(x): return struct.pack("<q", x)

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
            s.pump(0.2); s.packets()
        return None
    def send(s, pkt): s.sock.sendall(pkt)
    def close(s):
        try: s.sock.close()
        except OSError: pass

def start_server():
    os.environ["PATH"] = r"C:/Qt/5.14.2/mingw73_64/bin;C:/Qt/Tools/mingw730_64/bin;" + os.environ["PATH"]
    subprocess.run(["taskkill", "/F", "/IM", "Server.exe"], capture_output=True)
    time.sleep(1.0)
    subprocess.Popen([EXE], cwd=DIST, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    dl = time.time() + 25
    while time.time() < dl:
        try:
            socket.create_connection((HOST, PORT), timeout=1).close(); return True
        except OSError: time.sleep(0.4)
    return False

def server_alive():
    out = subprocess.run(["tasklist"], capture_output=True,
                         text=True, encoding="gbk", errors="replace").stdout
    return "Server.exe" in (out or "")

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

def up_init(c, tid, dirPath, name, size):
    c.send(pdu(T_UPINIT, msg=dirPath.encode(),
               dat=f32(name) + i64(size) + u32(MAGIC) + b"\0" * 4 + u64(tid)))
    return c.wait(T_UPINIT_R)

def fastcheck(c, sha, name, dirPath, size, reqId):
    c.send(pdu(T_FC, msg=sha.encode() + name.encode() + dirPath.encode() + reqId.encode(),
               dat=u32(FCHK_VERSION) + u32(FCHK_MAGIC) + i64(size) + u32(64)
                   + u32(len(name.encode())) + u32(len(dirPath.encode())) + u32(len(reqId.encode()))))
    return c.wait(T_FC_R, 10)

def ping(c):
    c.send(pdu(T_PING)); return c.wait(T_PONG, 4)

if not start_server():
    print("FATAL: server start failed"); sys.exit(2)

crashed_at = None
for it in range(1, ITERS + 1):
    c, ret = login_conn(U)
    if ret != 1:
        print(f"iter {it}: login failed ret={ret}"); crashed_at = it; break
    # fill 3 upload slots with idle contexts (no data chunks -> pure idle victims)
    inits = [up_init(c, 7000 + it * 10 + k, DST, f"probe_{it}_{k}.bin", 800) for k in range(3)]
    # 97-miss -> id=0 placeholder with begun staging session (the N-1-timed slot)
    pl = f"probe{it}-".encode() + b"P" * 600
    fc = fastcheck(c, SHA(pl), f"probe_fc_{it}.bin", DST, len(pl), f"{int(time.time_ns()) % 10**16:016x}")
    alive0 = ping(c) is not None
    # silent window > reap threshold (compressed); reap timer (15s) will reap all 4 slots incl. id=0
    time.sleep(IDLE_WAIT)
    alive1 = server_alive()
    pong = None
    if alive1:
        pong = ping(c) is not None
    print(f"iter {it}: init={[r is not None and r[1][0] != 0 for r in inits]} "
          f"fc={fc is not None and fc[1][0] != 0 and fc[1][1] == 0} "
          f"alive_before_wait={alive0} proc_alive_after={alive1} pong={pong}")
    if not alive1 or pong is not True:
        crashed_at = it
        try: ping(c)
        except OSError as e: print(f"  socket error: {e}")
        break
    c.close()

if crashed_at is None:
    print(f"PROBE PASS: server survived {ITERS} iterations of the N-1 reap condition")
    subprocess.run(["taskkill", "/F", "/IM", "Server.exe"], capture_output=True)
    sys.exit(0)
else:
    print(f"PROBE FAIL: server died at iteration {crashed_at}")
    sys.exit(1)
