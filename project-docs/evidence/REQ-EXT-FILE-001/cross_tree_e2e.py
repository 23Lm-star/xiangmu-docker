# REQ-EXT-FILE-001 F-3/D5 修复回归：跨树逻辑键（两棵不同绝对根）。
# 树A = <repo>/dist（默认 cwd），树B = <repo>/build/xtree-file001/（独立 cwd），
# 树B 的 ./filesys 用 NTFS junction 指向同一物理 filesys（mklink /J，无需管理员），共享同一 MySQL。
# 断言：同一逻辑请求在两树得到同一 parent_path 键（幂等命中不重复建条目/不加引用）、
# 分享接收者条目落 '/' 根键、存量"绝对机器路径"行启动时确定性转换且重放幂等。
#
# ---- 统一 e2e 运行形态（五套件一致，见 project-docs/evidence/E2E-RUNFORM.md）----
# 运行：py -3.13 project-docs/evidence/REQ-EXT-FILE-001/cross_tree_e2e.py
#   cwd     : 任意（脚本自管理；树A 以 <repo>/dist 为 cwd，树B 以 <repo>/build/xtree-file001 为 cwd）
#   exe     : <repo>/dist/Server.exe（qmake DESTDIR=dist；connect.config 构建期经 qrc 内嵌）
#   config  : Server/connect.config -> filesPath=./filesys（相对 cwd；两树因此指向各自的 ./filesys）
#   数据根  : <repo>/dist/filesys（树A）；树B 经 junction 复用同一物理目录
#   未入库依赖：dist/{Qt5Core,Qt5Gui,Qt5Network,Qt5Sql,Qt5Svg,Qt5Widgets}.dll、dist/libmysql.dll、
#              dist/sqldrivers/qsqlmysql.dll、dist/platforms/qwindows.dll
import hashlib, os, shutil, socket, struct, subprocess, sys, time

HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_REG, T_CAP, T_LOGIN = 1, 44, 3
T_MKDIR, T_FLUSH, T_UPINIT, T_UPDATA = 19, 23, 29, 31
T_UPINIT_R, T_UPDATA_R, T_FLUSH_R = 30, 32, 24
T_DOWN, T_DOWN_R, T_DDOWN = 40, 41, 42
T_SHARE, T_AGREE_R, T_SHARE_FINISH = 33, 36, 37
MAGIC = 0x5253554d
MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
DIST = os.path.join(REPO, "dist")
EXE = os.path.join(DIST, "Server.exe")
sys.path.insert(0, os.path.join(REPO, "project-docs", "evidence", "_harness"))
import server_ctl
_SRV = server_ctl.ServerCtl(EXE, DIST, HOST, PORT)
SRV = DIST
FS = os.path.join(DIST, "filesys")
TREEB = os.path.join(REPO, "build", "xtree-file001")
X1, X2 = "xtuser1", "xtuser2"
PC = b"XTREE-CROSS-CONTENT-" * 40
PS = b"XTREE-SUB-CONTENT-" * 30
SH = lambda b: hashlib.sha256(b).hexdigest()

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
    def idle(s, t=0.6):
        s._pump(t); return s.packets()
    def close(s):
        try: s.sock.close()
        except OSError: pass

def sql(q):
    r = subprocess.run([MYSQL] + DB + [q], capture_output=True, text=True)
    return r.stdout.strip()

def start_server(cwd):
    # FINDING-GATE-1: 原来是 taskkill /F /IM Server.exe —— 按映像名杀，会打到并行套件的
    # 进程（实测造成三次"服务端突然消失"的伪缺陷）。现在只按自己启动的 PID 管理；端口被
    # 别人占用时直接报错，不盲杀。契约不变：仍返回 bool。
    global _SRV
    _SRV = server_ctl.ServerCtl(EXE, cwd, HOST, PORT)
    if not _SRV.start():
        print(f"[E] {_SRV.last_error}")
        return False
    return True

def stop_server():
    _SRV.stop()

def wait_sql(q, want, timeout=15):
    dl = time.time() + timeout
    while time.time() < dl:
        if sql(q) == want: return True
        time.sleep(0.5)
    return False

results = []
def check(sid, desc, cond, detail=""):
    results.append((sid, desc, bool(cond)))
    print(f"[{'PASS' if cond else 'FAIL'}] {sid} {desc} :: {detail}")

def login_conn(name):
    c = Conn()
    r = c.ask(pdu(T_CAP)); code = r[0][2].split(b"\0")[0]
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
        a = c.ask(pdu(T_UPDATA, msg=ch,
                      dat=struct.pack("<q", i) + b"\0" * 32 + struct.pack("<I", MAGIC)))
        if not a or a[0][0] != T_UPDATA_R or a[0][1][0] == 0: return False
    return True

def flush_names(c, dirPath):
    r = c.ask(pdu(T_FLUSH, msg=dirPath.encode()))
    names = []
    if r and r[0][0] == T_FLUSH_R:
        m = r[0][2]
        for i in range(len(m) // 36):
            names.append(m[i * 36:i * 36 + 32].split(b"\0")[0].decode("utf-8", "replace"))
    return names

def download_bytes(c, path, name, timeout=8):
    got = c.ask(pdu(T_DOWN, msg=path.encode(), dat=f32(name)), n=1, t=4.0)
    meta = None; data = b""
    for p in got:
        if p[0] == T_DOWN_R: meta = p
        elif p[0] == T_DDOWN: data += p[2]
    dl = time.time() + timeout
    while time.time() < dl:
        for p in c.idle(0.5):
            if p[0] == T_DOWN_R and meta is None: meta = p
            elif p[0] == T_DDOWN: data += p[2]
        if meta and meta[1][0] != 0 and len(data) == struct.unpack("<q", meta[1][1:9])[0]:
            break
    return meta, data

# ---------- 0. 清场 ----------
stop_server()
subprocess.run(["cmd", "/c", "rmdir", os.path.join(TREEB, "filesys")], capture_output=True)
shutil.rmtree(TREEB, ignore_errors=True)
for u in (X1, X2):
    sql(f"DELETE FROM user_file WHERE owner_name='{u}';")
    sql(f"DELETE FROM upload_session WHERE owner_name='{u}';")
    sql(f"DELETE FROM share_link WHERE owner_name='{u}';")
sql(f"DELETE FROM file_migration_map WHERE old_path LIKE 'filesys/{X1}/%' OR old_path LIKE '%filesys/{X1}/%';")
sql(f"DELETE FROM file_object WHERE sha256 IN ('{SH(PC)}','{SH(PS)}');")
for u in (X1, X2):
    shutil.rmtree(os.path.join(FS, u), ignore_errors=True)

# ---------- 1. 树A（cwd=Server/）：上传 + MKDIR + 子目录上传 ----------
check("TA1", "Server 树A 启动", start_server(SRV))
for u in (X1, X2):
    os.makedirs(os.path.join(FS, u), exist_ok=True)
ca, ret = login_conn(X1)
check("TA2", f"{X1} 登录", ret == 1, f"ret={ret}")
check("TA3", "树A 上传 ct.bin 成功", upload(ca, f"./filesys/{X1}", "ct.bin", PC))
check("TA4", "条目键 parent_path='/'（根，'/' 结尾规范）",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{X1}' AND display_name='ct.bin' "
          f"AND parent_path='/' AND status='active'") == "1")
check("TA5", "对象 refcount=1",
      sql(f"SELECT refcount FROM file_object WHERE sha256='{SH(PC)}'") == "1")
ca.ask(pdu(T_MKDIR, msg=f"./filesys/{X1}".encode(), dat=f32("sub")))
check("TA6", "MKDIR sub 目录条目键 '/'",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{X1}' AND display_name='sub' "
          f"AND is_dir=1 AND parent_path='/' AND status='active'") == "1")
check("TA7", "树A 上传 sub/s.bin 成功", upload(ca, f"./filesys/{X1}/sub", "s.bin", PS))
check("TA8", "子目录条目键 parent_path='/sub/'",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{X1}' AND display_name='s.bin' "
          f"AND parent_path='/sub/' AND status='active'") == "1")
ca.close()

# ---------- 2. 树B（独立绝对 cwd + junction）：同请求幂等命中、列表/下载走共享对象 ----------
stop_server()
os.makedirs(TREEB, exist_ok=True)
jr = subprocess.run(["cmd", "/c", "mklink", "/J", os.path.join(TREEB, "filesys"), FS],
                    capture_output=True)   # cmd 输出 GBK，不解码；以目录存在性判定
check("TB0", "树B junction filesys 建立", jr.returncode == 0 and os.path.isdir(os.path.join(TREEB, "filesys")), f"rc={jr.returncode}")
check("TB1", "Server 树B 启动（cwd=树B）", start_server(TREEB))
cb1, ret = login_conn(X1)
check("TB2", f"{X1} 树B 登录", ret == 1, f"ret={ret}")
check("TB3", "树B 同内容同名重复上传（初始化+分块 ack 正常）",
      upload(cb1, f"./filesys/{X1}", "ct.bin", PC))
check("TB4", "跨树幂等：条目仍 1 行 parent_path='/'",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{X1}' AND display_name='ct.bin' "
          f"AND parent_path='/' AND status='active'") == "1")
check("TB5", "跨树幂等：refcount 仍 1（无重复行/无重复计数）",
      wait_sql(f"SELECT refcount FROM file_object WHERE sha256='{SH(PC)}'", "1", timeout=6))
check("TB6", "跨树幂等：'%'旧形态键零残留",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{X1}' AND parent_path NOT LIKE '/%'") == "0")
fl = flush_names(cb1, f"./filesys/{X1}")
check("TB7", "树B flushfile 命中树A 建立的条目（逻辑键权威）", "ct.bin" in fl and "sub" in fl, f"names={fl}")
meta, data = download_bytes(cb1, f"./filesys/{X1}/ct.bin", "ct.bin")
check("TB8", "树B 下载树A 对象字节一致（storage_path 相对解析）",
      meta and meta[1][0] != 0 and data == PC, f"bytes={len(data)}/{len(PC)}")
check("TB9", "树B 子目录条目仍 1 行 '/sub/'",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{X1}' AND display_name='s.bin' "
          f"AND parent_path='/sub/' AND status='active'") == "1")

# ---------- 3. 分享跨树落地（接收者根键 '/'） ----------
cb2, ret = login_conn(X2)
sharemsg = f32(X2) + f"./filesys/{X1}/ct.bin".encode()
cb1.ask(pdu(T_SHARE, msg=sharemsg, dat=f32(X1) + struct.pack("<i", 1)))
time.sleep(0.5)
fwd = cb2.idle(1.5)
ag = [p for p in fwd if p[0] == 35]
cb2.sock.sendall(pdu(T_AGREE_R, msg=(ag[0][2] if ag else f"./filesys/{X1}/ct.bin".encode()), dat=f32(X2)))
fin = None; dl = time.time() + 5
while time.time() < dl and fin is None:
    for p in cb2.idle(0.5):
        if p[0] == T_SHARE_FINISH: fin = p
check("XT1", "树B 同意分享→完成", fin and fin[1][0] != 0, f"pkt={fin[:2] if fin else None}")
check("XT2", "接收者条目键 '/' 且引用同一对象",
      sql(f"SELECT COUNT(*) FROM user_file uf JOIN file_object fo ON fo.object_id=uf.object_id "
          f"WHERE uf.owner_name='{X2}' AND uf.display_name='ct.bin' AND uf.parent_path='/' "
          f"AND uf.status='active' AND fo.sha256='{SH(PC)}'") == "1")
check("XT3", "refcount=2", wait_sql(f"SELECT refcount FROM file_object WHERE sha256='{SH(PC)}'", "2"))
meta2, data2 = download_bytes(cb2, f"./filesys/{X2}/ct.bin", "ct.bin")
check("XT4", "接收者树B 下载字节一致（物理零复制）",
      meta2 and meta2[1][0] != 0 and data2 == PC, f"bytes={len(data2)}/{len(PC)}")
cb1.close(); cb2.close()

# ---------- 4. 存量"绝对机器路径"行：启动确定性转换 + 幂等重放 ----------
stop_server()
LEG = "E:/legacy/roottree/filesys"
LEG2 = "E:/legacy/roottree-B/filesys"
sql(f"DELETE FROM user_file WHERE owner_name='{X1}' AND display_name IN ('oldfile.txt','dupf.txt','slashdir','fsdir');")
sql(f"DELETE FROM file_migration_map WHERE old_path LIKE '{LEG}/%' OR old_path LIKE '{LEG2}/%';")
# F-5 夹具：同 triple 撞键对（两他树绝对键、同 owner+名+对象、均 active）+ 无盘符 "/filesys/…" 探针
# T3 夹具：首段=filesys 且 '/' 结尾（旧 SQL 谓词误判 canonical，实际 normalizeParent 会转换）
ins = (f"INSERT INTO user_file (owner_name,parent_path,display_name,is_dir,object_id,status) "
       f"VALUES ('{X1}','/filesys/{X1}/sub/','fsdir',1,NULL,'active'); "
       f"INSERT INTO upload_session (owner_name,parent_path,display_name,expected_size,staging_path,status,expires_at) "
       f"VALUES ('{X1}','/filesys/{X1}/sub/','usd.txt',10,'x','failed',NOW()); "
       f"INSERT INTO user_file (owner_name,parent_path,display_name,is_dir,object_id,status) "
       f"SELECT '{X1}','{LEG}/{X1}','oldfile.txt',0,fo.object_id,'active' "
       f"FROM file_object fo WHERE fo.sha256='{SH(PC)}'; "
       f"INSERT INTO user_file (owner_name,parent_path,display_name,is_dir,object_id,status) "
       f"SELECT '{X1}','{LEG}/{X1}','dupf.txt',0,fo.object_id,'active' "
       f"FROM file_object fo WHERE fo.sha256='{SH(PC)}'; "
       f"INSERT INTO user_file (owner_name,parent_path,display_name,is_dir,object_id,status) "
       f"SELECT '{X1}','{LEG2}/{X1}','dupf.txt',0,fo.object_id,'active' "
       f"FROM file_object fo WHERE fo.sha256='{SH(PC)}'; "
       f"INSERT INTO user_file (owner_name,parent_path,display_name,is_dir,object_id,status) "
       f"VALUES ('{X1}','/filesys/{X1}','slashdir',1,NULL,'active'); "
       f"UPDATE file_object SET refcount=refcount+3 WHERE sha256='{SH(PC)}'; "
       f"INSERT INTO file_migration_map (old_path,file_id,object_id,status) "
       f"VALUES ('{LEG}/{X1}/oldfile.txt',NULL,NULL,'failed');")
subprocess.run([MYSQL] + DB + [ins], capture_output=True, text=True)
check("CV1", "Server 树B 重启（触发存量键转换）", start_server(TREEB))
time.sleep(1.5)
check("CV2", "user_file 旧绝对键 → '/' 逻辑键",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{X1}' AND display_name='oldfile.txt' "
          f"AND parent_path='/' AND status='active'") == "1")
check("CV3", "file_migration_map 绝对 old_path → 'filesys/...' 相对键",
      wait_sql(f"SELECT COUNT(*) FROM file_migration_map WHERE old_path='filesys/{X1}/oldfile.txt'", "1"))
# F-5：撞键收敛=loser 软删且不改写 parent（改写=同三重撞 uk_loc→1062）；存活者最早旧行
check("CVXA", "dupf 撞键：active 仅 1 行（最早旧行存活）",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{X1}' AND display_name='dupf.txt' "
          f"AND status='active' AND parent_path='/'") == "1")
check("CVXB", "dupf 撞键：loser 已软删（total 2，deleted 1）",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{X1}' AND display_name='dupf.txt' "
          f"AND status='deleted'") == "1")
check("CVXC", "dupf loser 保留旧键未被改写（无 1062 路径）",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{X1}' AND display_name='dupf.txt' "
          f"AND status='deleted' AND parent_path='{LEG2}/{X1}'") == "1")
check("CVXD", "无盘符 '/filesys/<user>' 探针 → '/'",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{X1}' AND display_name='slashdir' "
          f"AND parent_path='/' AND is_dir=1") == "1")
# T3/FINDING-DEV-3：判别与 isLogicKeyForm 对齐——旧形状谓词误判为 canonical 的形态必须转换
check("CVXG", "首段=filesys 且 '/' 结尾（旧谓词误判 canonical）→ '/sub/'",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{X1}' AND display_name='fsdir' "
          f"AND parent_path='/sub/' AND is_dir=1") == "1")
check("CVXH", "user_file 零 '/filesys/...' 残留",
      sql("SELECT COUNT(*) FROM user_file WHERE parent_path LIKE '/filesys/%'") == "0")
check("CVXJ", "upload_session 同判别：'/filesys/...' → '/sub/'",
      sql(f"SELECT COUNT(*) FROM upload_session WHERE owner_name='{X1}' AND display_name='usd.txt' "
          f"AND parent_path='/sub/'") == "1")
check("CV4", "active 行零残留非逻辑键（deleted 保留旧键属预期）",
      sql("SELECT COUNT(*) FROM user_file WHERE status='active' AND parent_path NOT LIKE '/%'") == "0")
cc, ret = login_conn(X1)
fl2 = flush_names(cc, f"./filesys/{X1}")
check("CV5", "转换后条目在树B 列表可见（跨树命中）",
      "oldfile.txt" in fl2 and "dupf.txt" in fl2 and "slashdir" in fl2, f"names={fl2}")
cc.close()
check("CVXE", "收敛镜像减引用（5 注入引用→loser -1=4）",
      sql(f"SELECT refcount FROM file_object WHERE sha256='{SH(PC)}'") == "4")
stop_server()
check("CV6", "Server#重放启动", start_server(TREEB))
time.sleep(1.5)
check("CV7", "重放幂等：oldfile 行数不变",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{X1}' AND display_name='oldfile.txt'") == "1")
check("CV8", "重放幂等：map 相对键行不变",
      sql(f"SELECT COUNT(*) FROM file_migration_map WHERE old_path='filesys/{X1}/oldfile.txt'") == "1")
check("CV9", "重放幂等：refcount 仍 4（收敛只发生一次，deleted 行静默跳过）",
      sql(f"SELECT refcount FROM file_object WHERE sha256='{SH(PC)}'") == "4")
check("CVXF", "重放幂等：dupf 1 active + 1 deleted 不变",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{X1}' AND display_name='dupf.txt' "
          f"AND status='active'") == "1"
      and sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{X1}' AND display_name='dupf.txt' "
              f"AND status='deleted'") == "1")
check("CVXK", "重放幂等：fsdir 1 行 '/sub/' 不变、'/filesys/...' 仍零残留",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{X1}' AND display_name='fsdir' "
          f"AND parent_path='/sub/'") == "1"
      and sql("SELECT COUNT(*) FROM user_file WHERE parent_path LIKE '/filesys/%'") == "0")

# ---------- 5. 收尾清场（不污染后续套件） ----------
stop_server()
subprocess.run(["cmd", "/c", "rmdir", os.path.join(TREEB, "filesys")], capture_output=True)
shutil.rmtree(TREEB, ignore_errors=True)
for u in (X1, X2):
    sql(f"DELETE FROM user_file WHERE owner_name='{u}';")
    sql(f"DELETE FROM upload_session WHERE owner_name='{u}';")
    sql(f"DELETE FROM share_link WHERE owner_name='{u}';")
sql(f"DELETE FROM file_migration_map WHERE old_path LIKE 'filesys/{X1}/%' OR old_path LIKE '{LEG}/%';")
sql("DELETE fo FROM file_object fo LEFT JOIN user_file uf ON uf.object_id=fo.object_id WHERE uf.file_id IS NULL;")
for u in (X1, X2):
    shutil.rmtree(os.path.join(FS, u), ignore_errors=True)

print("\n=== 汇总 cross_tree_e2e ===")
passed = sum(1 for _, _, ok in results if ok)
for sid, desc, ok in results:
    if not ok: print(f"  FAIL -> {sid} {desc}")
print(f"{passed}/{len(results)} passed")
sys.exit(0 if passed == len(results) else 1)
