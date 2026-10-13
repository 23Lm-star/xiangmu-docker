# REQ-EXT-TRANSFER-003 上传会话连续性与吞吐：验收套件（静态 S-01..S-05 + 动态 T3-01..T3-12）。
# 运行形态遵循 project-docs/evidence/E2E-RUNFORM.md：cwd 任意；脚本自管理 dist/Server.exe（按 PID），
# 串行独占 127.0.0.1:5000，DB = mydb（mysql CLI），运行树 = dist/filesys。
#
# 断言口径：本套件为**裸 socket** 驱动，覆盖 design §6.1/§6.2 全部机器面。
#   - T3-06 吞吐为同机同法对比：基线（改前 4 KB 分片、每块 open/close）0.28 MiB/s，阈值 8 MiB/s。
#   - 纯 GUI 面（§6.3 五项：肉眼速度、面板进度、暂停继续、0 字节显示、G7 断线收敛的手工演练）
#     不在本套件可断言范围，见文件末尾 NOTE。
#   - 客户端本地行为（C1/C2 分片计算、C3 空文件直接 Done、C4 摘旧映射、C5-C9 断线收敛）以**源码静态
#     契约**断言（S-02/S-04/S-05 与 T3-13），动态面需客户端宿主，仍属人工清单。
import hashlib, os, re, shutil, socket, struct, subprocess, sys, time

HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_REG, T_CAP, T_CAP_R, T_LOGIN, T_LOGIN_R, T_PING, T_PONG = 1, 44, 45, 3, 4, 50, 51
T_MKDIR, T_MKDIR_R, T_FLUSH, T_FLUSH_R = 19, 20, 23, 24
T_UPINIT, T_UPDATA, T_UPINIT_R, T_UPDATA_R = 29, 31, 30, 32
T_FC, T_FC_R = 97, 98
T_DOWN, T_DOWN_R, T_DATA, T_FINISH = 40, 41, 42, 43
T_ABORT, T_ABORT_R = 107, 108
T_GCREATE, T_GCREATE_R = 60, 61
T_GF_LIST, T_GF_LIST_R, T_GF_INIT, T_GF_INIT_R = 99, 100, 101, 102
MAGIC = 0x5253554d
FCHK_MAGIC, FCHK_VERSION = 0x4643484b, 1
ID_OFF = 48
DL_ID_OFF = 16
MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
DIST = os.path.join(REPO, "dist")
EXE = os.path.join(DIST, "Server.exe")
sys.path.insert(0, os.path.join(REPO, "project-docs", "evidence", "_harness"))
import server_ctl
_SRV = server_ctl.ServerCtl(EXE, DIST, HOST, PORT)
U = "e2etx3"
DST = f"./filesys/{U}/tx3"
SHA = lambda b: hashlib.sha256(b).hexdigest()
NONCE = f"tx3{int(time.time())}".encode()
CHUNK = 1024 * 1024          # 服务端 MAX_UPLOAD_DATA_CHUNK
LEGACY_CHUNK = 4096          # 旧客户端分片


def f32(s): return s.encode("utf-8")[:31].ljust(32, b"\0")
def creds(n, p="123456"): return f32(n) + hashlib.md5(p.encode()).hexdigest().encode()
def pdu(t, msg=b"", dat=b""):
    dat = dat[:64].ljust(64, b"\0")
    return struct.pack("<III", HDR + len(msg), len(msg), t) + dat + msg
def u16(x): return struct.pack("<H", x)
def u32(x): return struct.pack("<I", x)
def u64(x): return struct.pack("<Q", x)
def i64(x): return struct.pack("<q", x)
def string(x): return u32(len(x)) + x
def u32at(b, o): return struct.unpack("<I", b[o:o + 4])[0]
def u64at(b, o): return struct.unpack("<Q", b[o:o + 8])[0]
def i64at(b, o): return struct.unpack("<q", b[o:o + 8])[0]
def eid(dat, off=ID_OFF):
    v = u64at(dat, off)
    return v - (1 << 64) if v >= (1 << 63) else v
def envelope(gid, body=b""):
    return u16(1) + u16(20) + u64(int(time.time_ns() & ((1 << 63) - 1))) + u64(gid) + body


class Conn:
    def __init__(s):
        s.sock = socket.create_connection((HOST, PORT), timeout=10)
        s.buf = b""; s.pending = []

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
        s.pending.extend(out)
        return out

    def wait(s, ty, timeout=8.0):
        dl = time.time() + timeout
        while time.time() < dl:
            for i, p in enumerate(s.pending):
                if p[0] == ty:
                    return s.pending.pop(i)
            s.pump(0.2)
            s.packets()
        return None

    def take(s, t=0.1):
        s.pump(t); s.packets()
        out = s.pending[:]; s.pending = []
        return out

    def send(s, pkt): s.sock.sendall(pkt)

    def close(s):
        try: s.sock.close()
        except OSError: pass


def sql(q):
    return subprocess.run([MYSQL] + DB + [q], capture_output=True, text=True).stdout.strip()


def sql_row(q):
    out = sql(q)
    if not out:
        return None
    return out.splitlines()[0].split("\t")


results = []
def check(sid, desc, cond, detail=""):
    results.append((sid, desc, bool(cond)))
    print(f"[{'PASS' if cond else 'FAIL'}] {sid} {desc} :: {detail}")


def login_conn(name):
    c = Conn()
    c.send(pdu(T_CAP)); r = c.wait(T_CAP_R)
    code = r[2].split(b"\0")[0] if r else b""
    c.send(pdu(T_REG, msg=code + b"\0", dat=creds(name))); c.wait(T_REG + 1, 4)
    c.close()
    c = Conn()
    c.send(pdu(T_CAP)); r = c.wait(T_CAP_R); code = r[2].split(b"\0")[0] if r else b""
    c.send(pdu(T_LOGIN, msg=code + b"\0", dat=creds(name))); r = c.wait(T_LOGIN_R, 6)
    ret = struct.unpack("<i", r[1][:4])[0] if r else -99
    return c, ret


# ---------- 传输原语（逐字节照抄 wire 布局） ----------
def up_init(c, tid, dirPath, name, size):
    c.send(pdu(T_UPINIT, msg=dirPath.encode(),
               dat=f32(name) + i64(size) + u32(MAGIC) + b"\0" * 4 + u64(tid)))
    return c.wait(T_UPINIT_R)


def up_chunk(c, tid, off, data, wait=True):
    c.send(pdu(T_UPDATA, msg=data,
               dat=i64(off) + b"\0" * 32 + u32(MAGIC) + b"\0" * 4 + u64(tid)))
    return c.wait(T_UPDATA_R) if wait else None


def ack_ok(r):
    return r is not None and r[1][0] != 0


def ack_next(r):
    return i64at(r[1], 8) if r else -1


def ack_complete(r):
    return bool(r[1][16]) if r else False


def init_off(r):
    """30 的续传起点在 caData[32]（UPLOAD_RESUME_OFFSET），与 32 的 [8] 不同"""
    return i64at(r[1], 32) if r else -1


def fc_next(r):
    """98 的 miss 续传起点在 caData[24]（FASTCHECK_RESP_NEXT_OFFSET）"""
    return i64at(r[1], 24) if r else -1


def abort(c, tid):
    c.send(pdu(T_ABORT, dat=b"\0" * ID_OFF + u64(tid)))
    return c.wait(T_ABORT_R)


def fc(c, sha, name, dirPath, size, reqId):
    nb, db, rb = name.encode(), dirPath.encode(), reqId.encode()
    c.send(pdu(T_FC, msg=sha.encode() + nb + db + rb,
               dat=u32(FCHK_VERSION) + u32(FCHK_MAGIC) + i64(size) + u32(64)
                   + u32(len(nb)) + u32(len(db)) + u32(len(rb))))
    return c.wait(T_FC_R, 10)


def dl_req(c, tid, path, name):
    c.send(pdu(T_DOWN, msg=path.encode(), dat=f32(name) + b"\0" * 16 + u64(tid)))
    return c.wait(T_DOWN_R)


def collect(c, ids, timeout=30.0):
    got = {i: {"data": b"", "finish": None, "ok": None} for i in ids}
    dl = time.time() + timeout
    while time.time() < dl:
        for ty, dat, msg in c.take(0.2):
            tid = eid(dat, DL_ID_OFF if ty == T_DOWN_R else ID_OFF)
            if tid not in got: continue
            if ty == T_DOWN_R: got[tid]["ok"] = dat[0] != 0
            elif ty == T_DATA: got[tid]["data"] += msg
            elif ty == T_FINISH: got[tid]["finish"] = dat[0] != 0
        if all(v["finish"] is not None for v in got.values()): break
    return got


def download(c, tid, path, name, timeout=30.0):
    dl_req(c, tid, path, name)
    return collect(c, [tid], timeout)[tid]


def mkdir(c, parent, name):
    c.send(pdu(T_MKDIR, msg=parent.encode(), dat=f32(name)))
    return c.wait(T_MKDIR_R)


def gf_init(c, tid, gid, size, sha, name):
    c.send(pdu(T_GF_INIT, msg=envelope(gid, u64(size) + string(sha.encode()) + string(name.encode())),
               dat=b"\0" * ID_OFF + u64(tid)))
    return c.wait(T_GF_INIT_R)


def gf_next(r):
    """102 体：信封 24B 起 [hit+reserved 4B][fileId 8][groupFileId 8][sessionId 8][nextOffset 8]"""
    return i64at(r[2], 52) if r else -1


# ---------- DB 观测面 ----------
def rows(display):
    out = sql(f"SELECT session_id,status,confirmed_offset,staging_path,IFNULL(client_request_id,'-') "
              f"FROM upload_session WHERE owner_name='{U}' AND display_name='{display}' "
              f"ORDER BY session_id")
    if not out:
        return []
    return [ln.split("\t") for ln in out.splitlines()]


def row0(display):
    rs = rows(display)
    return rs[-1] if rs else None


def staging_size(display):
    r = row0(display)
    if r is None or not r[3]:
        return -1
    try:
        return os.path.getsize(r[3]) if os.path.isabs(r[3]) else \
            os.path.getsize(os.path.join(DIST, r[3]))
    except OSError:
        return -1


def obj_size(display):
    out = sql(f"SELECT fo.size_bytes,fo.sha256 FROM user_file uf JOIN file_object fo "
              f"ON fo.object_id=uf.object_id WHERE uf.owner_name='{U}' AND uf.display_name='{display}' "
              f"AND uf.status='active'")
    if not out:
        return (-1, "")
    p = out.splitlines()[0].split("\t")
    return (int(p[0]), p[1])


def read(p): return open(os.path.join(REPO, p), encoding="utf-8").read()


# ================= 静态：冻结面 + 客户端契约 =================
print("=== 静态：S-01..S-05 ===")
APPROVED = """static constexpr uint MAX_INBOUND_PDU_SIZE = 16 * 1024;   // 通用入站单包上限（16 KB）——REQ-EXT-TRANSFER-003 抬高的只有上传数据通道
// ===== REQ-EXT-TRANSFER-003：上传数据通道（31）分片上限（仅 31 生效；客户端 Client/uploader.h 的 UPLOAD_CHUNK_MAX 须 ≤ 此处） =====
static constexpr uint MAX_UPLOAD_DATA_CHUNK = 1024 * 1024;                    // 31 单块正文上限 1 MiB
static constexpr uint MAX_UPLOAD_DATA_PDU_SIZE = MAX_UPLOAD_DATA_CHUNK + 4096; // 31 整包上限 = 正文 + 头/余量
static constexpr int  MAX_PENDING_INBOUND_BYTES = 1024 * 1024 + 8192;         // 每连接入站累积上界（≥ 单包上限 + 余量；原 256 KB）
"""
srvs = read("Server/protocol.h")
block = "\n".join(srvs.split("// ===== REQ-EXT-TRANSFER-003", 1)[1].splitlines()[:5]) \
    if "REQ-EXT-TRANSFER-003" in srvs else ""
block = "static constexpr uint MAX_INBOUND_PDU_SIZE = 16 * 1024;" \
        + srvs.split("static constexpr uint MAX_INBOUND_PDU_SIZE = 16 * 1024;", 1)[1][:0] \
        + "\n" + block if block else ""
# 直接按行取：从 MAX_INBOUND_PDU_SIZE 行起 5 行 == §9 批准稿
lines = srvs.splitlines()
idx = next((i for i, ln in enumerate(lines)
            if ln.startswith("static constexpr uint MAX_INBOUND_PDU_SIZE")), -1)
actual_block = "\n".join(lines[idx:idx + 5]) + "\n" if idx >= 0 else ""
check("S-01a", "Server/protocol.h REQ-EXT-TRANSFER-003 常量块逐字节 == §9 批准稿",
      actual_block == APPROVED, f"len={len(actual_block)}")
check("S-01b", "通用入站上限 MAX_INBOUND_PDU_SIZE 未被抬高（仍 16 KB）",
      "static constexpr uint MAX_INBOUND_PDU_SIZE = 16 * 1024;" in srvs, "")

cl = read("Client/uploader.h")
cmax = re.search(r"UPLOAD_CHUNK_MAX\s*=\s*([^;]+);", cl)
cmin = re.search(r"UPLOAD_CHUNK_MIN\s*=\s*([^;]+);", cl)
cnt = re.search(r"UPLOAD_CHUNK_TARGET_COUNT\s*=\s*([^;]+);", cl)
smax = re.search(r"MAX_UPLOAD_DATA_CHUNK\s*=\s*([^;]+);", srvs)
def evalint(e):
    try: return int(eval(e, {"__builtins__": {}}, {}))
    except Exception: return -1
vmax = evalint(cmax.group(1)) if cmax else -1
check("S-02a", "Client/uploader.h 三常量存在（1024 块 / 64 KiB 下限 / 1 MiB 上限）",
      cmax is not None and cmin is not None and cnt is not None
      and vmax == CHUNK and evalint(cmin.group(1)) == 65536 and evalint(cnt.group(1)) == 1024,
      f"max={vmax}")
check("S-02b", "跨端一致：UPLOAD_CHUNK_MAX ≤ 服务端 MAX_UPLOAD_DATA_CHUNK",
      0 < vmax <= evalint(smax.group(1)) if smax else False, f"{vmax} <= {evalint(smax.group(1)) if smax else '?'}")

# 基准必须是"批准改动引入之前"的提交，不能是移动的 master：master 一旦合并本改动，
# `git diff master -- Server/protocol.h` 恒为空，S-03a/b 在任何 master 后代树上都空转失败
# （实测 rem=[] / n=0）。固定基准 = 本改动落地提交 b861351 之前最后一次改本文件的提交
# （f820e61，002 开发提交）。用 b861351^ 会连 002 的常量块一起带出，S-03b 逐字比对即失败。
T003_FROZEN_BASE = os.environ.get("T003_FROZEN_BASE", "f820e614bb45fe0c36b255f8a97f989fa9273ad4")
diff_srv = subprocess.run(["git", "diff", T003_FROZEN_BASE, "--", "Server/protocol.h"],
                          cwd=REPO, capture_output=True, text=True).stdout
rem = [l[1:].strip() for l in diff_srv.splitlines()
       if l.startswith("-") and not l.startswith("---") and l[1:].strip()]
add = [l[1:] for l in diff_srv.splitlines()
       if l.startswith("+") and not l.startswith("+++")]


def _drop_foreign_blocks(lines):
    """剔除"别的需求自己的常量块"（`// ===== REQ-EXT-<other>` 起、到下一个块头止）。

    本护栏要守的是"本需求的冻结面块没被动过"，不是"冻结面里不许出现别的需求的块"。
    冻结面变更本就按需求逐块批准（本次即 REQ-EXT-SHARE-001），整 diff 逐字比对会把
    任何一次合法的后续追加判成违规。
    """
    kept, i = [], 0
    while i < len(lines):
        if lines[i].startswith("// ===== REQ-EXT-") and "REQ-EXT-TRANSFER-003" not in lines[i]:
            i += 1
            while i < len(lines) and not lines[i].startswith("// ===== REQ-EXT-"):
                i += 1
            continue
        kept.append(lines[i])
        i += 1
    return kept


own_add = [l for l in _drop_foreign_blocks(add) if l.strip()]
check("S-03a", "冻结面 diff 删除行 == 原两行常量（无其它删改）",
      sorted(rem) == sorted(["static constexpr uint MAX_INBOUND_PDU_SIZE = 16 * 1024;",
                             "static constexpr int MAX_PENDING_INBOUND_BYTES = 256 * 1024;"]),
      f"rem={rem}")
check("S-03b", "剔除其它需求的已批准常量块后，冻结面新增行 == §9 批准稿五行（逐字）",
      "\n".join(own_add) + "\n" == APPROVED, f"n={len(own_add)} (raw n={len(add)})")
diff_cli = subprocess.run(["git", "diff", "master", "--", "Client/protocol.h"],
                          cwd=REPO, capture_output=True, text=True).stdout
_cli_lines = (diff_cli or "").splitlines()
_cli_rem = [l for l in _cli_lines if l.startswith("-") and not l.startswith("---") and l[1:].strip()]
_cli_add = [l[1:] for l in _cli_lines if l.startswith("+") and not l.startswith("+++")]
_segs, _cur = [], []
for _l in _cli_add:
    if _l.strip() == "":
        continue
    if _l.startswith("// ===== REQ-EXT-") and _cur:
        _segs.append(_cur)
        _cur = []
    _cur.append(_l)
if _cur:
    _segs.append(_cur)
_cli_unmarked = [s[0][:60] for s in _segs if not s[0].startswith("// ===== REQ-EXT-")]
check("S-03c", "Client/protocol.h 未被删改；追加行全在 REQ-EXT-* 已批准块内（本需求自身零改动）",
      (not _cli_rem) and (not _cli_unmarked), f"rem={_cli_rem} unmarked={_cli_unmarked}")

cli = read("Client/client.cpp")
tt = read("Client/transfertask.cpp")
d1 = re.search(r"void Client::onSocketDisconnected\(\).*?\n\}", cli, re.S)
d2 = re.search(r"void Client::onServerLost\(\).*?\n\}", cli, re.S)
check("S-04a", "G7 接线①：onSocketDisconnected 调用 onConnectionLost",
      d1 is not None and "onConnectionLost(" in d1.group(0), "")
check("S-04b", "G7 接线②：onServerLost 调用 onConnectionLost",
      d2 is not None and "onConnectionLost(" in d2.group(0), "")
pump = re.search(r"void TransferManager::pump\(\)\s*\{(.{0,200})", tt, re.S)
check("S-04c", "G7 接线③：pump() 首部有 m_connectionDown 闸",
      pump is not None and "m_connectionDown" in pump.group(1), "")
lr = re.search(r"case ENUM_MSG_TYPE_LOGIN_RESPOND:\s*\{(.*?)\n\s*break;", cli, re.S)
check("S-04d", "G7 接线④：LOGIN_RESPOND 分支调用 onConnectionRestored",
      lr is not None and "onConnectionRestored(" in lr.group(1), "")
sha_cb = re.search(r"QFutureWatcher<QByteArray>::finished.*?\n    \}\);", tt, re.S)
check("S-05a", "G7 防复活①：群上传 SHA 回调含 state != Hashing 守卫",
      sha_cb is not None and re.search(r"state\s*!=\s*Hashing", sha_cb.group(0)) is not None, "")
sub = re.search(r"void TransferManager::submitFastcheck\(.*?\n\}", tt, re.S)
check("S-05b", "G7 防复活②：submitFastcheck 含 state != Hashing 守卫",
      sub is not None and re.search(r"state\s*!=\s*Hashing", sub.group(0)) is not None, "")
check("S-05c", "C4 摘旧映射：pause/resume 上传分支均 m_idToKey.remove(t->id)",
      len(re.findall(r"m_idToKey\.remove\(t->id\)", tt)) >= 3, "")

# ---- G7 修复面（FINDING-IND-7 / R-1 + D-a + D-b）源码静态契约：拦住这四处回归 ----
mh = read("Client/msghandler.cpp")
mhh = read("Client/msghandler.h")
lr_body = lr.group(1) if lr is not None else ""
m_assign = re.search(r"m_LoginName\s*=", lr_body)
i_setuser = lr_body.find("setCurrentUser(")
check("S-06a", "R-1：LOGIN_RESPOND 分支内登录名赋值先于 setCurrentUser（同步取值，非 1.5 s 后）",
      m_assign is not None and i_setuser > m_assign.start() and "handllogin()" in lr_body,
      f"assign@{m_assign.start() if m_assign else -1} setUser@{i_setuser}")
hl = re.search(r"QString MsgHandler::handllogin\(\)\s*\{(.*?)\n\}", mh, re.S)
hlb = hl.group(1) if hl else ""
check("S-06b", "R-1：handllogin 同步返回登录名且 1.5 s 定时器内不再写 m_LoginName",
      hl is not None and "return loginName" in hlb
      and re.search(r"m_LoginName\s*=", hlb) is None
      and re.search(r"QString\s+handllogin\s*\(\s*\)", mhh) is not None,
      f"ret={'return loginName' in hlb} in_body_assign={re.search(r'm_LoginName\\s*=', hlb) is not None}")
snp = re.search(r"void TransferManager::saveSnapshot\(\)\s*\{(.*?)\n\}", tt, re.S)
sb = snp.group(1) if snp else ""
sb_lines = sb.splitlines()
unwarned = [ln.strip() for i, ln in enumerate(sb_lines)
            if re.search(r"\breturn\s*;", ln) and "qCWarning" not in "\n".join(sb_lines[max(0, i - 4):i])]
check("S-06c", "D-a：saveSnapshot 无静默丢写（每条 return 前都有 qCWarning 告警）",
      snp is not None and "qCWarning" in sb and not unwarned, f"unwarned={unwarned}")
check("S-06d", "D-a：saveSnapshot 以最近一次有效登录名兜底（m_lastKnownUser）",
      snp is not None and "m_lastKnownUser" in sb, "")
sc = re.search(r"void TransferManager::setCurrentUser\(.*?\)\s*\{(.*?)\n\}", tt, re.S)
scb = sc.group(1) if sc else ""
sc_lines = scb.splitlines()
ind = lambda s: len(s) - len(s.lstrip())
ln_guard = next((i for i, l in enumerate(sc_lines) if "!m_currentUser.isEmpty()" in l), -1)
ln_clear = next((i for i, l in enumerate(sc_lines) if "m_tasks.clear()" in l), -1)
same_ret = re.search(r"name\s*==\s*m_currentUser[^\n]*return", scb)
check("S-06e", "D-b：setCurrentUser 同名/空名不清表；清表嵌于\"另一个账号\"条件内且仍调 restoreSavedTasks",
      sc is not None and same_ret is not None and 0 <= ln_guard < ln_clear
      and ind(sc_lines[ln_clear]) > ind(sc_lines[ln_guard])
      and "restoreSavedTasks()" in scb,
      f"same_ret={same_ret is not None} guard_line={ln_guard} clear_line={ln_clear} "
      f"nested={ln_guard >= 0 and ln_clear > ln_guard and ind(sc_lines[ln_clear]) > ind(sc_lines[ln_guard])}")
rs = re.search(r"void TransferManager::restoreSavedTasks\(\)\s*\{(.*?)\n\}", tt, re.S)
rsb = rs.group(1) if rs else ""
i_dedup = rsb.find("containsIdentity(")
i_insert = rsb.find("m_tasks.insert(")
check("S-06f", "D-b：restoreSavedTasks 按身份去重合并（快照补缺、内存为准，不重复建行）",
      rs is not None and 0 <= i_dedup < i_insert and "continue;" in rsb[i_dedup:i_insert],
      f"dedup@{i_dedup} insert@{i_insert}")

# ================= 环境准备 =================
print("=== 环境准备 ===")
if not _SRV.start():
    print(f"FATAL: {_SRV.last_error}"); sys.exit(2)
sql(f"DELETE FROM user_file WHERE owner_name='{U}';")
sql(f"DELETE FROM upload_session WHERE owner_name='{U}';")
sql(f"DELETE FROM group_file WHERE uploader_name='{U}';")
sql(f"DELETE FROM group_member WHERE user_name='{U}';")
sql(f"DELETE FROM group_info WHERE creator_name='{U}';")
shutil.rmtree(os.path.join(DIST, "filesys", ".staging"), ignore_errors=True)

c, ret = login_conn(U)
check("E0", "登录成功", ret == 1, f"ret={ret}")
if ret != 1: sys.exit(2)
# 用户根只在首次注册时由服务端创建；MKDIR 对已存在目录回 ret=false（非幂等），
# 故本套件自建目录并以"目录存在"为断言口径（见 E2E-RUNFORM.md 的既有约定）。
os.makedirs(os.path.join(DIST, "filesys", U, "tx3"), exist_ok=True)
mkdir(c, f".filesys/{U}", "tx3")
check("E0b", "测试目录就绪", os.path.isdir(os.path.join(DIST, "filesys", U, "tx3")), DST)

# ================= T3-01：进度落库节流 =================
print("=== T3-01 ===")
pl = NONCE + b"T301" + bytes(range(256)) * 16384          # 4 MiB
pl = pl[:4 * 1024 * 1024]
name = "t301.bin"
r = up_init(c, 101, DST, name, len(pl))
check("T3-01-init", "4 MiB INIT ret=true", ack_ok(r), f"off={init_off(r)}")
samples = []
for i in range(0, len(pl), CHUNK):
    a = up_chunk(c, 101, i, pl[i:i + CHUNK])
    rw = row0(name)
    samples.append(int(rw[2]) if rw else -1)
check("T3-01a", "上传中进度单调不减", all(samples[i] <= samples[i + 1] for i in range(len(samples) - 1)),
      f"{samples}")
check("T3-01b", "末次采样 ≥ 已发字节 − 1 MiB（节流上限）",
      samples[-1] >= len(pl) - CHUNK, f"last={samples[-1]} sent={len(pl)}")
rw = row0(name)
check("T3-01c", "完成后 confirmed_offset == expected_size 且 status=done",
      rw is not None and int(rw[2]) == len(pl) and rw[1] == "done",
      f"{rw[1] if rw else '?'}/{rw[2] if rw else '?'}")
d = download(c, 1001, f"{DST}/{name}", name)
check("T3-01d", "4 MiB 逐字节正确（转正对象可下载且内容一致）", d["data"] == pl, f"len={len(d['data'])}")

# ================= T3-02：暂停→续传单行贯穿 =================
print("=== T3-02 ===")
pl = NONCE + b"T302" + b"B" * (3 * 1024 * 1024 - 4)
pl = pl[:3 * 1024 * 1024]
name = "t302.bin"
up_init(c, 201, DST, name, len(pl))
up_chunk(c, 201, 0, pl[:CHUNK])
r = abort(c, 201)
rw = rows(name)
check("T3-02a", "107 后：108 ret=true 且该逻辑上传仅 1 行（status=expired）",
      r is not None and r[1][0] != 0 and len(rw) == 1 and rw[0][1] == "expired",
      f"108={r[1][0] if r else '?'} rows={len(rw)}")
r = up_init(c, 202, DST, name, len(pl))
off = init_off(r)
for i in range(off, len(pl), CHUNK):
    up_chunk(c, 202, i, pl[i:i + CHUNK])
rw = rows(name)
check("T3-02b", "续传（新 id 29）→ 完成后仍 1 行且 status=done、confirmed=size",
      len(rw) == 1 and rw[0][1] == "done" and int(rw[0][2]) == len(pl) and off == CHUNK,
      f"rows={len(rw)} off={off}")
d = download(c, 1002, f"{DST}/{name}", name)
check("T3-02c", "续传结果逐字节正确", d["data"] == pl, f"len={len(d['data'])}")

# ================= T3-03：97 行复用键（改键收敛） =================
print("=== T3-03 ===")
pl = NONCE + b"T303" + b"C" * (2 * 1024 * 1024 - 4)
pl = pl[:2 * 1024 * 1024]
name = "t303.bin"
rq1, rq2 = "1a" * 16, "2b" * 16
r = fc(c, SHA(pl), name, DST, len(pl), rq1)
check("T3-03-init", "97 miss：98 ret=true/hit=false", ack_ok(r) and r[1][1] == 0,
      f"hit={r[1][1] if r else '?'}")
r = up_init(c, 301, DST, name, len(pl))
up_chunk(c, 301, 0, pl[:CHUNK])
r = fc(c, SHA(pl), name, DST, len(pl), rq2)
rw = rows(name)
check("T3-03a", "新 reqId 再 97：未完成会话仍仅 1 行", len(rw) == 1, f"rows={len(rw)}")
check("T3-03b", "client_request_id 改写为最后一次 reqId", rw and rw[0][4] == rq2,
      f"{rw[0][4] if rw else '?'}")
off = fc_next(r)
for i in range(max(off, CHUNK), len(pl), CHUNK):
    up_chunk(c, 301, i, pl[i:i + CHUNK])
rw = rows(name)
check("T3-03c", "完成后仍 1 行且 done", len(rw) == 1 and rw[0][1] == "done", f"rows={len(rw)}")

# ================= T3-04：群路径单行 =================
print("=== T3-04 ===")
c.send(pdu(T_GCREATE, msg=envelope(0, string(b"tx3-group"))))
r = c.wait(T_GCREATE_R)
GID = u64at(r[2], 24) if r and u32at(r[2], 20) == 0 else 0
check("T3-04-init", "建群成功", GID > 0, f"gid={GID}")
pl = NONCE + b"T304" + b"D" * (2 * 1024 * 1024 - 4)
pl = pl[:2 * 1024 * 1024]
gname = "t304_group.bin"
r = gf_init(c, 401, GID, len(pl), SHA(pl), gname)
check("T3-04-init2", "101 未命中续传起点 0", r is not None and gf_next(r) == 0,
      f"off={gf_next(r)}")
a1 = up_chunk(c, 401, 0, pl[:CHUNK])
abort(c, 401)
grp = sql(f"SELECT session_id,status,confirmed_offset FROM upload_session WHERE owner_name='{U}' "
          f"AND parent_path='/' AND display_name='{gname}'")
gn = len(grp.splitlines()) if grp else 0
r = gf_init(c, 402, GID, len(pl), SHA(pl), gname)
off = gf_next(r)
for i in range(off, len(pl), CHUNK):
    up_chunk(c, 402, i, pl[i:i + CHUNK])
grp2 = sql(f"SELECT status,confirmed_offset FROM upload_session WHERE owner_name='{U}' "
           f"AND parent_path='/' AND display_name='{gname}'")
check("T3-04a", "群上传 101→107→101 全程 1 行且续传起点=已收字节",
      ack_ok(a1) and gn == 1 and len(grp2.splitlines()) == 1 and off == CHUNK,
      f"ack={ack_ok(a1)} rows={gn}->{len(grp2.splitlines())} off={off}")
check("T3-04b", "群上传完成落定（done 且 confirmed=size）",
      grp2.splitlines()[0].split("\t")[0] == "done"
      and int(grp2.splitlines()[0].split("\t")[1]) == len(pl), f"{grp2}")

# ================= T3-05：大分片合法 / 超长块显式拒绝 =================
print("=== T3-05 ===")
pl = NONCE + b"T305" + b"E" * (3 * 1024 * 1024 - 4)
pl = pl[:3 * 1024 * 1024]
name = "t305.bin"
up_init(c, 501, DST, name, len(pl))
a = up_chunk(c, 501, 0, pl[:CHUNK])
check("T3-05a", "单块 1 MiB 接受（32 ok=true）", ack_ok(a), f"next={ack_next(a)}")
a = up_chunk(c, 501, CHUNK, pl[CHUNK:CHUNK + CHUNK + 1])
check("T3-05c", "1 MiB + 1 字节块被 32{ok=false} 明确拒绝",
      a is not None and a[1][0] == 0, f"ok={a[1][0] if a else '?'}")
a = up_chunk(c, 501, CHUNK, pl[CHUNK:CHUNK + CHUNK])
check("T3-05d", "拒绝后连接存活且后续合法块被接受", ack_ok(a) and ack_next(a) == 2 * CHUNK,
      f"next={ack_next(a)}")
up_chunk(c, 501, 2 * CHUNK, pl[2 * CHUNK:])
d = download(c, 1005, f"{DST}/{name}", name)
check("T3-05b", "大分片上传结果逐字节正确", d["data"] == pl, f"len={len(d['data'])}")

# ================= T3-06：吞吐（新分片策略） =================
print("=== T3-06 ===")
size = 8 * 1024 * 1024
pl = NONCE + b"T306" + os.urandom(0) + b"F" * (size - 4 - len(NONCE))
pl = pl[:size]
name = "t306.bin"
up_init(c, 601, DST, name, len(pl))
t0 = time.time()
for i in range(0, len(pl), 65536):
    a = up_chunk(c, 601, i, pl[i:i + 65536])
    if not ack_ok(a):
        break
dt = max(time.time() - t0, 1e-6)
mbps = (len(pl) / 1048576.0) / dt
check("T3-06a", "8 MiB / 64 KiB 分片吞吐 ≥ 8 MiB/s（基线 0.28 MiB/s @4 KB）",
      mbps >= 8.0, f"{mbps:.2f} MiB/s ({dt:.2f}s)")
d = download(c, 1006, f"{DST}/{name}", name)
check("T3-06b", "吞吐路径结果逐字节正确", d["data"] == pl, f"len={len(d['data'])}")

# ================= T3-07：边界五档 =================
print("=== T3-07 ===")
cases = [("t307a.bin", 0, 65536), ("t307b.bin", 65536, 65536),
         ("t307c.bin", 65536 + 100, 65536), ("t307d.bin", CHUNK, CHUNK),
         ("t307e.bin", CHUNK + 1, CHUNK)]
for n, (nm, sz, ck) in enumerate(cases):
    pl = (NONCE + f"T307{n}".encode() + b"G" * sz)[:sz]
    tid = 700 + n
    r = up_init(c, tid, DST, nm, sz)
    ok = ack_ok(r)
    for i in range(0, sz, ck):
        a = up_chunk(c, tid, i, pl[i:i + ck])
        ok = ok and ack_ok(a)
    osz, osha = obj_size(nm)
    exp_sha = hashlib.sha256(pl).hexdigest()
    check(f"T3-07{'abcde'[n]}", f"边界 {sz} 字节：转正且对象 sha/大小一致",
          ok and osz == sz and osha == exp_sha and SHA(pl) == exp_sha,
          f"ok={ok} obj=({osz},{osha[:8]})")

# ================= T3-08：3 路大分片并发 =================
print("=== T3-08 ===")
pls = {}
for k, n in enumerate("XYZ"):
    pls[k] = (NONCE + f"T308{n}".encode() + bytes([65 + k]) * (2 * 1024 * 1024))[:2 * 1024 * 1024]
ids = {k: 800 + k for k in pls}
tids = {}
for k in pls:
    nm = f"t308_{k}.bin"
    tids[k] = nm
    r = up_init(c, ids[k], DST, nm, len(pls[k]))
    if not ack_ok(r): check("T3-08-init", f"INIT {k} 失败", False, "")
ok = True
off = {k: 0 for k in pls}
while any(off[k] < len(pls[k]) for k in pls):
    for k in pls:
        if off[k] >= len(pls[k]): continue
        a = up_chunk(c, ids[k], off[k], pls[k][off[k]:off[k] + CHUNK])
        ok = ok and ack_ok(a)
        off[k] += CHUNK
dls = {}
for k in pls:
    dls[k] = download(c, 1100 + k, f"{DST}/{tids[k]}", tids[k])
cnts = [len(rows(tids[k])) for k in pls]
check("T3-08a", "3 路大分片并发各自逐字节正确、互不串扰",
      ok and all(dls[k]["data"] == pls[k] for k in pls),
      f"lens={[len(dls[k]['data']) for k in pls]}")
check("T3-08b", "3 路各自只有 1 行会话（无重复行/无串行）", cnts == [1, 1, 1], f"{cnts}")

# ================= T3-09：续传前缀不重传 =================
print("=== T3-09 ===")
pl = (NONCE + b"T309" + b"H" * (2 * 1024 * 1024))[:2 * 1024 * 1024]
name = "t309.bin"
up_init(c, 901, DST, name, len(pl))
up_chunk(c, 901, 0, pl[:CHUNK])
abort(c, 901)
ssz = staging_size(name)
r = up_init(c, 902, DST, name, len(pl))
off = init_off(r)
check("T3-09a", "107 后 29 回带 resumeOffset == 暂存物理大小",
      off == ssz == CHUNK, f"off={off} staging={ssz}")
pl2 = pl[:off] + b"!" * (len(pl) - off)          # 断点之后故意不同：证明前缀未被重传
for i in range(off, len(pl2), CHUNK):
    up_chunk(c, 902, i, pl2[i:i + CHUNK])
d = download(c, 1009, f"{DST}/{name}", name)
check("T3-09b", "首块首字节 == 源[offset]（前缀来自暂存、后缀为新写）", d["data"] == pl2,
      f"eq_pl2={d['data'] == pl2} eq_pl={d['data'] == pl}")

# ================= T3-10：107 × 在途写（AUD-4 锚点保持） =================
print("=== T3-10 ===")
pl = (NONCE + b"T310" + b"I" * (8 * 1024 * 1024))[:8 * 1024 * 1024]
name = "t310.bin"
ok108, okdb, okoff = True, True, True
for rd in range(6):
    tid = 1000 + rd
    r = up_init(c, tid, DST, name, len(pl))
    off = init_off(r)
    up_chunk(c, tid, off, pl[off:off + CHUNK], wait=False)   # 31 发出
    r108 = abort(c, tid)                                     # 不等 32 立即 107
    ok108 = ok108 and r108 is not None and r108[1][0] != 0
    time.sleep(0.4)                                          # 让在途写与延迟 abort 收尾
    ssz = staging_size(name)
    rw = row0(name)
    okdb = okdb and rw is not None and ssz == off + CHUNK and int(rw[2]) == ssz
    r = up_init(c, 1100 + rd, DST, name, len(pl))
    okoff = okoff and init_off(r) == ssz
    abort(c, 1100 + rd)
check("T3-10a", "6 轮 31→107：108 全 ret=true", ok108, "")
check("T3-10b", "每轮 confirmed_offset == 暂存物理大小（无越过/无落后）", okdb, "")
check("T3-10c", "每轮续传起点 == 暂存物理大小", okoff, "")

# ================= T3-11：通用上限未被抬高 =================
print("=== T3-11 ===")
c.send(pdu(T_FLUSH, msg=b"/" + b"z" * (17 * 1024 - 1)))
r = c.wait(T_FLUSH_R, 1.5)
alive = False
c.send(pdu(T_PING))
p = c.wait(T_PONG, 3)
alive = p is not None
check("T3-11a", "17 KB 非 31 报文仍走非法 PDU（无响应）且连接存活",
      r is None and alive, f"resp={r is not None} pong={alive}")

# ================= T3-12：旧客户端（4 KB + id=0）兼容 =================
print("=== T3-12 ===")
pl = (NONCE + b"T312" + b"J" * (256 * 1024))[:256 * 1024]
name = "t312.bin"
r = up_init(c, 0, DST, name, len(pl))
ok = ack_ok(r)
for i in range(0, len(pl), LEGACY_CHUNK):
    a = up_chunk(c, 0, i, pl[i:i + LEGACY_CHUNK])
    ok = ok and ack_ok(a)
d = download(c, 1012, f"{DST}/{name}", name)
check("T3-12a", "id=0 旧单流（4 KB 分片）全流程逐字节不变",
      ok and d["data"] == pl and ack_complete(a), f"ok={ok} len={len(d['data'])}")
rw = rows(name)
check("T3-12b", "旧路径同样单行贯穿（1 行、done）",
      len(rw) == 1 and rw[0][1] == "done", f"rows={len(rw)}")

# ================= 汇总 =================
print("\n=== 汇总 ===")
okn = sum(1 for _, _, cc in results if cc)
for sid, desc, cc in results:
    if not cc: print(f"  FAIL -> {sid} {desc}")
print(f"{okn}/{len(results)} passed")
print("NOTE: §6.3 NEEDS-HUMAN 五项（GUI 肉眼吞吐 / 面板进度回看 / 暂停继续单行 / 0 字节任务显示 / "
      "G7 断线秒级收敛与重连续传）不在本套件范围；客户端侧 C1–C9 的动态行为需客户端宿主，"
      "本套件以静态契约（S-02/S-04/S-05c）覆盖其接线与守卫。")
c.close()
_SRV.stop()
sys.exit(0 if okn == len(results) else 1)
