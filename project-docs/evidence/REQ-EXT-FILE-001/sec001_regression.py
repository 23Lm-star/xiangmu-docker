# REQ-EXT-SEC-001 端到端负向/回归 harness（REQ-EXT-FILE-001 D5 适配副本，48 用例数不变）
# 适配范围：安全断言零改动；仅 K1c/K1f/K2a/K2b 的"物理落盘"期望（D5 批准前的旧语义）
#   替换为"条目归属+对象摘要"等价断言（上传内容寻址、分享物理零复制——owner 批准 D5，task.md 记录）。
# 原件保持不动：project-docs/evidence/REQ-EXT-SEC-001/verify.py
#
# ---- 统一 e2e 运行形态（五套件一致，见 project-docs/evidence/E2E-RUNFORM.md）----
# 运行：py -3.13 project-docs/evidence/REQ-EXT-FILE-001/sec001_regression.py
#   cwd     : 任意（脚本自管理，Server 以 <repo>/dist 为 cwd 启动）
#   exe     : <repo>/dist/Server.exe（qmake DESTDIR=dist；connect.config 构建期经 qrc 内嵌）
#   config  : Server/connect.config -> filesPath=./filesys（相对 cwd）；未入库的机器本地文件
#   数据根  : <repo>/dist/filesys
#   未入库依赖：dist/{Qt5Core,Qt5Gui,Qt5Network,Qt5Sql,Qt5Svg,Qt5Widgets}.dll、dist/libmysql.dll、
#              dist/sqldrivers/qsqlmysql.dll、dist/platforms/qwindows.dll
# 脚本自管理 Server 生命周期（taskkill + Popen, cwd=<repo>/dist）。
import hashlib, os, socket, struct, subprocess, sys, time

MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DBQ = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
def sql(q):
    r = subprocess.run([MYSQL] + DBQ + [q], capture_output=True, text=True)
    return r.stdout.strip()

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
DIST = os.path.join(REPO, "dist")
EXE = os.path.join(DIST, "Server.exe")
sys.path.insert(0, os.path.join(REPO, "project-docs", "evidence", "_harness"))
import server_ctl
SBX = os.path.join(DIST, "filesys")
HOST, PORT = "127.0.0.1", 5000
_SRV = server_ctl.ServerCtl(EXE, DIST, HOST, PORT)   # 必须在 HOST/PORT 之后构造
HDR = 76
T_REG, T_REG_R, T_LOGIN, T_LOGIN_R = 1, 2, 3, 4
T_FINDU, T_FINDU_R, T_ONLINE, T_ONLINE_R = 5, 6, 7, 8
T_ADDF, T_ADDF_R, T_FLUSHF, T_FLUSHF_R = 9, 10, 11, 12
T_DELFR, T_DELFR_R, T_CHAT = 13, 14, 15
T_GETMSG, T_GETMSG_R = 17, 18
T_MKDIR, T_MKDIR_R, T_CHGDIR, T_CHGDIR_R = 19, 20, 21, 22
T_FLUSH, T_FLUSH_R, T_DEL, T_DEL_R = 23, 24, 25, 26
T_REN, T_REN_R, T_UPINIT, T_UPINIT_R = 27, 28, 29, 30
T_UPDATA, T_UPDATA_R = 31, 32
T_SHARE, T_SHARE_R, T_AGREE, T_AGREE_R, T_FINISH = 33, 34, 35, 36, 37
T_MOVE, T_MOVE_R, T_DOWN, T_DOWN_R, T_DDOWN, T_DFIN = 38, 39, 40, 41, 42, 43
T_CAP, T_CAP_R, T_UI, T_UI_R = 44, 45, 46, 47
T_LOGOFF, T_LOGOFF_R, T_HB = 48, 49, 50
T_CPL, T_CPL_R, T_QFL, T_QFL_R, T_LIST, T_LIST_R = 52, 53, 56, 57, 58, 59
MAGIC = 0x5253554d
PWD = "123456"
A, B, R, V = "sec001a", "sec001b", "sec001r", "sec001v"

def f32(s):
    return s.encode("utf-8")[:31].ljust(32, b"\0")

def pdu(t, msg=b"", dat=b""):
    dat = dat[:64].ljust(64, b"\0")
    return struct.pack("<III", HDR + len(msg), len(msg), t) + dat + msg

def creds(name, pwd=PWD):
    return f32(name) + hashlib.md5(pwd.encode()).hexdigest().encode()

class Conn:
    def __init__(s):
        s.sock = socket.create_connection((HOST, PORT), timeout=10)
        s.buf = b""
    def _pump(s, timeout):
        s.sock.settimeout(timeout)
        while True:
            try:
                d = s.sock.recv(65536)
            except socket.timeout:
                return
            if not d:
                return
            s.buf += d
    def packets(s):
        out = []
        while len(s.buf) >= HDR:
            tl, ml, ty = struct.unpack("<III", s.buf[:12])
            if tl < HDR or tl != HDR + ml or len(s.buf) < tl:
                break
            out.append((ty, s.buf[12:76], s.buf[76:tl]))
            s.buf = s.buf[tl:]
        return out
    def ask(s, pkt, n=1, t=4.0):
        s.sock.sendall(pkt)
        got = []
        deadline = time.time() + t
        while len(got) < n and time.time() < deadline:
            s._pump(0.5)
            got = s.packets()
        return got
    def idle(s, t=0.6):
        s._pump(t)
        return s.packets()
    def close(s):
        try:
            s.sock.close()
        except OSError:
            pass

def start_server():
    # FINDING-GATE-1: 原来是 taskkill /F /IM Server.exe —— 按映像名杀，会打到并行套件的
    # 进程（实测造成三次"服务端突然消失"的伪缺陷）。现在只按自己启动的 PID 管理；端口被
    # 别人占用时直接报错，不盲杀。契约不变：仍返回 bool。
    if not _SRV.start():
        print(f"[E] {_SRV.last_error}")
        return False
    return True

def stop_server():
    _SRV.stop()

results = []
def check(sid, desc, cond, detail):
    results.append((sid, desc, bool(cond)))
    print(f"[{'PASS' if cond else 'FAIL'}] {sid} {desc} :: {detail}")

def bf(d, off=0):
    return d[off] != 0

def fresh_conn():
    return Conn()

def login_conn(name, pwd=PWD):
    c = Conn()
    r = c.ask(pdu(T_CAP))
    code = r[0][2].split(b"\0")[0]
    r = c.ask(pdu(T_LOGIN, msg=code + b"\0", dat=creds(name, pwd)))
    ret = struct.unpack("<i", r[0][1][:4])[0]
    return c, ret

def ensure_user(name):
    c = Conn()
    r = c.ask(pdu(T_CAP))
    code = r[0][2].split(b"\0")[0]
    r = c.ask(pdu(T_REG, msg=code + b"\0", dat=creds(name)))
    c.close()
    c, ret = login_conn(name)
    if ret == 1:
        c.close()
        return
    c.close()
    print(f"FATAL: 用户 {name} 既注册不上也登录不了 ret={ret}")
    sys.exit(2)

print("=== 0. DB 清场（F-2 修复：他树/上轮残留不得以清场豁免冒充 PASS） ===")
if not start_server():
    print("FATAL: Server 启动失败"); sys.exit(2)
for u in (A, B, R, V):
    sql(f"DELETE FROM user_file WHERE owner_name='{u}';")
    sql(f"DELETE FROM upload_session WHERE owner_name='{u}';")
    sql(f"DELETE FROM share_link WHERE owner_name='{u}';")
    sql(f"DELETE FROM file_migration_map WHERE old_path LIKE 'filesys/{u}/%' "
        f"OR old_path LIKE '%/filesys/{u}/%';")
sql("DELETE fo FROM file_object fo LEFT JOIN user_file uf ON uf.object_id=fo.object_id WHERE uf.file_id IS NULL;")

for u in (A, B, R, V):
    ensure_user(u)
# 夹具：服务端用户根只在"首次注册"时创建；账号在多轮运行后已在 DB 中，故显式预建（运行环境事实，
# 与 entry_ops/fastupload/dedup 同式）。R 的根由注销用例删除、V 的根由 X3c 校验存在。
for u in (A, B, R, V):
    os.makedirs(os.path.join(SBX, u), exist_ok=True)

import shutil

def rmtree_retry(path):
    # taskkill 后句柄可能瞬态占用目录：重试保证清场确定性（F-2 修复的一部分）
    for _ in range(10):
        shutil.rmtree(path, ignore_errors=True)
        if not os.path.isdir(path):
            return
        time.sleep(0.5)

def remove_retry(path):
    for _ in range(10):
        if not os.path.isfile(path):
            return
        try:
            os.remove(path)
        except OSError:
            pass
        time.sleep(0.5)

wd = os.path.join(SBX, A, "workdir")
rmtree_retry(wd)
for leftover in (os.path.join(SBX, B, "up.bin"), os.path.join(SBX, B, "up2.bin")):
    remove_retry(leftover)

secret = os.path.join(SBX, B, "secret.txt")
os.makedirs(os.path.join(SBX, B), exist_ok=True)
open(secret, "w").write("VICTIM-CONTENT-DO-NOT-TOUCH")

print("=== G1 未认证连接拒绝（验收1/4-群留待GROUP分支 D5） ===")
probes = [
    (T_MKDIR, "mkdir", pdu(T_MKDIR, msg=(f"./filesys/{A}").encode(), dat=f32("g1dir")), T_MKDIR_R),
    (T_UPINIT, "upload-init", pdu(T_UPINIT, msg=(f"./filesys/{A}").encode(), dat=f32("g1.bin") + struct.pack("<q", 4)), T_UPINIT_R),
    (T_FLUSH, "flushfile", pdu(T_FLUSH, msg=(f"./filesys/{B}").encode()), T_FLUSH_R),
    (T_DEL, "delfile", pdu(T_DEL, msg=secret.encode(), dat=struct.pack("<I", 1)), T_DEL_R),
    (T_DOWN, "download", pdu(T_DOWN, msg=secret.encode(), dat=f32("secret.txt")), T_DOWN_R),
    (T_LIST, "listdir", pdu(T_LIST, msg=b"\0"), T_LIST_R),
    (T_QFL, "query-publink", pdu(T_QFL, msg=f32("x.txt"), dat=f32(B)), T_QFL_R),
    (T_ONLINE, "onlineuser", pdu(T_ONLINE), T_ONLINE_R),
]
for t, label, pkt, rt in probes:
    c = fresh_conn()
    r = c.ask(pkt)
    ok = r and r[0][0] == rt and (r[0][2] == b"" or bf(r[0][1]) == False)
    check(f"G1-{label}", f"未认证 {label} 被拒且失败响应", ok, f"resp={r[0][0] if r else 'NONE'}")
    c.close()
c = fresh_conn()
c.sock.sendall(pdu(T_CHAT, msg=b"\0" * 68, dat=f32(V) + f32(A)))
c.idle(0.5)
check("G1-chat", "未认证 chat 被拒（无响应，服务端日志记录）", True, "见 AUTHGATE type=15 日志")
c.close()
time.sleep(0.3)

print("=== L1 登录绑定后本人操作正常（验收4回归） ===")
ca, ret = login_conn(A)
check("L1a", f"{A} 登录成功 ret=1", ret == 1, f"ret={ret}")
r = ca.ask(pdu(T_MKDIR, msg=(f"./filesys/{A}").encode(), dat=f32("workdir")))
check("L1b", "登录后本人根内 MKDIR 成功", r and r[0][0] == T_MKDIR_R and bf(r[0][1]), f"ret={bf(r[0][1]) if r else 'NORESP'}")
r = ca.ask(pdu(T_FLUSH, msg=(f"./filesys/{A}").encode()))
check("L1c", "登录后本人根 FLUSHFILE 有条目", r and r[0][0] == T_FLUSH_R and len(r[0][2]) > 0, f"msg_len={len(r[0][2]) if r else 0}")

print("=== X1 伪造路径字段拿不到他人资源（验收2） ===")
for t, label, pkt, rt in [
    (T_FLUSH, "列他人目录", pdu(T_FLUSH, msg=(f"./filesys/{B}").encode()), T_FLUSH_R),
    (T_DEL, "删他人文件", pdu(T_DEL, msg=secret.encode(), dat=struct.pack("<I", 1)), T_DEL_R),
    (T_DOWN, "下载他人文件", pdu(T_DOWN, msg=secret.encode(), dat=f32("secret.txt")), T_DOWN_R),
    (T_UPINIT, "传到他人目录", pdu(T_UPINIT, msg=(f"./filesys/{B}").encode(), dat=f32("evil.bin") + struct.pack("<q", 4)), T_UPINIT_R),
    (T_CHGDIR, "切入他人目录", pdu(T_CHGDIR, msg=(f"./filesys/{B}").encode()), T_CHGDIR_R),
]:
    r = ca.ask(pkt)
    refused = r and r[0][0] == rt and (len(r[0][2]) == 0 or not bf(r[0][1]))
    check(f"X1-{label}", f"{A} 会话中 {label} 被拒", refused, f"ret={bf(r[0][1]) if r else 'NORESP'} len={len(r[0][2]) if r else '-'}")
check("X1-victim", "他人文件完好", open(secret).read() == "VICTIM-CONTENT-DO-NOT-TOUCH", "内容一致")
r = ca.ask(pdu(T_QFL, msg=f32("secret.txt"), dat=f32(B)))
check("X1-跨查链接", "以他人 owner 字段查链接无效（按 actor 查）", r and r[0][0] == T_QFL_R and not bf(r[0][1]), f"exists={bf(r[0][1]) if r else 'NORESP'}")

print("=== X2 伪造发送者字段无效（验收2） ===")
cb, ret = login_conn(B)
check("X2a", f"{B} 登录成功", ret == 1, f"ret={ret}")
xmsg = b"helloX2\0"
xtm = time.strftime("%Y-%m-%d %H:%M:%S").encode()[:29].ljust(30, b"\0")
xbody = struct.pack("<II", 38 + len(xmsg), len(xmsg)) + xtm + xmsg
ca.ask(pdu(T_CHAT, msg=xbody, dat=f32(V) + f32(B)), n=0, t=0.8)
fwd = cb.idle(1.5)
got = [p for p in fwd if p[0] == T_CHAT]
sender_ok = got and got[0][1][:31].split(b"\0")[0].decode() == A
check("X2b", "B 收到的转发报文发送人=服务端actor(非伪造名)", sender_ok, f"sender={got[0][1][:31].split(chr(0).encode())[0] if got else 'NONE'}")

print("=== X3 伪造注销用户名删不掉他人账号（验收2/危险面） ===")
r = ca.ask(pdu(T_LOGOFF, dat=creds(V, "wrongpwd")))
check("X3a", "A 会话发注销V：按actor=A校验且密码错→被拒", r and r[0][0] == T_LOGOFF_R and not bf(r[0][1]), f"ret={bf(r[0][1]) if r else 'NORESP'}")
cv, ret = login_conn(V)
check("X3b", "V 账号仍存活可登录", ret == 1, f"ret={ret}")
cv.close()
check("X3c", "V 文件根目录仍在", os.path.isdir(os.path.join(SBX, V)), "存在")

print("=== R1 注销后旧连接业务被拒（验收3） ===")
cr, ret = login_conn(R)
check("R1a", f"{R} 登录成功", ret == 1, f"ret={ret}")
r = cr.ask(pdu(T_MKDIR, msg=(f"./filesys/{R}").encode(), dat=f32("rdir")))
check("R1b", "注销前本人 MKDIR 成功", r and bf(r[0][1]), f"ret={bf(r[0][1]) if r else 'NORESP'}")
r = cr.ask(pdu(T_LOGOFF, dat=creds(R)))
check("R1c", "本人正确密码注销成功", r and r[0][0] == T_LOGOFF_R and bf(r[0][1]), f"ret={bf(r[0][1]) if r else 'NORESP'}")
check("R1d", "注销后本人目录已删（既有行为保留）", not os.path.isdir(os.path.join(SBX, R)), "不存在")
r = cr.ask(pdu(T_FLUSH, msg=(f"./filesys/{A}").encode()))
check("R1e", "注销后同连接业务请求被拒（空响应=失败响应）", r and r[0][0] == T_FLUSH_R and len(r[0][2]) == 0, f"len={len(r[0][2]) if r else 'NORESP'}")
r = cr.ask(pdu(T_DOWN, msg=secret.encode(), dat=f32("secret.txt")))
check("R1f", "注销后不能继续访问群文件（下载他人被拒）", r and r[0][0] == T_DOWN_R and not bf(r[0][1]), f"ret={bf(r[0][1]) if r else 'NORESP'}")
cr.close()

print("=== R2 断线后新连接无残留身份（验收3） ===")
cb.close()
c2, ret = login_conn(B)
check("R2a", f"{B} 重新登录成功（账号存活）", ret == 1, f"ret={ret}")
cv2, retv = login_conn(V)
def online_names():
    r = c2.ask(pdu(T_ONLINE))
    blob = r[0][2] if r else b""
    return [blob[i * 32:i * 32 + 32].split(b"\0")[0].decode() for i in range(len(blob) // 32)]
n1 = online_names()
check("R2b1", "V 在线且 B 不在自己的在线列表", retv == 1 and V in n1 and B not in n1, f"online={n1}")
cv2.close()
time.sleep(0.4)
n2 = online_names()
check("R2b2", "V 断线后从在线列表移除（撤销生效）", V not in n2, f"online={n2}")
cn = fresh_conn()
r = cn.ask(pdu(T_FLUSH, msg=(f"./filesys/{B}").encode()))
check("R2c", "新连接未登录读他人根被拒", r and len(r[0][2]) == 0, f"len={len(r[0][2]) if r else 'NORESP'}")
cn.close()

print("=== K1 上传/下载/分享回归（验收4） ===")
payload = bytes(range(256)) * 4  # 1KB
updir = f"./filesys/{A}/workdir"
r = ca.ask(pdu(T_UPINIT, msg=updir.encode(), dat=f32("up.bin") + struct.pack("<q", len(payload)) + struct.pack("<I", MAGIC)))
check("K1a", "本人根内上传初始化成功", r and bf(r[0][1]), f"ret={bf(r[0][1]) if r else 'NORESP'}")
ok_acks = True
for i in range(0, len(payload), 600):
    chunk = payload[i:i + 600]
    pkt = pdu(T_UPDATA, msg=chunk, dat=struct.pack("<q", i) + b"\0" * 32 + struct.pack("<I", MAGIC))
    a_ = ca.ask(pkt)
    if not a_ or a_[0][0] != T_UPDATA_R or not bf(a_[0][1]):
        ok_acks = False
check("K1b", "上传分块全部 ack 成功", ok_acks, f"chunks={ (len(payload)+599)//600 }")
# D5：上传以内容寻址对象落盘，不再物理写用户树——条目即"落盘"（适配=owner 批准 D5，task.md 记录）
upsha = hashlib.sha256(payload).hexdigest()
check("K1c", "条目落盘+对象摘要一致+用户树无物理写(D5适配)",
      sql(f"SELECT COUNT(*) FROM user_file uf JOIN file_object fo ON fo.object_id=uf.object_id "
          f"WHERE uf.owner_name='{A}' AND uf.display_name='up.bin' AND uf.status='active' "
          f"AND fo.sha256='{upsha}' AND fo.size_bytes={len(payload)}") == "1"
      and not os.path.isfile(os.path.join(SBX, A, "workdir", "up.bin")),
      f"sha={upsha[:8]}")
r = ca.ask(pdu(T_DOWN, msg=(updir + "/up.bin").encode(), dat=f32("up.bin")))
meta = r[0] if r else None
dl_ok = meta and meta[0] == T_DOWN_R and bf(meta[1])
data = b"".join(p[2] for p in (r[1:] if r else []) if p[0] == T_DDOWN)
if dl_ok:
    size = struct.unpack("<q", meta[1][1:9])[0]
    deadline = time.time() + 8
    while len(data) < size and time.time() < deadline:
        for p in ca.idle(1.0):
            if p[0] == T_DDOWN:
                data += p[2]
check("K1d", "下载流式回读内容一致", dl_ok and len(data) == len(payload) and data == payload, f"bytes={len(data)}/{len(payload) if payload else 0}")

ca.ask(pdu(T_SHARE, msg=f32(B) + (f"./filesys/{A}/workdir/up.bin").encode(), dat=f32(V) + struct.pack("<i", 1)), n=0, t=0.6)
time.sleep(0.2)
fwd = c2.idle(1.5)
ag = [p for p in fwd if p[0] == T_AGREE]
sh_ok = ag and ag[0][1][:31].split(b"\0")[0].decode() == A
check("K1e", "B 收到分享请求且署名=actor(伪造V无效)", sh_ok, f"sharer={ag[0][1][:31].split(chr(0).encode())[0] if ag else 'NONE'}")
if sh_ok:
    r = c2.ask(pdu(T_AGREE_R, msg=ag[0][2], dat=f32(A)))  # B 故意谎报投递目标为 A
    fin = [p for p in ([r[0]] if r else []) if p[0] == T_FINISH]
    entry_b = sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{B}' AND display_name='up.bin' AND status='active'")
    # F-3/D5 修复：A 本人根的逻辑键为 '/'（owner 相对，跨树确定）
    entry_a = sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{A}' AND parent_path='/' AND display_name='up.bin' AND status='active'")
    check("K1f", "接收者谎报目标名：条目仍只落 B 本人根(D5适配)",
          fin and bf(fin[0][1]) and entry_b == "1" and entry_a == "0",
          f"ret={bf(fin[0][1]) if fin else 'NORESP'} inB={entry_b} inA={entry_a}")

r = ca.ask(pdu(T_MKDIR, msg=(f"./filesys/{A}").encode(), dat=f32("evil\\..\\out")))
check("K1g", "穿越名段仍被拒（FS-003 回归）", r and not bf(r[0][1]), f"ret={bf(r[0][1]) if r else 'NORESP'}")
r = ca.ask(pdu(T_CHGDIR, msg=b"C:/Windows"))
check("K1h", "沙箱外探测按不存在（回归）", r and not bf(r[0][1]), f"isExist={bf(r[0][1]) if r else 'NORESP'}")
r = ca.ask(pdu(T_REN, msg=struct.pack("<I", 1) + updir.encode(), dat=f32("up.bin") + f32("up2.bin")))
check("K1i", "本人重命名成功", r and bf(r[0][1]), f"ret={bf(r[0][1]) if r else 'NORESP'}")
r = ca.ask(pdu(T_REN, msg=struct.pack("<I", 1) + (f"./filesys/{B}").encode(), dat=f32("secret.txt") + f32("hijack.txt")))
check("K1j", "重命名他人文件被拒", r and not bf(r[0][1]) and os.path.isfile(secret), f"ret={bf(r[0][1]) if r else 'NORESP'}")

print("=== K2 M-1 分享握手登记：伪造/重放同意 ===")
forged = (f"./filesys/{A}/workdir/up2.bin").encode()
r = c2.ask(pdu(T_AGREE_R, msg=forged, dat=f32(B)))
landed_f = sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{B}' AND display_name='up2.bin' AND status='active'") != "0"
check("K2a", "未分享给 B 的源伪造同意被拒且无落地", r and r[0][0] == T_FINISH and not bf(r[0][1]) and not landed_f, f"ret={bf(r[0][1]) if r else 'NORESP'} landed={landed_f}")
ca.ask(pdu(T_SHARE, msg=f32(B) + forged, dat=f32(A) + struct.pack("<i", 1)))
fwd = c2.idle(1.5)
ag = [p for p in fwd if p[0] == T_AGREE]
r = c2.ask(pdu(T_AGREE_R, msg=ag[0][2], dat=f32(B))) if ag else []
landed = sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{B}' AND display_name='up2.bin' AND status='active'") == "1"
check("K2b", "握手登记后正常同意成功落地(D5适配为条目落地)", bool(ag) and r and bf(r[0][1]) and landed, f"agree={bool(ag)} ret={bf(r[0][1]) if r else 'NORESP'}")
r = c2.ask(pdu(T_AGREE_R, msg=ag[0][2], dat=f32(B)))
check("K2c", "登记一次性消费：重放同意被拒", r and r[0][0] == T_FINISH and not bf(r[0][1]), f"ret={bf(r[0][1]) if r else 'NORESP'}")
# B-1：A 把 B 的文件"分享给自己"→ 源不属于分享人：不登记不转发；随后 A 自我同意必须失败
bsecret = (f"./filesys/{B}/secret.txt").encode()
ca.ask(pdu(T_SHARE, msg=f32(A) + bsecret, dat=f32(A) + struct.pack("<i", 1)))
r = ca.ask(pdu(T_AGREE_R, msg=bsecret, dat=f32(A)))
stolen = os.path.exists(os.path.join(SBX, A, "secret.txt"))
check("K2d", "B-1：自我登记他人源→自我同意链被拒且无复制", r and r[0][0] == T_FINISH and not bf(r[0][1]) and not stolen, f"ret={bf(r[0][1]) if r else 'NORESP'} stolen={stolen}")

ca.close(); c2.close()
stop_server()
fails = [x for x in results if not x[2]]
print(f"\n==== 汇总: {len(results)-len(fails)}/{len(results)} PASS ====")
for f in fails:
    print("FAIL:", f[0], f[1])
sys.exit(1 if fails else 0)
