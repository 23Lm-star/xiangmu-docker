# REQ-EXT-FILE-001 验收1/2/3 端到端脚本（证据归档，仿 SEC-001 verify.py 模式）。
# 复用 SEC-001 PDU 构包。断言走 DB：同内容两次上传 -> 1 物理对象 / 2 逻辑条目 / refcount=2；
# 删一引用 -> refcount=1 对象仍 ready 且另一条目可下载。
#
# ---- 统一 e2e 运行形态（五套件一致，见 project-docs/evidence/E2E-RUNFORM.md）----
# 运行：py -3.13 project-docs/evidence/REQ-EXT-FILE-001/dedup_e2e.py
#   cwd     : 任意（脚本自管理，Server 以 <repo>/dist 为 cwd 启动）
#   exe     : <repo>/dist/Server.exe（qmake DESTDIR=dist；connect.config 构建期经 qrc 内嵌）
#   config  : Server/connect.config -> filesPath=./filesys（相对 cwd）；未入库的机器本地文件
#   数据根  : <repo>/dist/filesys
#   未入库依赖：dist/{Qt5Core,Qt5Gui,Qt5Network,Qt5Sql,Qt5Svg,Qt5Widgets}.dll、dist/libmysql.dll、
#              dist/sqldrivers/qsqlmysql.dll、dist/platforms/qwindows.dll
# 脚本自管理 Server 生命周期（taskkill + Popen, cwd=<repo>/dist）。
import hashlib, os, socket, struct, subprocess, sys, time

HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_REG, T_CAP, T_LOGIN = 1, 44, 3
T_MKDIR, T_FLUSH, T_DEL, T_UPINIT, T_UPDATA, T_DOWN = 19, 23, 25, 29, 31, 40
T_UPINIT_R, T_UPDATA_R, T_DOWN_R = 30, 32, 41
MAGIC = 0x5253554d
MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
A = "dedupuser"
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
DIST = os.path.join(REPO, "dist")
EXE = os.path.join(DIST, "Server.exe")
SBX = os.path.join(DIST, "filesys")
sys.path.insert(0, os.path.join(REPO, "project-docs", "evidence", "_harness"))
import server_ctl

def f32(s): return s.encode("utf-8")[:31].ljust(32, b"\0")
def creds(name, pwd="123456"):
    return f32(name) + hashlib.md5(pwd.encode()).hexdigest().encode()
def pdu(t, msg=b"", dat=b""):
    dat = dat[:64].ljust(64, b"\0")
    return struct.pack("<III", HDR + len(msg), len(msg), t) + dat + msg

class Conn:
    def __init__(s):
        s.sock = socket.create_connection((HOST, PORT), timeout=10); s.buf = b""
    def _pump(s, t):
        s.sock.settimeout(t)
        try:
            d = s.sock.recv(65536)
            if d: s.buf += d
        except socket.timeout: return
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

def sql(q):
    r = subprocess.run([MYSQL] + DB + [q], capture_output=True, text=True)
    return r.stdout.strip()

_SRV = server_ctl.ServerCtl(EXE, DIST, HOST, PORT)

def start_server():
    # FINDING-GATE-1: 原来是 taskkill /F /IM Server.exe —— 按映像名杀，会打到并行套件的
    # 进程（实测造成三次"服务端突然消失"的伪缺陷）。现在只按自己启动的 PID 管理；端口被
    # 别人占用时直接报错，不盲杀。
    # 契约保持原样：返回 bool。调用点 (start_server() -> if not ...) 不因此改变。
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

# 注册 + 登录（带验证码）
def login_conn(name):
    c = Conn()
    r = c.ask(pdu(T_CAP))
    code = r[0][2].split(b"\0")[0]
    c.ask(pdu(T_REG, msg=code + b"\0", dat=creds(name)))
    c.close()
    c = Conn()
    r = c.ask(pdu(T_CAP)); code = r[0][2].split(b"\0")[0]
    r = c.ask(pdu(T_LOGIN, msg=code + b"\0", dat=creds(name)))
    ret = struct.unpack("<i", r[0][1][:4])[0]
    return c, ret

# FK 安全清理：先删本测试用户全部逻辑条目，再删本测试涉及的两个内容对象
stop_server()
sha = hashlib.sha256((b"DEDUP-Payload-CONTENT-12345" * 40)).hexdigest()
sha2 = hashlib.sha256((b"OTHER-DISTINCT-CONTENT" * 30)).hexdigest()
sql(f"DELETE FROM user_file WHERE owner_name='{A}';")
sql(f"DELETE FROM file_object WHERE sha256 IN ('{sha}','{sha2}');")
sql(f"DELETE FROM upload_session WHERE owner_name='{A}';")

if not start_server():
    print("FAIL S0 Server 启动并监听 5000"); sys.exit(2)
# 夹具：服务端用户根只在"首次注册"时创建；账号在多轮运行后已在 DB 中，故显式准备（同 entry_ops/fastupload）
os.makedirs(os.path.join(SBX, A), exist_ok=True)
ca, ret = login_conn(A)
check("L0", "dedupuser 登录成功", ret == 1, f"ret={ret}")
if ret != 1: sys.exit(2)
ca.ask(pdu(T_MKDIR, msg=f"./filesys/{A}".encode(), dat=f32("dd")))

def upload(name, payload):
    r = ca.ask(pdu(T_UPINIT, msg=f"./filesys/{A}/dd".encode(),
                   dat=f32(name) + struct.pack("<q", len(payload)) + struct.pack("<I", MAGIC)))
    if not (r and r[0][1][0] != 0):
        return False
    for i in range(0, len(payload), 600):
        ch = payload[i:i + 600]
        pkt = pdu(T_UPDATA, msg=ch, dat=struct.pack("<q", i) + b"\0"*32 + struct.pack("<I", MAGIC))
        a = ca.ask(pkt)
        if not a or a[0][0] != T_UPDATA_R or a[0][1][0] == 0: return False
    return True

payload = b"DEDUP-Payload-CONTENT-12345" * 40
sha = hashlib.sha256(payload).hexdigest()

print("=== 验收1：同内容不同名 -> 一个物理对象 ===")
check("A1-up1", "上传 a.txt 成功", upload("a.txt", payload), "")
check("A1-up2", "上传 b.txt(同内容) 成功", upload("b.txt", payload), "")
nobj = sql(f"SELECT COUNT(*) FROM file_object WHERE sha256='{sha}'")
nentries = sql(f"SELECT COUNT(*) FROM user_file uf JOIN file_object fo ON fo.object_id=uf.object_id WHERE fo.sha256='{sha}' AND uf.owner_name='{A}' AND uf.status='active'")
nref = sql(f"SELECT refcount FROM file_object WHERE sha256='{sha}'")
check("A1-one-object", f"sha256 命中唯一物理对象 count=1", nobj == "1", f"objects={nobj}")
check("A1-two-entries", f"同一对象两条逻辑条目 count=2", nentries == "2", f"entries={nentries}")
check("A1-refcount", f"引用计数 refcount=2", nref == "2", f"refcount={nref}")
spath = sql(f"SELECT storage_path FROM file_object WHERE sha256='{sha}'")
check("A1-objects-dir", "对象按内容寻址落在 objects/{sha2}/ 下", spath.startswith("objects/") and "/" in spath[8:], f"path={spath}")

print("=== 验收2：不同内容 -> 各自独立对象 ===")
p2 = b"OTHER-DISTINCT-CONTENT" * 30
sha2 = hashlib.sha256(p2).hexdigest()
check("A2-up3", "上传 c.txt(不同内容) 成功", upload("c.txt", p2), "")
check("A2-sep-obj", "不同内容新建独立对象", sql(f"SELECT COUNT(*) FROM file_object WHERE sha256='{sha2}'") == "1", "")

print("=== 验收3：删一引用不影响另一引用；归零前不物理删 ===")
objfile_before = sql(f"SELECT storage_path FROM file_object WHERE sha256='{sha}'")
ca.ask(pdu(T_DEL, msg=f"./filesys/{A}/dd/a.txt".encode(), dat=struct.pack("<I", 1)))
nref2 = sql(f"SELECT refcount FROM file_object WHERE sha256='{sha}'")
stat2 = sql(f"SELECT status FROM file_object WHERE sha256='{sha}'")
check("A3-refcount-1", f"删一条目后 refcount=1", nref2 == "1", f"refcount={nref2}")
check("A3-still-ready", "另一引用仍在，对象保持 ready", stat2 == "ready", f"status={stat2}")
# 另一条目仍可下载
r = ca.ask(pdu(T_DOWN, msg=f"./filesys/{A}/dd/b.txt".encode(), dat=f32("b.txt")))
ok_down = r and r[0][1][0] != 0
check("A3-other-downloadable", "未删条目 b.txt 仍可下载", ok_down, f"ret_byte={r[0][1][0] if r else 'NORESP'}")
objfile_after = sql(f"SELECT storage_path FROM file_object WHERE sha256='{sha}'")
check("A3-no-physical-del", "对象未被删除且 storage_path 不变(D3)",
      objfile_after != "" and objfile_after == objfile_before, f"before={objfile_before} after={objfile_after}")

ca.close()
stop_server()
print("\n=== 汇总 ===")
passed = sum(1 for _, _, ok in results if ok)
for sid, desc, ok in results:
    if not ok: print(f"  FAIL -> {sid} {desc}")
print(f"{passed}/{len(results)} passed")
sys.exit(0 if passed == len(results) else 1)
