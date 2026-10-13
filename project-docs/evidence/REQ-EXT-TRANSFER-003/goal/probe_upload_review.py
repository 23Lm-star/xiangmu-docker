# 一次性取证探针（goal 节点，非回归套件）：确认三条上传缺陷的事实面。
# 1) upload_session.confirmed_offset 在上传过程中是否前进
# 2) 暂停->续传是否新增 upload_session 行
# 3) 4KB 块 vs 协议上限内最大块(16308B) 的吞吐差
# 运行形态遵循 project-docs/evidence/E2E-RUNFORM.md；自管 dist/Server.exe（PID 级）。
import hashlib, os, random, socket, struct, subprocess, sys, time

HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_REG, T_CAP, T_CAP_R, T_LOGIN, T_LOGIN_R = 1, 44, 45, 3, 4
T_UPINIT, T_UPDATA, T_UPINIT_R, T_UPDATA_R = 29, 31, 30, 32
T_ABORT, T_ABORT_R = 107, 108
MAGIC = 0x5253554d
ID_OFF = 48
MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DIST = os.path.join(REPO, "dist")
EXE = os.path.join(DIST, "Server.exe")
sys.path.insert(0, os.path.join(REPO, "project-docs", "evidence", "_harness"))
import server_ctl
_SRV = server_ctl.ServerCtl(EXE, DIST, HOST, PORT)

OWNER = "e2eprobe"
ROOT = f"./filesys/{OWNER}"
NONCE = "%08x" % random.getrandbits(32)


def sql(q):
    return subprocess.run([MYSQL] + DB + [q], capture_output=True, text=True).stdout.strip()


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

    def wait(s, ty, timeout=15.0):
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


def up_chunk(c, tid, off, data):
    c.send(pdu(T_UPDATA, msg=data,
               dat=i64(off) + b"\0" * 32 + u32(MAGIC) + b"\0" * 4 + u64(tid)))
    return c.wait(T_UPDATA_R)


def abort(c, tid):
    c.send(pdu(T_ABORT, dat=b"\0" * ID_OFF + u64(tid)))
    return c.wait(T_ABORT_R)


def rows(name_like):
    return sql("SELECT session_id,status,confirmed_offset,expected_size FROM upload_session "
               "WHERE owner_name='%s' AND display_name LIKE '%s' ORDER BY session_id"
               % (OWNER, name_like))


def cleanup():
    sql("DELETE FROM upload_session WHERE owner_name='%s'" % OWNER)
    import shutil
    shutil.rmtree(os.path.join(DIST, "filesys", OWNER), ignore_errors=True)


def main():
    if not _SRV.start():
        print("[E]", _SRV.last_error)
        return 2
    try:
        c, ret = login_conn(OWNER)
        print("[i] login ret=%s" % ret)
        if ret != 1:
            print("[E] 登录失败，终止")
            return 2

        cleanup()
        # 用户根只在首次注册时创建；清场后必须补建（且必须在 login 之后清，避免删掉注册建出的根）
        os.makedirs(os.path.join(DIST, "filesys", OWNER), exist_ok=True)
        # ---------- A. 吞吐：4KB vs 上限内最大块 ----------
        for chunk, tag in ((4096, "4KB"), (16384 - HDR, "16KB(cap)")):
            name = "perf_%s_%s.bin" % (tag.replace("(", "_").replace(")", ""), NONCE)
            payload = os.urandom(8 << 20)
            tid = random.getrandbits(32) + 1
            r = up_init(c, tid, ROOT, name, len(payload))
            if r is None or r[1][0] == 0:
                print("[E] init 失败 chunk=%s" % chunk)
                continue
            t0 = time.time()
            off = 0
            while off < len(payload):
                a = up_chunk(c, tid, off, payload[off:off + chunk])
                if a is None or a[1][0] == 0:
                    print("[E] chunk 失败 off=%d" % off)
                    break
                off += chunk
            dt = time.time() - t0
            print("[A] chunk=%-9d %-9s 8MiB 用时 %.2fs  => %.2f MiB/s" % (chunk, tag, dt, 8.0 / dt))

        # ---------- B. 上传中的 confirmed_offset ----------
        name = "offprobe_%s.bin" % NONCE
        payload = os.urandom(2 << 20)
        tid = random.getrandbits(32) + 1
        r = up_init(c, tid, ROOT, name, len(payload))
        print("[B] INIT ret=%s tid=%d" % (r[1][0] if r else None, tid))
        off = 0
        samples = []
        while off < len(payload):
            a = up_chunk(c, tid, off, payload[off:off + 4096])
            off += 4096
            if off == (512 << 10) or off == (1024 << 10) or off == (1536 << 10):
                samples.append((off, rows(name + "%")))
        print("[B] 上传中(客户端已发字节 -> DB 行):")
        for sent, rs in samples:
            print("      已发 %8d  行=%s" % (sent, rs.replace("\n", " | ") or "(无)"))

        # ---------- C. 暂停（真中途）-> 续传：会话行如何变化 ----------
        name = "pauseprobe_%s.bin" % NONCE
        payload = os.urandom(2 << 20)
        tid = random.getrandbits(32) + 1
        r = up_init(c, tid, ROOT, name, len(payload))
        print("[C] INIT ret=%s tid=%d" % (r[1][0] if r else None, tid))
        off = 0
        while off < (1 << 20):                       # 只发一半，留出"中途"状态
            a = up_chunk(c, tid, off, payload[off:off + 4096])
            if a is None or a[1][0] == 0:
                print("[C] chunk 失败 off=%d" % off)
                break
            off += 4096
        print("[C] 中途(已发 %d) 行=%s" % (off, rows(name + "%").replace("\n", " | ") or "(无)"))
        ra = abort(c, tid)
        print("[C] 107 ret=%s" % (ra[1][0] if ra else None))
        time.sleep(0.5)
        after_abort = rows(name + "%")
        print("[C] 暂停后 行=%s" % (after_abort.replace("\n", " | ") or "(无)"))
        tid2 = random.getrandbits(32) + 1
        r2 = up_init(c, tid2, ROOT, name, len(payload))
        off2 = struct.unpack("<q", r2[1][32:40])[0] if r2 else -1
        print("[C] 续传 INIT(ret=%s, resumeOffset=%d, newTid=%d)" % (r2[1][0] if r2 else None, off2, tid2))
        after_resume = rows(name + "%")
        print("[C] 续传后 行=%s" % (after_resume.replace("\n", " | ") or "(无)"))
        print("[C] 行数变化: %d -> %d" % (len(after_abort.splitlines()), len(after_resume.splitlines())))
        # 续传收尾，便于观察最终行状态
        while off2 < len(payload):
            a = up_chunk(c, tid2, off2, payload[off2:off2 + 4096])
            if a is None or a[1][0] == 0:
                print("[C] 续传 chunk 失败 off=%d" % off2)
                break
            off2 += 4096
        print("[C] 完成后 行=%s" % (rows(name + "%").replace("\n", " | ") or "(无)"))
        c.close()
        print("[i] 清理: " + (rows("%") or "(空)"))
        return 0
    finally:
        _SRV.stop()


if __name__ == "__main__":
    sys.exit(main())
