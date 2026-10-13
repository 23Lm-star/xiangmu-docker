# -*- coding: utf-8 -*-
# REQ-EXT-SESSION-001 回归：注销账号 → 同一服务端进程内重新注册同名账号 → 登录被服务端误杀。
#
# 缺陷（修复前，已复现）：MyTcpSocket 的 LOGOFF 分支在注销成功时清空 m_strLoginName 且
# m_actorAuthenticated=false（连接保留，SEC-001 D2）。该连接日后断开走 clientOffLine 时
# authoritative=true（未认证跳过版本比对），但 removeLoginVersion 只在 m_actorAuthenticated 为真
# 时执行 → 该用户在 MyTcpServer::m_loginVersionRegistry 里的旧版本号永久残留。
# 而 handleLogin 的 login_version 计数器挂在 user_info 行上，注销删行、重新注册归零 →
# 下次登录分配到小版本号，被 setLoginVersion 的单调 CAS 丢弃 → 紧随其后的 closeStaleSockets
# 按 "m_loginVersion != cur" 把这条刚登录成功的连接自己 abort（客户端收不到 LOGIN_RESPOND），
# 且断线非权威 → handleOffLine 被跳过 → DB 留下 online=1 幽灵。修复=注销成功时显式释放登记表条目。
#
# 运行形态（见 E2E-RUNFORM.md）：cwd 任意；脚本自管服务端生命周期（server_ctl 按 PID，
# 端口被占则报错不盲杀）。exe=<repo>/dist/Server.exe，cwd=dist，filesPath=./filesys。
# 运行：py -3.13 project-docs/evidence/REQ-EXT-SESSION-001/relogoff_login_e2e.py
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
T_FINDU, T_FINDU_R = 5, 6
T_CAP, T_CAP_R = 44, 45
T_LOGOFF, T_LOGOFF_R = 48, 49

USER = "rluser"
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


def logoff(c, name):
    r = c.ask(pdu(T_LOGOFF, dat=creds(name)), T_LOGOFF_R)
    return struct.unpack("<?", r[0][1][:1])[0] if r else None


def lv(name):
    r = q1("SELECT login_version FROM user_info WHERE name=%s", (name,))
    return r[0] if r else None


def online(name):
    r = q1("SELECT online FROM user_info WHERE name=%s", (name,))
    return r[0] if r else None


def user_exists(name):
    return q1("SELECT name FROM user_info WHERE name=%s", (name,)) is not None


# ---------------- 环境 ----------------
ctl = server_ctl.ServerCtl(str(EXE), str(DIST))
if not ctl.start():
    print("FATAL: " + str(ctl.last_error))
    sys.exit(3)

try:
    exec_sql("DELETE FROM login_record WHERE username=%s", (USER,))
    exec_sql("DELETE FROM user_info WHERE name=%s", (USER,))
    shutil.rmtree(FS / USER, ignore_errors=True)

    check("R1 夹具就绪（同名账号与目录已清空）", not user_exists(USER) and not (FS / USER).exists())

    check("R2 注册账号成功", register(USER) is True)

    c1, ret1 = login(USER)
    check("R3 首次登录成功且 login_version=1", ret1 == 1 and lv(USER) == 1, "ret=%s lv=%s" % (ret1, lv(USER)))

    c2, ret2 = login(USER)
    check("R4 第二设备登录成功且 login_version=2（登记表版本推到 2）",
          ret2 == 1 and lv(USER) == 2, "ret=%s lv=%s" % (ret2, lv(USER)))

    ok = logoff(c2, USER)
    check("R5 在第二连接注销成功且账号行被删除",
          ok is True and not user_exists(USER), "ok=%s exists=%s" % (ok, user_exists(USER)))
    c1.close()
    c2.close()
    time.sleep(1.2)

    print("--- 同一个服务端进程内，重新注册同名账号 ---")
    check("R6 同一进程内重新注册同名账号成功", register(USER) is True)
    check("R7 重新注册后 login_version 计数器归零（=0）", lv(USER) == 0, "lv=%s" % lv(USER))

    c3, ret3 = login(USER)
    check("R8 【核心】重新注册后首次登录成功（修复前被服务端立即切断，ret=-99）",
          ret3 == 1, "ret=%s" % ret3)

    time.sleep(0.5)
    r = c3.ask(pdu(T_FINDU, dat=f32("nobody_xxx")), T_FINDU_R)
    served = (not c3.eof) and any(p[0] == T_FINDU_R for p in r)
    check("R9 该登录连接未被误杀：登录后能正常发起业务请求并拿到响应", served,
          "eof=%s pkts=%d" % (c3.eof, len(r)))
    check("R10 登录后库内在线态为 1", online(USER) == 1, "online=%s" % online(USER))

    c4, ret4 = login(USER)
    check("R11 第二次登录（新连接）成功且版本递增到 2（踢旧语义未被破坏）",
          ret4 == 1 and lv(USER) == 2, "ret=%s lv=%s" % (ret4, lv(USER)))
    time.sleep(0.6)
    r3 = c3.ask(pdu(T_FINDU, dat=f32("nobody_xxx")), T_FINDU_R)
    kicked = c3.eof or not any(p[0] == T_FINDU_R for p in r3)
    check("R12 原登录连接被正常踢下线（防『靠禁用踢旧来修复』）", kicked,
          "eof=%s pkts=%d" % (c3.eof, len(r3)))

    c3.close()
    c4.close()
    time.sleep(1.0)
    check("R13 全部连接关闭后库内 online=0（无 online=1 死连接幽灵）",
          online(USER) == 0, "online=%s" % online(USER))
finally:
    ctl.stop()

good = sum(1 for _, o, _ in RESULTS if o)
print("RESULT %d/%d %s" % (good, len(RESULTS), "PASS" if good == len(RESULTS) else "FAIL"))
sys.exit(0 if good == len(RESULTS) else 1)
