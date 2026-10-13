# -*- coding: utf-8 -*-
# REQ-EXT-SESSION-002 回归：服务端异常终止后，启动期在线态对账。
#
# 缺陷（修复前，已复现，见 build/audit-20261012/probe_abnormal.py 7/7）：
# online 的唯一权威来源是本进程的活连接——置 0 只在 OperateDB::handleOffLine（连接断开时按人调用）。
# 服务端被 kill / 崩溃 / 断电时，当时在线的人没有任何机会走 handleOffLine，库里永久留下
# online=1 的幽灵：在线列表与用户信息把它们当在线返回，"加好友"还会朝幽灵转发请求（无接收者、
# 静默丢弃）。只有该用户自己重新登录再断开才会自愈。也正因如此，init_test_data.sql 把
# tester/friend1 直接插成 online=1 —— 装好库就自带幽灵。
# 修复=在 main 里 a.exec() 之前、DB 可用之后做一次全量对账（启动瞬间本进程零连接，
# 任何 online=1 都必然是上个进程的残留）。
#
# 本套件对缺陷敏感：B4 在修复前二进制上必红（重启后 online 仍为 1）。
# 运行形态（见 E2E-RUNFORM.md）：cwd 任意；脚本自管服务端生命周期（server_ctl 按 PID，
# 端口被占则报错不盲杀；stop→start 用 taskkill /F，正是"崩溃"语义）。
# 运行：py -3.13 project-docs/evidence/REQ-EXT-SESSION-001/reconcile_online_e2e.py
import hashlib
import shutil
import socket
import struct
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
DIST = REPO / "dist"
EXE = DIST / "Server.exe"
FS = DIST / "filesys"
sys.path.insert(0, str(REPO / "project-docs" / "evidence" / "_harness"))
import server_ctl  # noqa: E402
import pymysql  # noqa: E402

HOST, PORT, HDR, PWD = "127.0.0.1", 5000, 76, "123456"
T_REG, T_REG_R, T_LOGIN, T_LOGIN_R = 1, 2, 3, 4
T_ONLINE, T_ONLINE_R = 7, 8
T_CAP, T_CAP_R = 44, 45

U1, U2 = "rc_u1", "rc_u2"
RESULTS = []


def check(label, cond, detail=""):
    RESULTS.append((label, bool(cond), detail))
    print(("[PASS] " if cond else "[FAIL] ") + label + (" :: " + str(detail) if detail != "" else ""))
    sys.stdout.flush()


def f32(s):
    return s.encode()[:31].ljust(32, b"\0")


def creds(name):
    return f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()


def pdu(t, msg=b"", dat=b""):
    return struct.pack("<III", HDR + len(msg), len(msg), t) + dat[:64].ljust(64, b"\0") + msg


def unpack(buf):
    out = []
    while len(buf) >= HDR:
        tl, ml, ty = struct.unpack("<III", buf[:12])
        if tl < HDR or tl != HDR + ml or len(buf) < tl:
            break
        out.append((ty, buf[12:76], buf[76:tl]))
        buf = buf[tl:]
    return out, buf


class Conn:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=10)
        self.buf = b""
        self.eof = False

    def pump(self, timeout=0.4):
        self.s.settimeout(timeout)
        try:
            d = self.s.recv(262144)
            if not d:
                self.eof = True
            else:
                self.buf += d
        except socket.timeout:
            pass
        except OSError:
            self.eof = True

    def packets(self):
        ps, self.buf = unpack(self.buf)
        return ps

    def ask(self, pkt, want, timeout=8):
        try:
            self.s.sendall(pkt)
        except OSError:
            self.eof = True
        got, end = [], time.time() + timeout
        while not [g for g in got if g[0] == want] and time.time() < end and not self.eof:
            self.pump(0.2)
            got.extend(self.packets())
        return got

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def db():
    cfg = (REPO / "Server" / "connect.config").read_text(encoding="utf-8").splitlines()
    return pymysql.connect(host=cfg[3], port=int(cfg[4]), user=cfg[6], password=cfg[7],
                           database=cfg[5], autocommit=True)


def q1(sql, args=()):
    with db() as c, c.cursor() as cur:
        cur.execute(sql, args)
        return cur.fetchone()


def exec_sql(sql, args=()):
    with db() as c, c.cursor() as cur:
        return cur.execute(sql, args)


def register(name):
    c = Conn()
    code = c.ask(pdu(T_CAP), T_CAP_R)[0][2].split(b"\0")[0]
    r = c.ask(pdu(T_REG, msg=code + b"\0", dat=creds(name)), T_REG_R)
    ok = struct.unpack("<?", r[0][1][:1])[0] if r else None
    c.close()
    return ok


def login(name):
    c = Conn()
    code = c.ask(pdu(T_CAP), T_CAP_R)[0][2].split(b"\0")[0]
    r = c.ask(pdu(T_LOGIN, msg=code + b"\0", dat=creds(name)), T_LOGIN_R)
    ret = struct.unpack("<i", r[0][1][:4])[0] if r else -99
    return c, ret


def online(name):
    r = q1("SELECT online FROM user_info WHERE name=%s", (name,))
    return r[0] if r else None


def online_list(c):
    r = c.ask(pdu(T_ONLINE), T_ONLINE_R)
    if not r:
        return []
    payload = r[0][2]
    return [payload[i:i + 32].split(b"\0")[0].decode() for i in range(0, len(payload) - 31, 32)]


# ---------------- 环境 ----------------
ctl = server_ctl.ServerCtl(str(EXE), str(DIST))
if not ctl.start():
    print("FATAL: " + str(ctl.last_error))
    sys.exit(3)

try:
    for u in (U1, U2):
        exec_sql("DELETE FROM login_record WHERE username=%s", (u,))
        exec_sql("DELETE FROM user_info WHERE name=%s", (u,))
        shutil.rmtree(FS / u, ignore_errors=True)
        register(u)

    check("A1 夹具就绪（两个账号已注册、无历史在线态）",
          online(U1) == 0 and online(U2) == 0, (online(U1), online(U2)))

    # ---------- A. 正常路径基线：对账不能把"在线/离线"功能一起砍掉 ----------
    print("=== A 正常路径基线 ===")
    c1, ret1 = login(U1)
    check("A2 登录成功且 online 置 1", ret1 == 1 and online(U1) == 1, "ret=%s online=%s" % (ret1, online(U1)))
    c1.close()
    time.sleep(1.0)
    check("A3 正常断开后 handleOffLine 把 online 归零", online(U1) == 0, online(U1))

    # ---------- B. 崩溃残留 → 重启对账 ----------
    print("=== B 崩溃残留与启动期对账 ===")
    exec_sql("UPDATE user_info SET online=1 WHERE name IN (%s,%s)", (U1, U2))
    check("B1 造出崩溃现场：两行 online 均为 1（模拟上个进程被 kill）",
          online(U1) == 1 and online(U2) == 1, (online(U1), online(U2)))

    ctl.stop()          # taskkill /F：等价于进程被杀，没有任何优雅退出路径
    check("B2 服务端进程已终止（端口不再监听）", not server_ctl.port_busy(HOST, PORT))
    check("B3 kill 后库里仍是 online=1（修复前的现场）", online(U1) == 1 and online(U2) == 1,
          (online(U1), online(U2)))

    if not ctl.start():
        print("FATAL: server did not come back: " + str(ctl.last_error))
        sys.exit(3)

    check("B4 【核心】重启后遗留在线态被对账归零",
          online(U1) == 0 and online(U2) == 0, (online(U1), online(U2)))

    # 在线列表（7/8）会排除"自己"，所以只用 U2 的连接观察 U1 的在/不在
    c2, ret2 = login(U2)
    names = online_list(c2)
    check("B5a 重启后在线列表不含幽灵 U1",
          ret2 == 1 and U1 not in names, names)

    ca, ra = login(U1)
    names2 = online_list(c2)
    check("B5b U1 真实登录后同一列表立刻含 U1（证明列表功能正常，不是恒空）",
          ra == 1 and U1 in names2, names2)
    ca.close()
    c2.close()
    time.sleep(1.0)
    check("B5c 两条连接关闭后均归零（为下一步的幂等性检查清场）",
          online(U1) == 0 and online(U2) == 0, (online(U1), online(U2)))

    # 幂等：再崩溃一次、再重启一次
    exec_sql("UPDATE user_info SET online=1 WHERE name IN (%s,%s)", (U1, U2))
    ctl.stop()
    if not ctl.start():
        print("FATAL: server did not come back (2nd): " + str(ctl.last_error))
        sys.exit(3)
    check("B6 再次崩溃→重启仍归零（对账可重复执行）",
          online(U1) == 0 and online(U2) == 0, (online(U1), online(U2)))

    # 不误伤真实会话
    cb, rb = login(U2)
    check("B7 重启后真实登录照常置 1（对账不误伤活连接）", rb == 1 and online(U2) == 1,
          (rb, online(U2)))
    cc, rc = login(U1)
    cc.close()          # 断开 U1，U2 保持在线的 cb 不动
    time.sleep(1.0)
    check("B8 其中一个断开只归零自己，另一在线者不受影响",
          rc == 1 and online(U1) == 0 and online(U2) == 1,
          (rc, online(U1), online(U2)))
    cb.close()
finally:
    ctl.stop()

good = sum(1 for _, o, _ in RESULTS if o)
print("RESULT %d/%d %s" % (good, len(RESULTS), "PASS" if good == len(RESULTS) else "FAIL"))
sys.exit(0 if good == len(RESULTS) else 1)
