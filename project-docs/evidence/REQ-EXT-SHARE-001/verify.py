# -*- coding: utf-8 -*-
# REQ-EXT-SHARE-001 外链分享生命周期（重新生成 / 续期）端到端套件。
#
# 覆盖面：
#   静态 S1-S2  两端 protocol.h 的 REQ-EXT-SHARE-001 常量块逐字节一致；块内含状态字与动作字面量
#   动态 A1..A4 旧语义（52 不带动作区）与查询（56）在 有效/过期/撤销 三态下的返回值
#   动态 B1..B6 续期：生效、token 不变、链接仍可解析、对已撤销行拒绝
#   动态 C1..C5 重新生成：换新 token、旧链接立即失效、新链接可解析、被撤销后仍可自救
#   动态 D1..D2 向后兼容面：响应 caData[0]（ok/exists）语义未变；不带动作区的请求照旧
#
# 运行形态（见 project-docs/evidence/E2E-RUNFORM.md）：cwd 任意；脚本自管服务端生命周期
# （server_ctl 按 PID；端口被占则报错不盲杀）。Server.exe 取 <repo>/dist/Server.exe，
# 其 cwd=dist、filesPath=./filesys（构建期内嵌的 Server/connect.config）。
# 运行：py -3.13 project-docs/evidence/REQ-EXT-SHARE-001/verify.py
import hashlib
import re
import shutil
import socket
import struct
import sys
import time
from datetime import datetime, timedelta
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
DIST = REPO / "dist"
EXE = DIST / "Server.exe"
FS = DIST / "filesys"
sys.path.insert(0, str(REPO / "project-docs" / "evidence" / "_harness"))
import server_ctl  # noqa: E402
import pymysql  # noqa: E402

HOST, PORT, HDR, PWD = "127.0.0.1", 5000, 76, "123456"
T_CAP, T_CAP_R, T_REG, T_REG_R, T_LOGIN, T_LOGIN_R = 44, 45, 1, 2, 3, 4
T_CPL, T_CPL_R, T_RES, T_RES_R, T_QUERY, T_QUERY_R = 52, 53, 54, 55, 56, 57

OWNER, OTHER = "sh1user", "sh1other"
REL = "sh1.bin"
PUBLINK_STATE_OFFSET = 8
ST_VALID, ST_EXPIRED, ST_REVOKED = 0, 1, 2
ACTION_NEW, ACTION_RENEW = "new", "renew"

RESULTS = []


def check(label, cond, detail=""):
    RESULTS.append((label, bool(cond), detail))
    print(("[PASS] " if cond else "[FAIL] ") + label + (" :: " + str(detail) if detail != "" else ""))
    sys.stdout.flush()


def f32(s):
    return s.encode()[:31].ljust(32, b"\0")


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

    def pump(self, timeout=0.4):
        self.s.settimeout(timeout)
        try:
            d = self.s.recv(262144)
            if not d:
                raise ConnectionError("peer closed")
            self.buf += d
        except socket.timeout:
            pass

    def packets(self):
        ps, self.buf = unpack(self.buf)
        return ps

    def send(self, pkt):
        self.s.sendall(pkt)

    def collect(self, want, count, timeout=20):
        got, end = [], time.time() + timeout
        while len([g for g in got if g[0] == want]) < count and time.time() < end:
            try:
                self.pump(0.2)
            except ConnectionError:
                break
            got.extend(self.packets())
        return got

    def ask(self, pkt, want, timeout=8):
        self.send(pkt)
        return self.collect(want, 1, timeout)

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
    c.ask(pdu(T_REG, msg=code + b"\0", dat=f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()),
          T_REG_R)
    c.close()


def login(name):
    c = Conn()
    code = c.ask(pdu(T_CAP), T_CAP_R)[0][2].split(b"\0")[0]
    r = c.ask(pdu(T_LOGIN, msg=code + b"\0",
                  dat=f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()), T_LOGIN_R)
    ret = struct.unpack("<i", r[0][1][:4])[0] if r else -99
    if ret != 1:
        c.close()
        return None
    return c


def cpl(c, rel, action=None, seconds=0):
    """发 52；action 非空时在路径 NUL 之后附 <action>\\0<seconds>（旧客户端不附）"""
    rb = rel.encode()
    tail = b""
    if action:
        tail = action.encode() + b"\0" + str(seconds).encode()
    pkt = pdu(T_CPL, msg=rb + b"\0" + tail)
    r = c.ask(pkt, T_CPL_R)
    if not r:
        return None, None, ""
    ok = struct.unpack("<?", r[0][1][:1])[0]
    state = struct.unpack("<I", r[0][1][PUBLINK_STATE_OFFSET:PUBLINK_STATE_OFFSET + 4])[0]
    return ok, state, r[0][2].split(b"\0")[0].decode()


def query(c, rel):
    r = c.ask(pdu(T_QUERY, dat=f32(OWNER), msg=rel.encode()), T_QUERY_R)
    if not r:
        return None, None, ""
    exists = struct.unpack("<?", r[0][1][:1])[0]
    state = struct.unpack("<I", r[0][1][PUBLINK_STATE_OFFSET:PUBLINK_STATE_OFFSET + 4])[0]
    return exists, state, r[0][2].split(b"\0")[0].decode()


def resolve(c, token):
    r = c.ask(pdu(T_RES, msg=token.encode()), T_RES_R)
    if not r:
        return None, ""
    ok = struct.unpack("<?", r[0][1][:1])[0]
    return ok, r[0][2].split(b"\0")[0].decode()


def token_of(link):
    return link.split("cloud-disk://share/")[-1]


def hash_of(link):
    return token_of(link) if len(token_of(link)) == 64 else hashlib.sha256(
        token_of(link).encode()).hexdigest()


def landed(json_txt):
    """55 响应 caMsg 是 {"saved_path":"分享链接/xxx"}（服务端复制到请求者目录后的相对路径）"""
    m = re.search(r'"saved_path":"([^"]*)"', json_txt or "")
    return FS / OWNER / m.group(1) if m else None


def row():
    return q1("SELECT token_hash, expire_time, revoked FROM share_link "
              "WHERE owner_name=%s AND relative_path=%s ORDER BY created_at DESC, id DESC LIMIT 1",
              (OWNER, REL))


# ---------------- 静态 ----------------
print("=== 静态：冻结面常量块 ===")


def block(path):
    txt = open(path, encoding="utf-8").read()
    m = re.search(r"// ===== REQ-EXT-SHARE-001.*?#endif // PROTOCOL_H", txt, re.S)
    return m.group(0) if m else None


bc = block(REPO / "Client" / "protocol.h")
bs = block(REPO / "Server" / "protocol.h")
check("S1-same-block :: 两端 protocol.h 的 REQ-EXT-SHARE-001 常量块逐字节一致",
      bc is not None and bc == bs, "len=%d" % len(bc or ""))
check("S2-consts :: 块内含状态字偏移/三态常量/两个动作字面量/续期缺省",
      all(k in (bc or "") for k in ("PUBLINK_STATE_OFFSET = 8", "PUBLINK_STATE_EXPIRED",
                                    "PUBLINK_STATE_REVOKED", '"new"', '"renew"',
                                    "PUBLINK_RENEW_DEFAULT_SECONDS")))

# ---------------- 环境 ----------------
ctl = server_ctl.ServerCtl(str(EXE), str(DIST))
if not ctl.start():
    print("FATAL: " + str(ctl.last_error))
    sys.exit(3)

try:
    for u in (OWNER, OTHER):
        exec_sql("DELETE FROM share_link WHERE owner_name=%s", (u,))
        exec_sql("DELETE FROM user_file WHERE owner_name=%s", (u,))
        exec_sql("DELETE FROM user_info WHERE name=%s", (u,))
        shutil.rmtree(FS / u, ignore_errors=True)
        register(u)
    (FS / OWNER).mkdir(parents=True, exist_ok=True)
    (FS / OWNER / REL).write_text("SHARE-001-CONTENT")

    c = login(OWNER)
    check("A0 夹具就绪（登录成功、无历史链接）", c is not None and row() is None)

    # ---------------- 旧语义 + 状态字 ----------------
    print("=== 旧语义（52 不带动作区）与查询三态 ===")
    e0, s0, _ = query(c, REL)
    check("A1 无记录时查询 exists=false", e0 is False, "exists=%s" % e0)

    ok, st, link1 = cpl(c, REL)
    tok1 = token_of(link1)
    check("A2 首次创建 ok=true/state=VALID/返回 22 位原始 token",
          ok is True and st == ST_VALID and len(tok1) == 22 and not re.fullmatch(r"[0-9a-f]{64}", tok1),
          "ok=%s state=%s token=%s" % (ok, st, tok1))
    check("A3 落库一行且 token_hash=sha256(token)",
          row() is not None and row()[0] == hashlib.sha256(tok1.encode()).hexdigest())

    e1, s1, link1q = query(c, REL)
    check("A4 查询 exists=true/state=VALID/返回 hash 形式链接",
          e1 is True and s1 == ST_VALID and token_of(link1q) == row()[0], "state=%s" % s1)

    ok, saved = resolve(c, tok1)
    f = landed(saved)
    check("A5 有效链接可解析落地", ok is True and f is not None and f.is_file(), "ok=%s %s" % (ok, saved))

    exec_sql("UPDATE share_link SET expire_time=%s WHERE owner_name=%s AND relative_path=%s",
             ((datetime.now() - timedelta(hours=1)).strftime("%Y-%m-%d %H:%M:%S"), OWNER, REL))
    e2, s2, _ = query(c, REL)
    check("A6 过期后查询 state=EXPIRED(1)", e2 is True and s2 == ST_EXPIRED, "state=%s" % s2)

    ok, st, _ = cpl(c, REL)
    check("A7 过期时旧语义创建不再谎报有效：ok=true 但 state=EXPIRED",
          ok is True and st == ST_EXPIRED, "ok=%s state=%s" % (ok, st))
    ok, _ = resolve(c, tok1)
    check("A8 过期链接解析被拒", ok is False, "ok=%s" % ok)

    # ---------------- 续期 ----------------
    print("=== 续期（action=renew） ===")
    ok, st, linkR = cpl(c, REL, ACTION_RENEW, 7 * 24 * 3600)
    exp = row()[1]
    want = datetime.now() + timedelta(days=7)
    check("B1 续期成功 ok=true/state=VALID", ok is True and st == ST_VALID, "ok=%s state=%s" % (ok, st))
    check("B2 expire_time 前推到 ~7 天后",
          abs((exp - want).total_seconds()) < 180, "expire=%s" % exp)
    check("B3 续期不改 token（链接文本仍是原 hash）", hash_of(linkR) == row()[0], "hash=%s" % hash_of(linkR)[:16])
    ok, _ = resolve(c, tok1)
    check("B4 续期后原 token 重新可用", ok is True, "ok=%s" % ok)

    exec_sql("UPDATE share_link SET revoked=1 WHERE owner_name=%s AND relative_path=%s", (OWNER, REL))
    e3, s3, _ = query(c, REL)
    ok, st, _ = cpl(c, REL, ACTION_RENEW, 7 * 24 * 3600)
    check("B5 撤销后查询 state=REVOKED(2)", e3 is True and s3 == ST_REVOKED, "state=%s" % s3)
    check("B6 已撤销链接续期被拒（续期不让作废链接复活）",
          ok is False and st == ST_REVOKED, "ok=%s state=%s" % (ok, st))

    # ---------------- 重新生成 ----------------
    print("=== 重新生成（action=new） ===")
    old_hash = row()[0]
    ok, st, linkN = cpl(c, REL, ACTION_NEW)
    new_tok = token_of(linkN)
    check("C1 重新生成 ok=true/state=VALID/返回新 22 位 token",
          ok is True and st == ST_VALID and len(new_tok) == 22, "ok=%s token=%s" % (ok, new_tok))
    check("C2 新链接落库且与旧 hash 不同",
          row() is not None and row()[0] == hashlib.sha256(new_tok.encode()).hexdigest()
          and row()[0] != old_hash)
    check("C3 撤销位已被新行清掉（revoked=0）", row()[2] == 0, "revoked=%s" % row()[2])
    ok, _ = resolve(c, old_hash)
    check("C4 旧链接立即失效（解析被拒）", ok is False, "ok=%s" % ok)
    ok, saved = resolve(c, new_tok)
    f = landed(saved)
    check("C5 新链接可解析落地", ok is True and f is not None and f.is_file(), "ok=%s %s" % (ok, saved))

    e4, s4, link4 = query(c, REL)
    check("C6 重新生成后查询回到 VALID 且指向新 hash",
          e4 is True and s4 == ST_VALID and token_of(link4) == row()[0], "state=%s" % s4)

    # ---------------- 向后兼容面 ----------------
    print("=== 向后兼容 ===")
    exec_sql("DELETE FROM share_link WHERE owner_name=%s AND relative_path=%s", (OWNER, REL))
    ok, st, link0 = cpl(c, REL)   # 无动作区的旧请求，无记录 → 新建
    check("D1 无动作区的旧请求在零记录时仍走新建路径", ok is True and st == ST_VALID and len(token_of(link0)) == 22)
    ok, st, link2 = cpl(c, REL)
    check("D2 无动作区 + 已有有效记录 → 复用（ok=true/state=VALID，token 不变）",
          ok is True and st == ST_VALID and hash_of(link2) == row()[0], "state=%s" % st)

    c.close()
finally:
    ctl.stop()

good = sum(1 for _, o, _ in RESULTS if o)
print("RESULT %d/%d %s" % (good, len(RESULTS), "PASS" if good == len(RESULTS) else "FAIL"))
sys.exit(0 if good == len(RESULTS) else 1)
