# REQ-EXT-FILE-001 验收4/5 + 条目操作 e2e（证据归档，仿 SEC-001 verify.py 模式）。
# 覆盖：启动存量迁移(T6)/幂等重跑/同名不同内容(1)安置/skipped_name_conflict/旧文件零删除(验收5)、
# MKDIR目录条目、rename/move/sharefile 条目优先+物理回退(D5)、伪造大小拒绝+跨用户拒绝(验收4)。
# 脚本自管理 Server.exe 生命周期（taskkill+Popen, cwd=<repo>/dist）；断言走 DB/磁盘/响应字节。
#
# ---- 统一 e2e 运行形态（五套件一致，见 project-docs/evidence/E2E-RUNFORM.md）----
# 运行：py -3.13 project-docs/evidence/REQ-EXT-FILE-001/entry_ops_e2e.py
#   cwd     : 任意（脚本自管理，Server 以 <repo>/dist 为 cwd 启动）
#   exe     : <repo>/dist/Server.exe（qmake DESTDIR=dist；connect.config 构建期经 qrc 内嵌）
#   config  : Server/connect.config -> filesPath=./filesys（相对 cwd）；未入库的机器本地文件
#   数据根  : <repo>/dist/filesys
#   未入库依赖：dist/{Qt5Core,Qt5Gui,Qt5Network,Qt5Sql,Qt5Svg,Qt5Widgets}.dll、dist/libmysql.dll、
#              dist/sqldrivers/qsqlmysql.dll、dist/platforms/qwindows.dll
import hashlib, os, shutil, socket, struct, subprocess, sys, time

HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_REG, T_CAP, T_LOGIN = 1, 44, 3
T_MKDIR, T_FLUSH, T_DEL, T_UPINIT, T_UPDATA = 19, 23, 25, 29, 31
T_UPINIT_R, T_UPDATA_R, T_FLUSH_R = 30, 32, 24
T_DOWN, T_DOWN_R = 40, 41
T_SHARE, T_AGREE_R, T_SHARE_FINISH = 33, 36, 37
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
SRV = DIST
FS = os.path.join(DIST, "filesys")
OP, RC = "e2eop1", "e2eop2"
C1 = b"LEGACY-CONTENT-ONE" * 20
C2 = b"UPLOADED-CONTENT-TWO" * 20
C3 = b"LEGACY-DEEP-CONTENT-THREE" * 20
ND = b"NESTED-DELETE-FOUR" * 20
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
    r = c.ask(pdu(T_CAP))
    code = r[0][2].split(b"\0")[0]
    c.ask(pdu(T_REG, msg=code + b"\0", dat=creds(name)))
    c.close()
    c = Conn()
    r = c.ask(pdu(T_CAP)); code = r[0][2].split(b"\0")[0]
    r = c.ask(pdu(T_LOGIN, msg=code + b"\0", dat=creds(name)))
    ret = struct.unpack("<i", r[0][1][:4])[0]
    return c, ret

def drain(c):
    c.sock.settimeout(0.3)
    try:
        while True:
            if not c.sock.recv(65536): break
    except OSError:
        pass
    c.buf = b""

def recv_until(c, t, timeout=6):
    dl = time.time() + timeout
    while time.time() < dl:
        c._pump(0.5)
        pkts = c.packets()
        for p in pkts:
            if p[0] == t:
                return p
    return None

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

def flush_names(c, dirPath):
    r = c.ask(pdu(T_FLUSH, msg=dirPath.encode()))
    names = []
    if r and r[0][0] == T_FLUSH_R:
        m = r[0][2]
        for i in range(len(m) // 36):
            names.append(m[i * 36:i * 36 + 32].split(b"\0")[0].decode("utf-8", "replace"))
    return names

# ---------- 0. 清场（停服+清DB测试行+删测试目录） ----------
stop_server()
for u in (OP, RC):
    sql(f"DELETE FROM user_file WHERE owner_name='{u}';")
    sql(f"DELETE FROM upload_session WHERE owner_name='{u}';")
sql(f"DELETE FROM file_migration_map WHERE old_path LIKE '%filesys/{OP}/%' OR old_path LIKE '%filesys/{RC}/%';")
sql(f"DELETE FROM file_object WHERE sha256 IN ('{SHA(C1)}','{SHA(C2)}','{SHA(C3)}','{SHA(ND)}');")
sql(f"DELETE FROM share_link WHERE owner_name IN ('{OP}','{RC}');")
for u in (OP, RC):
    shutil.rmtree(os.path.join(FS, u), ignore_errors=True)
shutil.rmtree(os.path.join(FS, ".staging"), ignore_errors=True)

# ---------- 1. 起服#1：登录、上传 a.txt(C2)、MKDIR mvd、播未迁移旧文件 ----------
check("S1", "Server#1 启动并监听", start_server())
for u in (OP, RC):
    os.makedirs(os.path.join(FS, u), exist_ok=True)
ca, ret = login_conn(OP)
check("L1", f"{OP} 登录", ret == 1, f"ret={ret}")
cb, ret2 = login_conn(RC)
check("L2", f"{RC} 登录", ret2 == 1, f"ret={ret2}")
check("U1", "上传 a.txt(C2) 成功", upload(ca, f"./filesys/{OP}", "a.txt", C2))
check("U2", "条目 a.txt→对象shaC2",
      sql(f"SELECT COUNT(*) FROM user_file uf JOIN file_object fo ON fo.object_id=uf.object_id "
          f"WHERE uf.owner_name='{OP}' AND uf.display_name='a.txt' AND uf.status='active' AND fo.sha256='{SHA(C2)}'") == "1")
ca.ask(pdu(T_MKDIR, msg=f"./filesys/{OP}".encode(), dat=f32("mvd")))
check("K1", "MKDIR mvd 建目录条目",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{OP}' AND display_name='mvd' AND is_dir=1 AND status='active'") == "1")
base = os.path.join(FS, OP)
os.makedirs(os.path.join(base, "legdir"), exist_ok=True)
open(os.path.join(base, "a.txt"), "wb").write(C1)            # 同名不同内容→(1)+skipped_name_conflict
open(os.path.join(base, "legacyL1.txt"), "wb").write(C1)     # 正常映射
open(os.path.join(base, "legdir", "l2.txt"), "wb").write(C3) # 子目录映射
ca.close(); cb.close()

# ---------- 2. 起服#2：启动迁移 ----------
stop_server()
check("S2", "Server#2 启动（触发 T6 迁移）", start_server())
pLegacy = (base + "/legacyL1.txt").replace("\\", "/")
pConflict = (base + "/a.txt").replace("\\", "/")
pDeep = (base + "/legdir/l2.txt").replace("\\", "/")
# F-3/D5 修复：old_path 为跨树确定相对键 "filesys/<user>/..."（design §2），不再存机器绝对路径
kLegacy = f"filesys/{OP}/legacyL1.txt"
kConflict = f"filesys/{OP}/a.txt"
kDeep = f"filesys/{OP}/legdir/l2.txt"
check("M1", "legacyL1 映射 mapped",
      wait_sql(f"SELECT status FROM file_migration_map WHERE old_path='{kLegacy}'", "mapped"))
check("M2", "a.txt 冲突→skipped_name_conflict",
      wait_sql(f"SELECT status FROM file_migration_map WHERE old_path='{kConflict}'", "skipped_name_conflict"))
check("M3", "a(1).txt 条目落定→对象shaC1",
      sql(f"SELECT COUNT(*) FROM user_file uf JOIN file_object fo ON fo.object_id=uf.object_id "
          f"WHERE uf.owner_name='{OP}' AND uf.display_name='a(1).txt' AND uf.status='active' AND fo.sha256='{SHA(C1)}'") == "1")
check("M4", "深层 legdir/l2.txt 映射+mapped",
      wait_sql(f"SELECT status FROM file_migration_map WHERE old_path='{kDeep}'", "mapped"))
check("M5", "旧文件零删除（验收5）",
      all(os.path.isfile(x) for x in (pLegacy.replace("/", os.sep),
                                       pConflict.replace("/", os.sep),
                                       pDeep.replace("/", os.sep))))
check("M6", "对象 shaC1 refcount=2",
      sql(f"SELECT refcount FROM file_object WHERE sha256='{SHA(C1)}'") == "2")
nMap = sql("SELECT COUNT(*) FROM file_migration_map WHERE old_path LIKE "
           f"'%filesys/{OP}/%'")
nEnt = sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{OP}' AND status='active'")

# ---------- 3. 起服#3：重跑幂等 ----------
stop_server()
check("S3", "Server#3 启动（迁移重跑）", start_server())
time.sleep(2.5)
check("I1", "重跑映射行数不变（幂等）",
      sql("SELECT COUNT(*) FROM file_migration_map WHERE old_path LIKE "
          f"'%filesys/{OP}/%'") == nMap, f"before={nMap}")
check("I2", "重跑条目行数不变",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{OP}' AND status='active'") == nEnt,
      f"before={nEnt}")

# ---------- 4. 条目操作：rename / move / 目录改名回退 / 删除 ----------
ca, ret = login_conn(OP)
cb, ret2 = login_conn(RC)
rootP = f"./filesys/{OP}"
ren = struct.pack("<I", 1) + f"{rootP}".encode()
r = ca.ask(pdu(T_RENAME, msg=ren, dat=f32("legacyL1.txt") + f32("renamed1.txt")))
check("N1", "rename 条目 legacyL1→renamed1", r and r[0][0] == T_RENAME_R and r[0][1][0] != 0)
check("N2", "改名后条目同对象 oid 不变",
      sql(f"SELECT COUNT(*) FROM user_file uf JOIN file_object fo ON fo.object_id=uf.object_id "
          f"WHERE uf.owner_name='{OP}' AND uf.display_name='renamed1.txt' AND fo.sha256='{SHA(C1)}' AND uf.status='active'") == "1")
check("N3", "物理实体同步改名（防幽灵）",
      os.path.isfile(os.path.join(base, "renamed1.txt")) and not os.path.isfile(os.path.join(base, "legacyL1.txt")))
fl = flush_names(ca, rootP)
check("N4", "flushfile 显示 renamed1.txt", "renamed1.txt" in fl, f"names={fl}")
mvdat = f32("l2.txt")
mvmsg = b"legdir/l2.txt\0mvd\0"
r = ca.ask(pdu(T_MOVE, msg=mvmsg, dat=mvdat))
check("MV1", "movefile 条目 legdir/l2.txt→mvd", r and r[0][0] == T_MOVE_R and r[0][1][0] != 0)
check("MV2", "条目 parent_path=/mvd/（owner 相对逻辑键，跨树确定）",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{OP}' AND display_name='l2.txt' "
          f"AND parent_path='/mvd/' AND status='active'") == "1")
ren2 = struct.pack("<I", 0) + f"{rootP}".encode()
r = ca.ask(pdu(T_RENAME, msg=ren2, dat=f32("legdir") + f32("legdir2")))
check("MV3", "未建条目旧目录物理改名回退", r and r[0][0] == T_RENAME_R and r[0][1][0] != 0
      and os.path.isdir(os.path.join(base, "legdir2")) and not os.path.isdir(os.path.join(base, "legdir")))
r = ca.ask(pdu(T_DEL, msg=f"{rootP}/legdir2".encode(), dat=struct.pack("<I", 0)))
check("MV4", "deldir 旧目录物理删除+级联空操作", r and r[0][1][0] != 0
      and not os.path.isdir(os.path.join(base, "legdir2")))

# ---------- 4b. F-4 回归：条目优先 deldir 深度≥2 子树软删 + 目录行引用计数正确 ----------
ca.ask(pdu(T_MKDIR, msg=f"{rootP}".encode(), dat=f32("nd1")))
ca.ask(pdu(T_MKDIR, msg=f"{rootP}/nd1".encode(), dat=f32("nd2")))
check("ND0", "上传 nf.txt 到 nd1/nd2（深度2条目）", upload(ca, f"{rootP}/nd1/nd2", "nf.txt", ND))
check("ND0b", "深层条目键 /nd1/nd2/",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{OP}' AND display_name='nf.txt' "
          f"AND parent_path='/nd1/nd2/' AND status='active'") == "1")
r = ca.ask(pdu(T_DEL, msg=f"{rootP}/nd1".encode(), dat=struct.pack("<I", 0)))
check("ND1", "deldir nd1 条目优先成功", r and r[0][1][0] != 0, f"ret={r[0][1][0] if r else 'NR'}")
check("ND2", "深度≥2 后代 nf.txt 已软删（旧 LIKE '//%' 漏删回归）",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{OP}' AND display_name='nf.txt' "
          f"AND status='deleted'") == "1")
check("ND3", "中间目录 nd2 与 nd1 自身均软删",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{OP}' AND display_name IN ('nd1','nd2') "
          f"AND status='deleted'") == "2")
check("ND4", "nf 对象归零→pending_reclaim，目录行不误减他对象引用（F-4 prepend 列位）",
      wait_sql(f"SELECT status FROM file_object WHERE sha256='{SHA(ND)}'", "pending_reclaim"))
check("ND5", "对象文件不物理删（D3）",
      sql(f"SELECT COUNT(*) FROM file_object WHERE sha256='{SHA(ND)}'") == "1")

# ---------- 5. 分享：条目源→接收者新条目引用同一对象（物理零复制） ----------
sharemsg = f32(RC) + f"{rootP}/renamed1.txt".encode()
ca.ask(pdu(T_SHARE, msg=sharemsg, dat=f32(OP) + struct.pack("<i", 1)))
time.sleep(0.5)
drain(cb)   # 清掉转发过来的 AGREE_REQUEST(35) 等历史包
cb.sock.sendall(pdu(T_AGREE_R, msg=f"{rootP}/renamed1.txt".encode(), dat=f32(RC)))
r = recv_until(cb, T_SHARE_FINISH)
check("SH1", "接收者同意→完成响应 ret=true", r and r[1][0] != 0, f"pkt={r[:2] if r else None}")
check("SH2", "接收者条目引用同一对象(shaC1)",
      sql(f"SELECT COUNT(*) FROM user_file uf JOIN file_object fo ON fo.object_id=uf.object_id "
          f"WHERE uf.owner_name='{RC}' AND uf.display_name='renamed1.txt' AND fo.sha256='{SHA(C1)}' AND uf.status='active'") == "1")
check("SH3", "接收者根内无物理复制（D2 红利）",
      not os.path.isfile(os.path.join(FS, RC, "renamed1.txt")))
check("SH4", "shaC1 refcount=3",
      wait_sql(f"SELECT refcount FROM file_object WHERE sha256='{SHA(C1)}'", "3"))
cb.sock.sendall(pdu(T_DOWN, msg=f"./filesys/{RC}/renamed1.txt".encode(), dat=f32("renamed1.txt")))
r = recv_until(cb, T_DOWN_R)
check("SH5", "接收者可下载条目", r and r[1][0] != 0, f"pkt={r[:2] if r else None}")
negmsg = f32(RC) + f"{rootP}/a.txt".encode()
drain(cb)
cb.ask(pdu(T_SHARE, msg=negmsg, dat=f32(RC) + struct.pack("<i", 1)))
time.sleep(0.3)
drain(cb)
cb.sock.sendall(pdu(T_AGREE_R, msg=f"{rootP}/a.txt".encode(), dat=f32(RC)))
r = recv_until(cb, T_SHARE_FINISH)
check("SH6", "分享他人文件→无登记→同意被拒（B-1 不回退）",
      r and r[1][0] == 0, f"pkt={r[:2] if r else None}")

# ---------- 6. 验收4：伪造大小（staging 篡改）→ failed+无条目；跨用户目录上传拒绝 ----------
r = ca.ask(pdu(T_UPINIT, msg=f"{rootP}/mvd".encode(),
               dat=f32("forged.bin") + struct.pack("<q", 600) + struct.pack("<I", MAGIC)))
check("F0", "forged.bin 初始化接受（复算在服务端）", r and r[0][1][0] != 0)
a = ca.ask(pdu(T_UPDATA, msg=b"A" * 200,
                dat=struct.pack("<q", 0) + b"\0" * 32 + struct.pack("<I", MAGIC)))
stag = sql(f"SELECT staging_path FROM upload_session WHERE owner_name='{OP}' AND display_name='forged.bin' ORDER BY session_id DESC LIMIT 1")
if stag:
    open(stag.replace("/", os.sep), "ab").write(b"T" * 500)   # 中途篡改：staging 超出登记大小
a = ca.ask(pdu(T_UPDATA, msg=b"B" * 400,
                dat=struct.pack("<q", 200) + b"\0" * 32 + struct.pack("<I", MAGIC)))
check("F1", "伪造大小→会话 status=failed（验收4）",
      wait_sql(f"SELECT status FROM upload_session WHERE owner_name='{OP}' AND display_name='forged.bin' "
               "ORDER BY session_id DESC LIMIT 1", "failed"))
check("F2", "不建条目（无 forged.bin）",
      sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{OP}' AND display_name='forged.bin'") == "0")
check("F3", "staging 清理（失败即删暂存）", stag != "" and not os.path.exists(stag.replace("/", os.sep)))
r = ca.ask(pdu(T_UPINIT, msg=f"./filesys/{RC}".encode(),
               dat=f32("x.bin") + struct.pack("<q", 10) + struct.pack("<I", MAGIC)))
check("F4", "上传到他人目录→初始化拒绝", r and r[0][1][0] == 0)
r = ca.ask(pdu(T_RENAME, msg=struct.pack("<I", 1) + f"./filesys/{RC}".encode(),
                dat=f32("a.txt") + f32("b.txt")))
check("F5", "他人目录改名→拒绝", r and r[0][0] == T_RENAME_R and r[0][1][0] == 0)
ca.close(); cb.close()

# ---------- 7. 收尾：停止服务器（回归脚本自行再启） ----------
stop_server()
print("\n=== 汇总 ===")
passed = sum(1 for _, _, ok in results if ok)
for sid, desc, ok in results:
    if not ok: print(f"  FAIL -> {sid} {desc}")
print(f"{passed}/{len(results)} passed")
sys.exit(0 if passed == len(results) else 1)
