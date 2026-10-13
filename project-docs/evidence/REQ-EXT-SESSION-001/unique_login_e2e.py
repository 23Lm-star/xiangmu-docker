# REQ-EXT-SESSION-001 唯一登录 e2e harness（开发段自证，E1-E7）
# 自管理 Server.exe 生命周期；DB 断言走 mysql CLI（仿 FILE-001 entry_ops_e2e.py 模式，无新依赖）。
# 运行：仓库根目录  py -3.13 project-docs/evidence/REQ-EXT-SESSION-001/unique_login_e2e.py
import atexit, hashlib, os, socket, struct, subprocess, sys, threading, time

HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_REG, T_LOGIN, T_LOGIN_R = 1, 3, 4
T_FINDU, T_FINDU_R = 5, 6
T_CAP, T_CAP_R, T_HB, T_HB_R = 44, 45, 50, 51
MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]

# FINDING-GATE-2：原来是 build/build-Server/release/Server.exe + cwd=Server/（源码树形态）。
# 那个路径是 qmake shadow build 的**中间目录**（被 /build/ gitignore，Server.pro 的 DESTDIR
# 只指向 ../dist），在主工作区根本不存在—— 当年能跑是因为整个测试节点在 detached worktree
# 内自行构建。改用与另外7 套一致的部署形态：exe 与 cwd 同为 dist/。
# connect.config 由 qrc 在构建期内嵌进 exe（所以 dist/ 下没有这个文件），因此换cwd 不会
# 改变 Server 读到的配置；这与 FILE-001 / TRANSFER-001 / GROUP-003 三套的既有做法相同。
DIST = os.path.abspath("dist")
EXE = os.path.join(DIST, "Server.exe")
SRV = DIST
sys.path.insert(0, os.path.join(os.path.dirname(DIST), "project-docs", "evidence", "_harness"))
import server_ctl
_SRV = server_ctl.ServerCtl(EXE, SRV, HOST, PORT)
U1, U2, U3 = "unilog1", "unilog2", "unilog3"
IDA = b"A1A1" * 8
IDB = b"B2B2" * 8

def f32(s): return s.encode("utf-8")[:31].ljust(32, b"\0")
def creds(name, pwd="123456"):
    return f32(name) + hashlib.md5(pwd.encode()).hexdigest().encode()
def pdu(t, msg=b"", dat=b""):
    dat = dat[:64].ljust(64, b"\0")
    return struct.pack("<III", HDR + len(msg), len(msg), t) + dat + msg

class Conn:
    def __init__(s):
        s.sock = socket.create_connection((HOST, PORT), timeout=10); s.buf = b""; s.eof = False
    def _pump(s, t):
        s.sock.settimeout(t)
        try:
            d = s.sock.recv(65536)
            if d: s.buf += d
            else: s.eof = True
        except socket.timeout: pass
        except (ConnectionResetError, ConnectionAbortedError): s.eof = True
        except OSError: s.eof = True
    def packets(s):
        out = []
        while len(s.buf) >= HDR:
            tl, ml, ty = struct.unpack("<III", s.buf[:12])
            if tl < HDR or tl != HDR + ml or len(s.buf) < tl: break
            out.append((ty, s.buf[12:76], s.buf[76:tl])); s.buf = s.buf[tl:]
        return out
    def ask(s, pkt, n=1, t=4.0):
        try: s.sock.sendall(pkt)
        except OSError: s.eof = True
        got = []; dl = time.time() + t
        while len(got) < n and time.time() < dl and not s.eof:
            s._pump(0.4); got = s.packets()
        return got
    def close(s):
        try: s.sock.close()
        except OSError: pass

def sql(q):
    r = subprocess.run([MYSQL] + DB + [q], capture_output=True, text=True)
    if r.returncode != 0:
        print("FATAL SQL:", q, r.stderr); sys.exit(2)
    return r.stdout.strip()

def start_server():
    # FINDING-GATE-1: 原来是 taskkill /F /IM Server.exe —— 按映像名杀，会打到并行套件的
    # 进程（实测造成三次"服务端突然消失"的伪缺陷）。现在只按自己启动的 PID 管理；端口被
    # 别人占用时直接报错，不盲杀。契约不变：仍返回 bool。
    global _SRV
    _SRV = server_ctl.ServerCtl(EXE, SRV, HOST, PORT)
    if not _SRV.start():
        print(f"[E] {_SRV.last_error}")
        return False
    return True

def stop_server():
    _SRV.stop()

atexit.register(stop_server)

results = []
def check(sid, desc, cond, detail):
    results.append(bool(cond))
    print(f"[{'PASS' if cond else 'FAIL'}] {sid} {desc} :: {detail}")

def login_conn(name, cid=None):
    c = Conn()
    r = c.ask(pdu(T_CAP))
    code = r[0][2].split(b"\0")[0]
    msg = code + b"\0" + (cid if cid else b"")
    r = c.ask(pdu(T_LOGIN, msg=msg, dat=creds(name)))
    ret = struct.unpack("<i", r[0][1][:4])[0] if r else -99
    return c, ret

def ensure_user(name):
    c = Conn()
    r = c.ask(pdu(T_CAP))
    code = r[0][2].split(b"\0")[0]
    c.ask(pdu(T_REG, msg=code + b"\0", dat=creds(name)))
    c.close()

# ---------- 前置：DB 就绪（迁移幂等应用）+ 清理测试用户 ----------
if not sql("SHOW COLUMNS FROM user_info LIKE 'login_version'"):
    sql("ALTER TABLE user_info ADD COLUMN login_version BIGINT NOT NULL DEFAULT 0")
    print("migrated: user_info.login_version")
sql("CREATE TABLE IF NOT EXISTS login_record (id INT AUTO_INCREMENT PRIMARY KEY,"
    " username VARCHAR(32) NOT NULL, client_id VARCHAR(64) NULL DEFAULT NULL,"
    " login_version BIGINT NOT NULL, login_time DATETIME NOT NULL,"
    " KEY idx_user_ver (username, login_version)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4")
# QMYSQL 空 QString 参数按 NULL 发送（实测驱动行为）：空 ID=无标识，列必须可空
sql("ALTER TABLE login_record MODIFY client_id VARCHAR(64) NULL DEFAULT NULL")
for u in (U1, U2, U3):
    sql(f"DELETE FROM login_record WHERE username='{u}'")
    sql(f"DELETE FROM user_info WHERE name='{u}'")
assert start_server(), "server did not start"
for u in (U1, U2, U3):
    ensure_user(u)
print("server up, db ready")

# ---------- E2 未认证白名单不受版本校验 ----------
c = Conn()
r = c.ask(pdu(T_CAP))
check("E2a", "未认证验证码正常", r and r[0][0] == T_CAP_R, f"type={r[0][0] if r else 'none'}")
r = c.ask(pdu(T_HB))
check("E2b", "未认证心跳 PONG 正常", r and r[0][0] == T_HB_R, f"type={r[0][0] if r else 'none'}")
c.close()

# ---------- E1 双客户端互踢 + 先断后处理 + 登录档案 ----------
cA, retA = login_conn(U1, IDA)
check("E0", f"{U1} 首登录成功", retA == 1, f"ret={retA}")
cB, retB = login_conn(U1, IDB)
check("E1a", "第二设备登录成功", retB == 1, f"ret={retB}")
time.sleep(0.5)
r = cA.ask(pdu(T_FINDU, dat=f32("nobody_xxx")))
closed = cA.eof and not any(p[0] == T_FINDU_R for p in r)
check("E1b", "旧连接业务请求被切断且未处理（无 FINDU 响应+EOF）", closed, f"eof={cA.eof} pkts={len(r)}")
rows = sql(f"SELECT login_version, client_id FROM login_record WHERE username='{U1}' ORDER BY login_version")
lv = sql(f"SELECT login_version FROM user_info WHERE name='{U1}'")
check("E1c", "login_record 恰两行、版本 1/2 连续", rows.splitlines() == ["1\tA1A1A1A1A1A1A1A1A1A1A1A1A1A1A1A1",
      "2\tB2B2B2B2B2B2B2B2B2B2B2B2B2B2B2B2"], repr(rows))
check("E1d", "user_info.login_version=2", lv == "2", f"lv={lv}")

# ---------- E4 竞态：被踢旧连接断开不清在线态 ----------
online = sql(f"SELECT online FROM user_info WHERE name='{U1}'")
check("E4", "旧连接断开后在线态仍为 1（守卫生效）", online == "1", f"online={online}")
cB.close(); cA.close(); time.sleep(0.8)

# ---------- E6 已认证过期连接心跳也被踢（D4） ----------
cX, retX = login_conn(U1, IDA)
cY, retY = login_conn(U1, IDB)
time.sleep(0.3)
r = cX.ask(pdu(T_HB), t=3.0)
kicked = cX.eof and not any(p[0] == T_HB_R for p in r)
check("E6", "过期已认证连接心跳被切断、无 PONG", kicked, f"eof={cX.eof} pkts={len(r)}")
cY.close(); cX.close()

# ---------- E5 空 ID 登录成功、档案空值（NULL=无标识，需求"按空值存储"的列形态） ----------
cZ, retZ = login_conn(U2, None)
cid = sql(f"SELECT IFNULL(client_id,'') FROM login_record WHERE username='{U2}' ORDER BY id DESC LIMIT 1")
nrow = sql(f"SELECT COUNT(*) FROM login_record WHERE username='{U2}'")
check("E5", "空 ID 登录成功且 login_record.client_id 为空值", retZ == 1 and cid == "" and nrow == "1", f"ret={retZ} cid={cid!r} rows={nrow}")
cZ.close()

# ---------- E3 并发双登录：版本原子、恰两行、高版本存活 ----------
gate = threading.Barrier(2)
socks = {}
def concurrent_login(tag, cid):
    g = gate; g.wait()
    c, ret = login_conn(U3, cid)
    socks[tag] = (c, ret)
t1 = threading.Thread(target=concurrent_login, args=("T1", IDA)); t2 = threading.Thread(target=concurrent_login, args=("T2", IDB))
t1.start(); t2.start(); t1.join(); t2.join()
rows = sql(f"SELECT login_version, client_id FROM login_record WHERE username='{U3}' ORDER BY login_version")
vs = [int(l.split("\t")[0]) for l in rows.splitlines()]
check("E3a", "并发登录恰两行、版本不重复不倒退", len(vs) == 2 and vs[1] == vs[0] + 1, repr(vs))
lv3 = sql(f"SELECT login_version FROM user_info WHERE name='{U3}'")
check("E3b", "user_info.login_version 等于最大版本", lv3 == str(max(vs)), f"lv={lv3} rows={vs}")
# 胜者=持有最大版本 client_id 的连接
win = "T2" if IDB.decode() in [l.split("\t")[1] for l in rows.splitlines() if int(l.split("\t")[0]) == max(vs)][0] else "T1"
lose = "T1" if win == "T2" else "T2"
cw, _ = socks[win]; cl, _ = socks[lose]
time.sleep(0.6)
rw = cw.ask(pdu(T_FINDU, dat=f32("nobody_xxx")), t=3.0)
rl = cl.ask(pdu(T_FINDU, dat=f32("nobody_xxx")), t=3.0)
check("E3c", "最高版本连接存活（有响应）", (not cw.eof) and len(rw) >= 1, f"eof={cw.eof} pkts={len(rw)}")
check("E3d", "较低版本连接已失效（EOF 或后续被切断）", cl.eof or rl == [] or any(p[0] not in (T_FINDU_R,) for p in rl), f"eof={cl.eof} pkts={len(rl)}")
cw.close(); cl.close()

# ---------- E7 回归：SEC-001 全量 verify.py + protocol.h 零命中 ----------
# SEC-001 独立作为 test_gate 套件时会自启动；这里复用本套件已启动的同一 PID。
env = dict(os.environ)
env["SEC001_BASE"] = os.path.abspath(".")
env["SEC001_SBX"] = os.path.join(env["SEC001_BASE"], "dist", "filesys")
env["SEC001_REUSE_SERVER"] = "1"
r = subprocess.run([sys.executable, os.path.join("project-docs", "evidence", "REQ-EXT-SEC-001", "verify.py")],
                   capture_output=True, text=True, timeout=600, env=env)
fails = [l for l in (r.stdout or "").splitlines() if l.startswith("[FAIL]")]
npass = sum(1 for l in (r.stdout or "").splitlines() if l.startswith("[PASS]"))
check("E7a", "SEC-001 全量回归无 FAIL", r.returncode == 0 and not fails, f"pass={npass} fails={fails[:3]}")
gd = subprocess.run(["git", "diff", "master...HEAD", "--", "Server/protocol.h", "Client/protocol.h",
                     "project-docs/init_test_data.sql"], capture_output=True, text=True)
_diff = gd.stdout or ""
_add = [l[1:] for l in _diff.splitlines() if l.startswith("+") and not l.startswith("+++")]
_rem = [l[1:].strip() for l in _diff.splitlines()
        if l.startswith("-") and not l.startswith("---") and l[1:].strip()]
# E7b 的语义是"本分支没在冻结面上做未经批准的手脚"，不是"本分支永不碰冻结面"。
# 后续需求（如 REQ-EXT-SHARE-001）获批准的常量块追加必然命中 protocol.h；按"零命中"判，
# 任何一次合法冻结面变更都会把它打成红，护栏就等于"冻结面永不可变"。收紧成真正要守的两条：
#   ① 冻结面只许追加，不许删除/改写；② 追加行必须整体落在带 `// ===== REQ-EXT-*` 头的常量块内
#      （散落的、无标记的常量改动一律算违规）。
_segments, _cur = [], []
for _l in _add:
    if _l.strip() == "":
        continue
    if _l.startswith("// ===== REQ-EXT-") and _cur:
        _segments.append(_cur)
        _cur = []
    _cur.append(_l)
if _cur:
    _segments.append(_cur)
_unmarked = [s[0][:60] for s in _segments if not s[0].startswith("// ===== REQ-EXT-")]
proto_ok = (not _rem) and (not _unmarked)
check("E7b", "分支 diff 未删改冻结面，且两 protocol.h 的追加行全在已批准的 REQ-EXT-* 常量块内",
      proto_ok, "rem=%s unmarked=%s" % (_rem, _unmarked))

stop_server()
nfail = results.count(False)
print(f"\nSUMMARY: {len(results)-nfail}/{len(results)} PASS, {nfail} FAIL")
sys.exit(0 if nfail == 0 else 1)
