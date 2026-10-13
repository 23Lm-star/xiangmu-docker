# REQ-UX-001 A2 定向探针：登录必须幂等恢复 actor 个人目录
#
# 判别性：先删除用户根目录（模拟"存量账号 + 新部署 filesPath"），再登录。
#   修复前：目录不存在 → isPathInSandbox 取 canonicalFilePath 为空 → 列为越权 → 文件功能全锁死；
#   修复后：登录时 mkpath 重建目录，flushfile 可正常列出条目。
# 运行：py -3.13 project-docs/evidence/REQ-UX-001/ux001_probe.py
import hashlib, os, shutil, socket, struct, subprocess, sys, time

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
DIST = os.path.join(REPO, "dist")
EXE = os.path.join(DIST, "Server.exe")
SBX = os.path.join(DIST, "filesys")
HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_REG, T_LOGIN, T_CAP, T_FLUSH, T_FLUSH_R = 1, 3, 44, 23, 24
PWD = "123456"
U = "uxprobe1"

def f32(s):
    return s.encode("utf-8")[:31].ljust(32, b"\0")

def pdu(t, msg=b"", dat=b""):
    dat = dat[:64].ljust(64, b"\0")
    return struct.pack("<III", HDR + len(msg), len(msg), t) + dat + msg

def creds(name, pwd=PWD):
    return f32(name) + hashlib.md5(pwd.encode()).hexdigest().encode()

class Conn:
    def __init__(s):
        s.sock = socket.create_connection((HOST, PORT), timeout=10); s.buf = b""
    def _pump(s, timeout):
        s.sock.settimeout(timeout)
        while True:
            try: d = s.sock.recv(65536)
            except socket.timeout: return
            if not d: return
            s.buf += d
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

def start_server():
    os.environ["PATH"] = r"C:/Qt/5.14.2/mingw73_64/bin;C:/Qt/Tools/mingw730_64/bin;" + os.environ["PATH"]
    subprocess.run(["taskkill", "/F", "/IM", "Server.exe"], capture_output=True); time.sleep(1.0)
    subprocess.Popen([EXE], cwd=DIST, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    dl = time.time() + 25
    while time.time() < dl:
        try: socket.create_connection((HOST, PORT), timeout=1).close(); return True
        except OSError: time.sleep(0.4)
    return False

def stop_server():
    subprocess.run(["taskkill", "/F", "/IM", "Server.exe"], capture_output=True); time.sleep(1.0)

def login_conn(name):
    c = Conn()
    r = c.ask(pdu(T_CAP)); code = r[0][2].split(b"\0")[0]
    r = c.ask(pdu(T_LOGIN, msg=code + b"\0", dat=creds(name)))
    return c, struct.unpack("<i", r[0][1][:4])[0]

def ensure_user(name):
    c = Conn()
    r = c.ask(pdu(T_CAP)); code = r[0][2].split(b"\0")[0]
    c.ask(pdu(T_REG, msg=code + b"\0", dat=creds(name))); c.close()
    c, ret = login_conn(name); c.close()
    if ret != 1:
        print(f"FATAL: 用户 {name} 既注册不上也登录不了 ret={ret}"); sys.exit(2)

results = []
def check(sid, desc, cond, detail):
    results.append((sid, bool(cond)))
    print(f"[{'PASS' if cond else 'FAIL'}] {sid} {desc} :: {detail}")

user_root = os.path.join(SBX, U)
if not start_server():
    print("FATAL: Server 启动失败"); sys.exit(2)
ensure_user(U)
stop_server()

# 制造缺陷条件：用户根目录缺失（存量账号在新部署 filesPath 下的真实状态）
shutil.rmtree(user_root, ignore_errors=True)
time.sleep(0.5)
check("A2a", "前置：用户根目录已被删除（缺陷条件成立）", not os.path.isdir(user_root), user_root)

if not start_server():
    print("FATAL: Server 启动失败"); sys.exit(2)
c, ret = login_conn(U)
check("A2b", "登录成功", ret == 1, f"ret={ret}")
check("A2c", "登录后用户根目录被幂等重建", os.path.isdir(user_root), user_root)

# 列表判别：在用户根内放一个物理文件，flushfile 应能列出（旧行为下整根被判越权，回空表）
probe_file = os.path.join(user_root, "hello.txt")
with open(probe_file, "w", encoding="utf-8") as f:
    f.write("ux001")
r = c.ask(pdu(T_FLUSH, msg=(f"./filesys/{U}").encode() + b"\0"))
ok_type = bool(r) and r[-1][0] == T_FLUSH_R
names = []
if ok_type:
    body = r[-1][2]
    for i in range(0, len(body) - 35, 36):
        names.append(body[i:i + 32].split(b"\0")[0].decode("utf-8", "replace"))
check("A2d", "flushfile 返回 FLUSHFILE_RESPOND", ok_type, f"types={[x[0] for x in r]}")
check("A2e", "flushfile 列出用户根内条目（不再整根越权回空）", "hello.txt" in names, f"names={names}")
c.close(); stop_server()

passed = sum(1 for _, ok in results if ok)
print(f"\n==== 汇总: {passed}/{len(results)} PASS ====")
sys.exit(0 if passed == len(results) else 1)
