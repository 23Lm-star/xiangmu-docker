# 别名句柄必要性对照探针（独立测试节点的负对照/正对照，两条断言之外的一次性实验）。
# 场景 = T3-03 的核心：同一连接上 97 预检查槽（id=0）与传输槽（id=tid）各持一份**同一 .part** 的句柄，
# 传输完成后走 finalizeUploadOnComplete 的 rename。
#   负对照二进制 = 在 b861351 之上仅去掉 finalize 路径的 releaseOtherStagingHandles 调用（其余全同）
#   正对照二进制 = b861351 原样
# 用法：py -3.13 build/negctl_probe.py <exe> <expect: done|failed>
import hashlib, os, socket, struct, subprocess, sys, time

HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_CAP, T_CAP_R, T_REG, T_LOGIN, T_LOGIN_R = 44, 45, 1, 3, 4
T_UPINIT, T_UPDATA, T_UPINIT_R, T_UPDATA_R = 29, 31, 30, 32
T_FC, T_FC_R = 97, 98
T_DOWN, T_DOWN_R, T_DATA, T_FINISH = 40, 41, 42, 43
MAGIC = 0x5253554d
FCHK_MAGIC, FCHK_VERSION = 0x4643484b, 1
ID_OFF = 48
MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DIST = os.path.join(REPO, "dist")
EXE = sys.argv[1] if len(sys.argv) > 1 else os.path.join(DIST, "Server.exe")
EXPECT = sys.argv[2] if len(sys.argv) > 2 else "done"
sys.path.insert(0, os.path.join(REPO, "project-docs", "evidence", "_harness"))
import server_ctl
_SRV = server_ctl.ServerCtl(EXE, DIST, HOST, PORT)

U = "e2etx3"
DST = f"./filesys/{U}/tx3"
NAME = "negctl_alias.bin"
CHUNK = 1024 * 1024
# 每轮内容不同：避免 97 因"同 sha 对象已在库"直接秒传命中，从而污染单行会话的观测
payload = (os.urandom(16) + b"NEGCTL" + bytes(range(256)) * (2 * CHUNK // 256 + 1))[:2 * CHUNK]


def f32(s): return s.encode("utf-8")[:31].ljust(32, b"\0")
def creds(n, p="123456"): return f32(n) + hashlib.md5(p.encode()).hexdigest().encode()
def pdu(t, msg=b"", dat=b""):
    dat = dat[:64].ljust(64, b"\0")
    return struct.pack("<III", HDR + len(msg), len(msg), t) + dat + msg
def u16(x): return struct.pack("<H", x)
def u32(x): return struct.pack("<I", x)
def u64(x): return struct.pack("<Q", x)
def i64(x): return struct.pack("<q", x)


class Conn:
    def __init__(s):
        s.sock = socket.create_connection((HOST, PORT), timeout=10); s.buf = b""

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

    def send(s, p): s.sock.sendall(p)

    def close(s):
        try: s.sock.close()
        except OSError: pass


def sql(q):
    return subprocess.run([MYSQL] + DB + [q], capture_output=True, text=True).stdout.strip()


def login(name):
    c = Conn()
    c.send(pdu(T_CAP)); r = c.wait(T_CAP_R); code = r[2].split(b"\0")[0] if r else b""
    c.send(pdu(T_REG, msg=code + b"\0", dat=creds(name))); c.wait(T_REG + 1, 4); c.close()
    c = Conn()
    c.send(pdu(T_CAP)); r = c.wait(T_CAP_R); code = r[2].split(b"\0")[0] if r else b""
    c.send(pdu(T_LOGIN, msg=code + b"\0", dat=creds(name))); r = c.wait(T_LOGIN_R, 6)
    return c, (struct.unpack("<i", r[1][:4])[0] if r else -99)


def up_init(c, tid):
    c.send(pdu(T_UPINIT, msg=DST.encode(), dat=f32(NAME) + i64(len(payload)) + u32(MAGIC) + b"\0" * 4 + u64(tid)))
    return c.wait(T_UPINIT_R)


def up_chunk(c, tid, off, data):
    c.send(pdu(T_UPDATA, msg=data, dat=i64(off) + b"\0" * 32 + u32(MAGIC) + b"\0" * 4 + u64(tid)))
    return c.wait(T_UPDATA_R, 15)


def fc(c, req_id, sha):
    nb, db, rb = NAME.encode(), DST.encode(), req_id.encode()
    c.send(pdu(T_FC, msg=sha.encode() + nb + db + rb,
               dat=u32(FCHK_VERSION) + u32(FCHK_MAGIC) + i64(len(payload)) + u32(64)
                   + u32(len(nb)) + u32(len(db)) + u32(len(rb))))
    return c.wait(T_FC_R, 10)


def download(c, path, name):
    c.send(pdu(T_DOWN, msg=path.encode(), dat=f32(name) + b"\0" * 16 + u64(9101)))
    data, ok, fin = b"", None, None
    dl = time.time() + 30
    while time.time() < dl:
        for ty, dat, msg in c.packets():
            if ty == T_DOWN_R: ok = dat[0] != 0
            elif ty == T_DATA: data += msg
            elif ty == T_FINISH: fin = dat[0] != 0
        if fin is not None: break
        c.pump(0.2)
    return ok, data, fin


res = []
def check(sid, desc, cond, detail=""):
    res.append(bool(cond))
    print(f"[{'PASS' if cond else 'FAIL'}] {sid} {desc} :: {detail}", flush=True)


print(f"=== exe={EXE} expect={EXPECT} ===")
if not _SRV.start():
    print(f"FATAL: {_SRV.last_error}"); sys.exit(2)
os.makedirs(os.path.join(DIST, "filesys", U, "tx3"), exist_ok=True)
sql(f"DELETE FROM user_file WHERE owner_name='{U}' AND display_name='{NAME}';")
sql(f"DELETE FROM upload_session WHERE owner_name='{U}' AND display_name='{NAME}';")
shutil_staging = os.path.join(DIST, "filesys", ".staging")
if os.path.isdir(shutil_staging):
    import glob
    for f in glob.glob(os.path.join(shutil_staging, "*")):
        try: os.remove(f)
        except OSError: pass

c, ret = login(U)
check("N0", "登录", ret == 1, f"ret={ret}")

sha = hashlib.sha256(payload).hexdigest()
r = fc(c, "n1" * 16, sha)                     # 97：建 id=0 预检查槽（持 .part 句柄）
check("N1", "97 建预检查槽", r is not None and r[1][0] != 0, "")
r = up_init(c, 3001)                          # 29：真实传输槽（同一 .part 第二个句柄）
check("N2", "29 INIT（同一暂存）", r is not None and r[1][0] != 0, "")
a = up_chunk(c, 3001, 0, payload[:CHUNK])
check("N3", "首块 1 MiB 接受", a is not None and a[1][0] != 0, "")
r = fc(c, "n2" * 16, sha)                     # 97 再来一次：按暂存键复用行
check("N4", "97 复用未完成行", r is not None and r[1][0] != 0, "")
a = up_chunk(c, 3001, CHUNK, payload[CHUNK:])  # 完成 → finalize → rename
check("N5", "末块提交（32 回执 ok）", a is not None and a[1][0] != 0, f"complete={a[1][16] if a else '?'}")
time.sleep(1.0)
st = sql(f"SELECT session_id,status,confirmed_offset FROM upload_session WHERE owner_name='{U}' "
         f"AND display_name='{NAME}' ORDER BY session_id")
obj = sql(f"SELECT fo.size_bytes FROM user_file uf JOIN file_object fo ON fo.object_id=uf.object_id "
          f"WHERE uf.owner_name='{U}' AND uf.display_name='{NAME}' AND uf.status='active'")
ok, data, fin = download(c, f"{DST}/{NAME}", NAME)
final_status = st.splitlines()[-1].split("\t")[1] if st else "none"
nrows = len(st.splitlines()) if st else 0
print(f"    DB: {st!r}  rows={nrows}")
print(f"    object: {obj!r}  download: ok={ok} fin={fin} len={len(data)} bytes_equal={data == payload}")
verdict = (final_status == "done" and data == payload and nrows == 1) if EXPECT == "done" else (final_status != "done")
check("N6", f"对照判定（expect={EXPECT}）", verdict, f"status={final_status} rows={nrows}")
c.close()
_SRV.stop()
print(f"=== {sum(res)}/{len(res)} ===")
sys.exit(0 if all(res) else 1)
