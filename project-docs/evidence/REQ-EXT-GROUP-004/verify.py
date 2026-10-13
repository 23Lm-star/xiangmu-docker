# REQ-EXT-GROUP-004 端到端验证 harness（development 节点交付）
#
# 覆盖两个面：
#   C* 客户端读游标状态机 —— 编译产品头文件 Client/groupchatstate.h 的断言探针
#      （socket 套件驱动不了客户端内存状态，所以抽成纯逻辑头 + 探针，见 cursor_probe.cpp）
#   S* 服务端已读契约 —— 钳制不拒绝 / 单调不回退 / 未读数服务端口径 / 成员门
#
# 用法：py -3.13 project-docs/evidence/REQ-EXT-GROUP-004/verify.py
# 服务端由本套件自管生命周期（_harness/server_ctl.py，只按自己启动的 PID 收尾）。
# 前置：MySQL 可达且已应用 Server/migrate_ext_group_002.sql（脚本会自检并断言）。
import hashlib
import json
import os
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
DIST = ROOT / 'dist'
EXE = DIST / 'Server.exe'
sys.path.insert(0, str(ROOT / 'project-docs' / 'evidence' / '_harness'))
import server_ctl            # noqa: E402
import pymysql               # noqa: E402

HOST, PORT = '127.0.0.1', 5000
HDR = 76
PWD = '123456'
GXX = 'C:/Qt/Tools/mingw730_64/bin/g++.exe'
QT_INC = 'C:/Qt/5.14.2/mingw73_64/include'
PROBE_SRC = HERE / 'cursor_probe.cpp'
PROBE_EXE = ROOT / 'build' / 'probe' / 'group004_cursor_probe.exe'

T_REG, T_REG_R, T_LOGIN, T_LOGIN_R, T_CAP, T_CAP_R = 1, 2, 3, 4, 44, 45
T_CREATE, T_CREATE_R = 60, 61
T_INVITE, T_INVITE_R = 62, 63
T_ACCEPT, T_ACCEPT_R = 64, 65
T_DISSOLVE, T_DISSOLVE_R = 76, 77
T_SEND, T_SEND_R = 88, 89
T_PULL, T_PULL_R = 91, 92
T_CONFIRM, T_CONFIRM_R = 93, 94
T_UNREAD, T_UNREAD_R = 95, 96

# 失败码（服务端 quint32 负数，客户端按无符号读）
N2101, N2102 = (1 << 32) - 2101, (1 << 32) - 2102

USERS = ['g4owner', 'g4m1', 'g4out']
GROUP_PREFIX = 'G4 %'

# 探针固定产出的用例编号：即使探针编译失败也要按固定条数计账，避免"断言总数漂移"
PROBE_IDS = [
    'C1_hole_buffers_no_advance',
    'C2_pull_fills_hole_and_advances',
    'C3_out_of_order_drains_on_connect',
    'C4_duplicate_and_late_ignored',
    'C5_backward_page_never_advances',
    'C6_out_of_range_jump_only_buffers',
    'C7_anchor_is_monotonic',
    'C8_anchor_drops_buffered_below_cursor',
    'C9_report_gate_and_ack_monotonic',
    'C10_clear_resets_session_state',
    'C11_water_moves_cursor_does_not',
    'C12_reach_water_level',
    'C13_seq_zero_ignored',
]

results = []


def check(sid, cond, detail=''):
    """结果一律记 (sid, ok, detail)。注意：conditions 与 detail 的位置不容错位——
    把详情串落到条件位会让断言恒真（伪造通过），本套件首轮就是这么骗过自己的。"""
    ok = bool(cond)
    results.append((sid, ok, detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {sid} :: {detail}")


# ---------------- 线协议 ----------------
def u16(x): return struct.pack('<H', x)


def u32(x): return struct.pack('<I', x)


def u64(x): return struct.pack('<Q', x)


def string(x):
    b = x.encode() if isinstance(x, str) else x
    return u32(len(b)) + b


def pdu(t, msg=b'', dat=b''):
    return struct.pack('<III', HDR + len(msg), len(msg), t) + dat[:64].ljust(64, b'\0') + msg


def envelope(gid=0, body=b''):
    return (u16(1) + u16(20) + u64(int(time.time_ns() & ((1 << 63) - 1))) + u64(gid) + body)


def f32(s):
    return s.encode()[:31].ljust(32, b'\0')


def unpack_packets(buf):
    out = []
    while len(buf) >= HDR:
        tl, ml, ty = struct.unpack('<III', buf[:12])
        if tl < HDR or tl != HDR + ml or len(buf) < tl:
            break
        out.append((ty, buf[12:76], buf[76:tl]))
        buf = buf[tl:]
    return out, buf


class Rd:
    def __init__(self, data):
        self.d, self.o = data, 0

    def u8(self):
        v = self.d[self.o]; self.o += 1; return v

    def u32(self):
        v = struct.unpack_from('<I', self.d, self.o)[0]; self.o += 4; return v

    def u64(self):
        v = struct.unpack_from('<Q', self.d, self.o)[0]; self.o += 8; return v

    def string(self):
        n = self.u32(); s = self.d[self.o:self.o + n]; self.o += n; return s


class Conn:
    def __init__(self, name=None):
        self.s = socket.create_connection((HOST, PORT), timeout=10)
        self.buf = b''
        self.name = name

    def pump(self, timeout=.5):
        self.s.settimeout(timeout)
        try:
            d = self.s.recv(262144)
            if not d:
                raise ConnectionError('peer closed')
            self.buf += d
        except socket.timeout:
            pass

    def packets(self):
        ps, self.buf = unpack_packets(self.buf)
        return ps

    def ask(self, pkt, expected=None, timeout=5):
        self.s.sendall(pkt)
        end, got = time.time() + timeout, []
        while time.time() < end:
            try:
                self.pump(.3)
            except ConnectionError:
                break
            got.extend(self.packets())
            if expected is not None and any(x[0] == expected for x in got):
                return got
        return got

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass


def cap_login(name):
    c = Conn(name)
    r = c.ask(pdu(T_CAP), T_CAP_R)
    code = r[0][2].split(b'\0')[0] if r else b''
    r = c.ask(pdu(T_LOGIN, msg=code + b'\0',
                  dat=f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()), T_LOGIN_R)
    ret = struct.unpack('<i', r[0][1][:4])[0] if r else -99
    if ret != 1:
        c.close()
        raise RuntimeError(f'login {name} ret={ret}')
    return c


def ensure_user(name):
    c = Conn(name)
    r = c.ask(pdu(T_CAP), T_CAP_R)
    code = r[0][2].split(b'\0')[0] if r else b''
    c.ask(pdu(T_REG, msg=code + b'\0',
              dat=f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()), T_REG_R)
    c.close()
    cap_login(name).close()


def greq(c, typ, gid=0, body=b'', timeout=5):
    """群请求：返回 (code, 信封之后的业务体)。响应布局 = 信封(20) + code(4) + 业务体。"""
    ps = c.ask(pdu(typ, msg=envelope(gid, body)), typ + 1, timeout)
    for x in ps:
        if x[0] == typ + 1:
            d = x[2]
            code = struct.unpack('<I', d[20:24])[0] if len(d) >= 24 else None
            return code, d[24:]
    return None, b''


def send_msg(c, gid, key, content):
    code, body = greq(c, T_SEND, gid, string(key) + string(content))
    r = Rd(body)
    out = {'code': code, 'seq': 0}
    if code == 0:
        r.string(); out['seq'] = r.u64()
    return out


def pull(c, gid, mode, cursor, limit):
    code, body = greq(c, T_PULL, gid, bytes([mode]) + u64(cursor) + u16(limit))
    out = {'code': code, 'records': [], 'unread': None, 'last_seq': None}
    if code != 0 or not body:
        return out
    r = Rd(body)
    r.u8(); out['has_more'] = r.u8(); out['next_cursor'] = r.u64()
    out['last_seq'] = r.u64(); out['unread'] = r.u64(); n = r.u32()
    for _ in range(n):
        out['records'].append({'seq': r.u64(), 'msg_id': r.u64(), 'sender': r.string(),
                               'type': r.u8(), 'created': r.u64(), 'content': r.string()})
    return out


def confirm(c, gid, seq):
    code, body = greq(c, T_CONFIRM, gid, u64(seq))
    eff = struct.unpack('<Q', body[:8])[0] if code == 0 and len(body) >= 8 else None
    return {'code': code, 'effective': eff}


def unread_summary(c):
    code, body = greq(c, T_UNREAD, 0, b'')
    rows = {}
    if code != 0 or not body:
        return rows
    r = Rd(body); n = r.u32()
    for _ in range(n):
        gid = r.u64()
        rows[gid] = {'last_seq': r.u64(), 'last_read_seq': r.u64(), 'unread': r.u64()}
    return rows


def row_for(c, gid, tries=3):
    for _ in range(tries):
        rows = unread_summary(c)
        if gid in rows:
            return rows[gid]
        time.sleep(.3)
    return None


# ---------------- 数据库 ----------------
def db_conn():
    lines = (ROOT / 'Server' / 'connect.config').read_text(encoding='utf-8').splitlines()
    return pymysql.connect(host=lines[3], port=int(lines[4]), user=lines[6],
                           password=lines[7], database=lines[5], autocommit=True)


def db_one(sql, args=()):
    with db_conn() as c:
        with c.cursor() as cur:
            cur.execute(sql, args)
            return cur.fetchone()


# ---------------- 探针 ----------------
def run_probe():
    """编译产品头文件并跑断言；返回 {id: (ok, detail)}。编译失败 → 空表（由调用方按固定编号计账）。"""
    PROBE_EXE.parent.mkdir(parents=True, exist_ok=True)
    cmd = [GXX, '-std=gnu++11', '-Wall', '-Wextra',
           '-I', str(ROOT), '-I', str(ROOT / 'Client'),
           '-I', QT_INC, '-I', QT_INC + '/QtCore',
           str(PROBE_SRC), '-o', str(PROBE_EXE)]
    built = subprocess.run(cmd, capture_output=True, text=True)
    if built.returncode != 0:
        print('[E] probe build failed:\n' + (built.stderr or '')[-2000:])
        return {}
    ran = subprocess.run([str(PROBE_EXE)], capture_output=True, text=True)
    out = {}
    for line in (ran.stdout or '').splitlines():
        for tag in ('[PASS] ', '[FAIL] '):
            if line.startswith(tag):
                rest = line[len(tag):]
                sid, _, detail = rest.partition(' :: ')
                out[sid.strip()] = (tag == '[PASS] ', detail.strip())
    return out


# ---------------- 主流程 ----------------
srv = server_ctl.ServerCtl(str(EXE), str(DIST), HOST, PORT)
commit = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT,
                        capture_output=True, text=True).stdout.strip()

print('=== 环境准备 ===')
if not srv.start():
    print(f'FATAL: Server 启动失败：{srv.last_error}')
    sys.exit(2)

try:
    # 迁移前置：本套件的服务端断言全部依赖 GROUP-002 的 last_seq/group_message
    cols = db_one("SELECT COUNT(*) FROM information_schema.columns WHERE table_schema=DATABASE() "
                  "AND table_name='group_info' AND column_name='last_seq'")[0]
    tbl = db_one("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
                 "AND table_name='group_message'")[0]
    check('S0_migration_applied', cols == 1 and tbl == 1, f'last_seq={cols} group_message={tbl}')

    # 可重放：清掉本 harness 的数据域
    with db_conn() as c:
        with c.cursor() as cur:
            cur.execute("DELETE FROM group_message WHERE group_id IN "
                        "(SELECT group_id FROM group_info WHERE group_name LIKE %s)", (GROUP_PREFIX + '%',))
            cur.execute("DELETE FROM group_invitation WHERE group_id IN "
                        "(SELECT group_id FROM group_info WHERE group_name LIKE %s)", (GROUP_PREFIX + '%',))
            cur.execute("DELETE FROM group_member WHERE group_id IN "
                        "(SELECT group_id FROM group_info WHERE group_name LIKE %s)", (GROUP_PREFIX + '%',))
            cur.execute("DELETE FROM group_info WHERE group_name LIKE %s", (GROUP_PREFIX + '%',))
            cur.execute("DELETE FROM message_info WHERE seed_name LIKE 'g4%' OR rev_name LIKE 'g4%'")

    for n in USERS:
        ensure_user(n)
    owner, m1, outsider = cap_login('g4owner'), cap_login('g4m1'), cap_login('g4out')
    print('[SETUP] users ready; commit ' + commit)

    def make_group(creator, name, invitees):
        code, body = greq(creator, T_CREATE, 0, string(name))
        gid = struct.unpack('<Q', body[:8])[0] if code == 0 and len(body) >= 8 else 0
        if gid == 0:
            raise RuntimeError(f'create {name} code={code}')
        for who, conn in invitees:
            code, body = greq(creator, T_INVITE, gid, string(who) + string(f'k-{name}-{who}'))
            iid = struct.unpack('<Q', body[:8])[0] if code == 0 and len(body) >= 8 else 0
            a, _ = greq(conn, T_ACCEPT, 0, u64(iid))
            if a != 0:
                raise RuntimeError(f'accept {who} in {name} code={a}')
        return gid

    gid = make_group(owner, 'G4 main', [('g4m1', m1)])
    base = row_for(owner, gid)
    if base is None:
        raise RuntimeError('unread summary missing group row')

    # ---- S1 超上界：钳制而不是拒绝（客户端与进度之间本来有正常时钟差）----
    for i in range(5):
        r = send_msg(owner, gid, f's1-{i}', f'msg-{i}')
        if r['code'] != 0:
            print(f"[NOTE] send s1-{i} failed code={r['code']}")
    after = row_for(owner, gid)
    water = after['last_seq']
    check('S1a_water_advanced_by_sends', water == base['last_seq'] + 5,
          f"base={base['last_seq']} now={water} (期望 +5)")

    c = confirm(owner, gid, water + 1000)
    check('S1b_overflow_clamped_not_rejected', c['code'] == 0 and c['effective'] == water,
          f"code={c['code']} effective={c['effective']} water={water}")

    s = row_for(owner, gid)
    check('S1c_full_confirm_zeroes_unread',
          s['last_read_seq'] == water and s['unread'] == 0,
          f"last_read={s['last_read_seq']} unread={s['unread']}")

    # ---- S2 迟到的旧确认：夹回边界，不回退 ----
    for i in range(3):
        send_msg(owner, gid, f's2-{i}', f'later-{i}')
    s2 = row_for(owner, gid)
    confirm(owner, gid, s2['last_seq'])
    late = confirm(owner, gid, base['last_seq'])   # 远小于当前游标
    s3 = row_for(owner, gid)
    check('S2_late_confirm_no_rollback',
          late['code'] == 0 and late['effective'] == s2['last_seq']
          and s3['last_read_seq'] == s2['last_seq'],
          f"late_effective={late['effective']} cursor={s3['last_read_seq']} 期望={s2['last_seq']}")

    # ---- S3 单调：任意乱序的上报序列，服务端游标只增不减 ----
    effs = [confirm(owner, gid, v)['effective'] for v in (10, 5, 20, 3, 999999)]
    check('S3_monotonic_non_decreasing',
          all(b >= a for a, b in zip(effs, effs[1:])) and effs[-1] == row_for(owner, gid)['last_seq'],
          f'effs={effs}')

    # ---- S4 未读口径：unread == last_seq - last_read_seq（客户端本地推导同式）。
    # 必须用一个"游标还没被推到水位"的成员（游标单调，用更小的值确认不生效）----
    gu = make_group(owner, 'G4 unread', [('g4m1', m1)])
    for i in range(5):
        send_msg(owner, gu, f's4-{i}', f'u-{i}')
    water = row_for(m1, gu)['last_seq']
    k = water - 3
    confirm(m1, gu, k)
    s4 = row_for(m1, gu)
    check('S4a_unread_identity_on_summary',
          s4['last_read_seq'] == k and s4['unread'] == water - k
          and s4['last_seq'] - s4['last_read_seq'] == s4['unread'],
          f"last_read={s4['last_read_seq']} unread={s4['unread']} 期望 {k}/{water - k}")

    p = pull(m1, gu, 0, k, 100)
    check('S4b_unread_identity_on_pull',
          p['code'] == 0 and p['unread'] == water - k and len(p['records']) == 3,
          f"unread={p['unread']} records={len(p['records'])} 期望 {water - k}/3")

    # ---- S5 成员门：非成员与已解散群不得推进任何人的游标 ----
    non = confirm(outsider, gid, 1)
    check('S5a_non_member_confirm_rejected', non['code'] == N2101 and non['effective'] is None,
          f"code={non['code']} 期望={N2101}")

    gone = make_group(owner, 'G4 gone', [('g4m1', m1)])
    send_msg(owner, gone, 's5-0', 'to-be-dissolved')
    d, _ = greq(owner, T_DISSOLVE, gone, b'')
    dis = confirm(m1, gone, 1)
    check('S5b_dissolved_group_confirm_rejected',
          d == 0 and dis['code'] == N2102,
          f"dissolve={d} confirm_code={dis['code']} 期望={N2102}")

    # ---- C* 客户端游标状态机（编译产品头文件的探针）----
    probe = run_probe()
    if not probe:
        print('[NOTE] 探针未产出结果（编译失败或工具链缺失）——C* 全部记为 FAIL')
    for pid in PROBE_IDS:
        ok, detail = probe.get(pid, (False, 'probe produced no verdict'))
        check(pid, ok, detail)
finally:
    for c in (locals().get('owner'), locals().get('m1'), locals().get('outsider')):
        if c is not None:
            c.close()
    srv.stop()

# ---------------- 汇总 ----------------
passed = sum(1 for _, ok, _ in results if ok)
print(f'\nRESULT {passed}/{len(results)} PASS  commit={commit}  {time.strftime("%Y-%m-%d %H:%M:%S")}')
outdir = ROOT / 'build' / 'group004'
outdir.mkdir(parents=True, exist_ok=True)
(outdir / 'verify-output.log').write_text(
    '\n'.join(f"[{'PASS' if ok else 'FAIL'}] {sid} :: {detail}" for sid, ok, detail in results)
    + f'\nRESULT {passed}/{len(results)} PASS commit={commit}\n', encoding='utf-8')
(outdir / 'evidence.json').write_text(json.dumps(
    {'commit': commit, 'timestamp': time.strftime('%Y-%m-%d %H:%M:%S'),
     'passed': passed, 'total': len(results),
     'results': [{'id': s, 'ok': o, 'detail': t} for s, o, t in results]},
    ensure_ascii=False, indent=2), encoding='utf-8')
sys.exit(0 if passed == len(results) else 1)
