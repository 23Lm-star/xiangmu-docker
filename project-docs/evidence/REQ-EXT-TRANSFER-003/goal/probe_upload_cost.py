# 一次性取证探针（goal 节点）：拆分"每块固定开销"的来源——网络/事件循环 RTT vs 上传写盘路径。
# 心跳 50/51 给出裸 RTT 基线；1KB/4KB/16KB 上传块给出每块固定成本随块大小的变化。
import hashlib, os, random, socket, struct, subprocess, sys, time

HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_REG, T_CAP, T_CAP_R, T_LOGIN, T_LOGIN_R, T_PING, T_PONG = 1, 44, 45, 3, 4, 50, 51
T_UPINIT, T_UPDATA, T_UPINIT_R, T_UPDATA_R = 29, 31, 30, 32
MAGIC = 0x5253554d
ID_OFF = 48
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DIST = os.path.join(REPO, "dist")
sys.path.insert(0, os.path.join(REPO, "project-docs", "evidence", "_harness"))
import server_ctl
_SRV = server_ctl.ServerCtl(os.path.join(DIST, "Server.exe"), DIST, HOST, PORT)

OWNER = "e2eprobe"
ROOT = f"./filesys/{OWNER}"
NONCE = "%08x" % random.getrandbits(32)


def f32(s):
    return s.encode("utf-8")[:31].ljust(32, b"\0")


def creds(n, p="123456"):
    return f32(n) + hashlib.md5(p.encode()).hexdigest().encode()


def pdu(t, msg=b"", dat=b""):
    dat = dat[:64].ljust(64, b"\0")
    return struct.pack("<III", HDR + len(msg), len(msg), t) + dat + msg


def u32(x):
    return struct.pack("<I", x)


def u64(x):
    return struct.pack("<Q", x)


def i64(x):
    return struct.pack("<q", x)


class Conn:
    def __init__(s):
        s.sock = socket.create_connection((HOST, PORT), timeout=10)
        s.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        s.buf = b""
        s.pending = []

    def pump(s, t):
        s.sock.settimeout(t)
        try:
            d = s.sock.recv(1 << 20)
            if d:
                s.buf += d
        except (socket.timeout, OSError):
            pass

    def packets(s):
        while len(s.buf) >= HDR:
            tl, ml, ty = struct.unpack("<III", s.buf[:12])
            if tl < HDR or tl != HDR + ml or len(s.buf) < tl:
                break
            s.pending.append((ty, s.buf[12:76], s.buf[76:tl]))
            s.buf = s.buf[tl:]

    def wait(s, ty, timeout=20.0):
        dl = time.time() + timeout
        while time.time() < dl:
            for i, p in enumerate(s.pending):
                if p[0] == ty:
                    return s.pending.pop(i)
            s.pump(0.2)
            s.packets()
        return None

    def send(s, pkt):
        s.sock.sendall(pkt)

    def close(s):
        try:
            s.sock.close()
        except OSError:
            pass


def login_conn(name):
    c = Conn()
    c.send(pdu(T_CAP))
    r = c.wait(T_CAP_R)
    code = r[2].split(b"\0")[0] if r else b""
    c.send(pdu(T_REG, msg=code + b"\0", dat=creds(name)))
    c.wait(T_REG + 1, 4)
    c.close()
    c = Conn()
    c.send(pdu(T_CAP))
    r = c.wait(T_CAP_R)
    code = r[2].split(b"\0")[0] if r else b""
    c.send(pdu(T_LOGIN, msg=code + b"\0", dat=creds(name)))
    r = c.wait(T_LOGIN_R, 6)
    ret = struct.unpack("<i", r[1][:4])[0] if r else -99
    return c, ret


def up_init(c, tid, dirPath, name, size):
    c.send(pdu(T_UPINIT, msg=dirPath.encode(),
               dat=f32(name) + i64(size) + u32(MAGIC) + b"\0" * 4 + u64(tid)))
    return c.wait(T_UPINIT_R)


def main():
    if not _SRV.start():
        print("[E]", _SRV.last_error)
        return 2
    try:
        os.makedirs(os.path.join(DIST, "filesys", OWNER), exist_ok=True)
        c, ret = login_conn(OWNER)
        if ret != 1:
            print("[E] 登录失败 ret=%s" % ret)
            return 2

        # 裸 RTT 基线：心跳 50/51（无文件、无线程池）
        for _ in range(3):
            t0 = time.time()
            c.send(pdu(T_PING))
            c.wait(T_PONG, 5)
            print("[RTT] 心跳往返 %.2f ms" % ((time.time() - t0) * 1000))

        # 每块固定成本：块大小 1KB / 4KB / 16KB，各 256 块（总量 < 4MB）
        N = 256
        for chunk in (1024, 4096, 16308):
            name = "cost_%d_%s.bin" % (chunk, NONCE)
            size = chunk * N
            payload = os.urandom(size)
            tid = random.getrandbits(32) + 1
            r = up_init(c, tid, ROOT, name, size)
            if r is None or r[1][0] == 0:
                print("[E] init 失败 chunk=%d" % chunk)
                continue
            t0 = time.time()
            off = 0
            while off < size:
                a = c.wait(T_UPDATA_R) if False else None
                c.send(pdu(T_UPDATA, msg=payload[off:off + chunk],
                           dat=i64(off) + b"\0" * 32 + u32(MAGIC) + b"\0" * 4 + u64(tid)))
                a = c.wait(T_UPDATA_R)
                if a is None or a[1][0] == 0:
                    print("[E] chunk 失败 off=%d" % off)
                    break
                off += chunk
            dt = time.time() - t0
            print("[CHUNK] %6dB x%3d  总 %.2fs  每块 %.2f ms  吞吐 %.2f MiB/s"
                  % (chunk, N, dt, dt / N * 1000, (size / 1048576.0) / dt))
        c.close()
        return 0
    finally:
        _SRV.stop()


if __name__ == "__main__":
    sys.exit(main())
