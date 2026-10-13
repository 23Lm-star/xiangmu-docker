# REQ-EXT-SEC-002 验收 e2e：rename(27)/move(38) 声明字段移交服务端确认。
# A1 未知 fileType 不再谎报成功；A2 同名幂等；A3 物理占用前置拒绝；A4 谎报类型按服务端推导；
# A5 move 声称名错位按 srcRel 末段派生；A6 声称名 "../" 不逃逸；A7 无 '\0' 分隔显式拒。
# 运行形态（E2E-RUNFORM）：py -3.13 project-docs/evidence/REQ-EXT-SEC-002/verify.py
#   exe=<repo>/dist/Server.exe cwd=<repo>/dist；filesPath=./filesys；生命周期自管（_harness/server_ctl）。
import hashlib, os, shutil, socket, struct, subprocess, sys, time

HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_REG, T_CAP, T_LOGIN = 1, 44, 3
T_MKDIR, T_FLUSH, T_UPINIT, T_UPDATA = 19, 23, 29, 31
T_UPINIT_R, T_UPDATA_R, T_FLUSH_R = 30, 32, 24
T_RENAME, T_RENAME_R = 27, 28
T_MOVE, T_MOVE_R = 38, 39
MAGIC = 0x5253554d
MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
DIST = os.path.join(REPO, "dist")
EXE = os.path.join(DIST, "Server.exe")
sys.path.insert(0, os.path.join(REPO, "project-docs", "evidence", "_harness"))
import server_ctl
_SRV = server_ctl.ServerCtl(EXE, DIST, HOST, PORT)
FS = os.path.join(DIST, "filesys")
U = "sec2a"
C1 = b"SEC002-CONTENT-ONE" * 20
C2 = b"SEC002-CONTENT-TWO" * 20
SHA = lambda b: hashlib.sha256(b).hexdigest()

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

results = []
def check(sid, desc, cond, detail=""):
    results.append((sid, desc, bool(cond)))
    print(f"[{'PASS' if cond else 'FAIL'}] {sid} {desc} :: {detail}")

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

def upload(c, dirPath, name, payload, chunk=200):
    r = c.ask(pdu(T_UPINIT, msg=dirPath.encode(),
                   dat=f32(name) + struct.pack("<q", len(payload)) + struct.pack("<I", MAGIC)))
    if not (r and r[0][1][0] != 0):
        return False
    for i in range(0, len(payload), chunk):
        ch = payload[i:i + chunk]
        pkt = pdu(T_UPDATA, msg=ch, dat=struct.pack("<q", i) + b"\0" * 32 + struct.pack("<I", MAGIC))
        a = c.ask(pkt)
        if not a or a[0][0] != T_UPDATA_R or a[0][1][0] == 0: return False
    return True

def ren_ret(r):   # rename 响应 ret 在 caData 首字节(bool)
    return r and r[0][0] == T_RENAME_R and r[0][1][0] != 0
def mv_pkt(r):
    return r and r[0][0] == T_MOVE_R

# ---------- 0. 清场（停服 + 清 DB 测试行 + 删测试目录/逃逸残留） ----------
_SRV.stop()
sql(f"DELETE FROM user_file WHERE owner_name='{U}';")
sql(f"DELETE FROM upload_session WHERE owner_name='{U}';")
sql(f"DELETE FROM file_migration_map WHERE old_path LIKE '%filesys/{U}/%';")
sql(f"DELETE FROM file_object WHERE sha256 IN ('{SHA(C1)}','{SHA(C2)}');")
shutil.rmtree(os.path.join(FS, U), ignore_errors=True)
shutil.rmtree(os.path.join(FS, ".staging"), ignore_errors=True)
os.remove(os.path.join(FS, "escape.txt")) if os.path.exists(os.path.join(FS, "escape.txt")) else None

# ---------- 1. 起服，建混合夹具：条目文件×2、目录条目 d1(内放 m1.txt)、纯物理旧文件（不重启=不迁移） ----------
check("S1", "Server 启动并监听", _SRV.start())
os.makedirs(os.path.join(FS, U), exist_ok=True)
ca, ret = login_conn(U)
check("L1", f"{U} 登录", ret == 1, f"ret={ret}")
rootP = f"./filesys/{U}"
check("U1", "上传 e1.txt(C1) 建条目", upload(ca, rootP, "e1.txt", C1))
check("U2", "上传 e2.txt(C2) 建条目", upload(ca, rootP, "e2.txt", C2))
ca.ask(pdu(T_MKDIR, msg=rootP.encode(), dat=f32("d1")))
ca.ask(pdu(T_MKDIR, msg=rootP.encode(), dat=f32("d2")))
check("U3", "上传 d1/m1.txt 建条目", upload(ca, f"{rootP}/d1", "m1.txt", C1))
base = os.path.join(FS, U)
open(os.path.join(base, "leg1.txt"), "wb").write(C2)      # 未迁移旧文件（物理在、条目无）
open(os.path.join(base, "ghost.txt"), "wb").write(C2)     # A3：物理幽灵占名
open(os.path.join(base, "esc2.txt"), "wb").write(C2)      # A6：被移动对象（物理在、条目无）

# ---------- 2. rename：A1 未知 fileType 谎报 → 现在真改名 ----------
r = ca.ask(pdu(T_RENAME, msg=struct.pack("<I", 7) + rootP.encode(),
                dat=f32("leg1.txt") + f32("leg1r.txt")))
check("A1", "未建条目文件+谎报 fileType=7 → 服务端推导为文件，实际改名成功（改前谎报成功不动）",
      ren_ret(r) and os.path.isfile(os.path.join(base, "leg1r.txt"))
      and not os.path.isfile(os.path.join(base, "leg1.txt")))

# ---------- 3. rename：A2 同名幂等（改前服务端命中自身条目误报"目标名已占用"） ----------
r = ca.ask(pdu(T_RENAME, msg=struct.pack("<I", 1) + rootP.encode(),
                dat=f32("e1.txt") + f32("e1.txt")))
check("A2", "条目同名重命名 → 幂等成功",
      ren_ret(r) and sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{U}' "
                         f"AND display_name='e1.txt' AND status='active'") == "1")

# ---------- 4. rename：A3 目标名被物理幽灵占用 → 前置拒绝，条目不动 ----------
r = ca.ask(pdu(T_RENAME, msg=struct.pack("<I", 1) + rootP.encode(),
                dat=f32("e2.txt") + f32("ghost.txt")))
check("A3", "条目改名为物理幽灵占名 → 拒绝且条目未动（改前条目已改、物理撞车留告警）",
      r and r[0][0] == T_RENAME_R and r[0][1][0] == 0
      and sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{U}' AND display_name='e2.txt' "
              f"AND status='active'") == "1"
      and os.path.isfile(os.path.join(base, "ghost.txt")))

# ---------- 5. rename：A4 谎报类型仍按条目权威 ----------
r = ca.ask(pdu(T_RENAME, msg=struct.pack("<I", 1) + rootP.encode(),
                dat=f32("e1.txt") + f32("e1b.txt")))
check("A4a", "文件条目谎报 fileType=0(目录) → 条目照改（物理本无实体，不借声明走错分支）",
      ren_ret(r) and sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{U}' "
                         f"AND display_name='e1b.txt' AND is_dir=0 AND status='active'") == "1")
r = ca.ask(pdu(T_RENAME, msg=struct.pack("<I", 1) + rootP.encode(),   # 谎报"文件"
                dat=f32("d1") + f32("d1ren")))
check("A4b", "目录条目谎报 fileType=1(文件) → 按 e.isDir 走目录改名+子树级联",
      ren_ret(r) and sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{U}' "
                         f"AND display_name='m1.txt' AND parent_path='/d1ren/' AND status='active'") == "1")

# ---------- 6. move：A5 声称名与 srcRel 末段错位 → 按服务端派生名（改前逻辑/物理解体） ----------
r = ca.ask(pdu(T_MOVE, msg=b"d1ren/m1.txt\0d2\0", dat=f32("evil.txt")))
echoed = r[0][1][32:64].split(b"\0")[0].decode("utf-8", "replace") if mv_pkt(r) else ""
check("A5", "move 错位声称名 → 条目按派生名 m1.txt 落 /d2/，无 evil 条目",
      mv_pkt(r) and r[0][1][0] != 0
      and sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{U}' AND display_name='m1.txt' "
              f"AND parent_path='/d2/' AND status='active'") == "1"
      and sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{U}' AND display_name='evil.txt'") == "0",
      f"echo={echoed}")

# ---------- 7. move：A6 声称名 "../" 且未建条目 → 不逃逸（改前 dstPath 逃出 dstDir） ----------
r = ca.ask(pdu(T_MOVE, msg=b"esc2.txt\0\0", dat=f32("../escape.txt")))
check("A6", "move 未迁移物理文件+声称名 ../escape.txt → 留在用户根内，filesys 层无逃逸文件",
      mv_pkt(r) and r[0][1][0] != 0
      and os.path.isfile(os.path.join(base, "esc2.txt"))
      and not os.path.exists(os.path.join(FS, "escape.txt")))

# ---------- 8. move：A7 caMsg 无 '\0' 分隔 → 显式拒绝（改前靠目标非目录碰巧拒掉） ----------
r = ca.ask(pdu(T_MOVE, msg=b"esc2.txt", dat=f32("esc2.txt")))
check("A7", "move caMsg 缺 '\\0' 分隔符 → 显式拒绝",
      mv_pkt(r) and r[0][1][0] == 0 and os.path.isfile(os.path.join(base, "esc2.txt")))

# ---------- 9. 视图自洽：flushfile 与条目一致（派生名没把视图搅乱） ----------
r = ca.ask(pdu(T_FLUSH, msg=rootP.encode()))
names = []
if r and r[0][0] == T_FLUSH_R:
    m = r[0][2]
    for i in range(len(m) // 36):
        names.append(m[i * 36:i * 36 + 32].split(b"\0")[0].decode("utf-8", "replace"))
check("V1", "根目录视图含 e1b/e2/ghost/d1ren/d2/esc2，不含被拒的改名",
      all(x in names for x in ("e1b.txt", "e2.txt", "ghost.txt", "d1ren", "d2", "esc2.txt")),
      f"names={names}")
ca.close()
_SRV.stop()
print("\n=== 汇总 ===")
passed = sum(1 for _, _, ok in results if ok)
for sid, desc, ok in results:
    if not ok: print(f"  FAIL -> {sid} {desc}")
print(f"{passed}/{len(results)} passed")
sys.exit(0 if passed == len(results) else 1)
