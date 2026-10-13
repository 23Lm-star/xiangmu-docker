# REQ-EXT-GROUP-002 T7 端到端验证 harness（development 节点交付）
# 前置：dist/Server.exe 运行中（127.0.0.1:5000），DB 已应用 Server/migrate_ext_group_002.sql
# 用法：py313 verify.py [--server-log <path>] [--commit <hash>]
# 说明：服务端为单线程事件循环，所有 DB 操作在主线程串行执行；"并发"探针以
#       多连接流水线对打实现（传输层交错），FOR UPDATE/gap 锁/唯一索引为正确性兜底。
import hashlib, json, os, re, socket, struct, subprocess, sys, threading, time
from pathlib import Path
import pymysql

HOST, PORT = '127.0.0.1', 5000
HDR = 76
PWD = '123456'

T_REG, T_REG_R, T_LOGIN, T_LOGIN_R, T_CAP, T_CAP_R = 1, 2, 3, 4, 44, 45
T_CHAT = 15
T_CPL, T_CPL_R, T_RES, T_RES_R = 52, 53, 54, 55
# GROUP-001
T_CREATE, T_CREATE_R = 60, 61
T_INVITE, T_INVITE_R = 62, 63
T_ACCEPT, T_ACCEPT_R = 64, 65
T_LEAVE, T_LEAVE_R = 68, 69
T_REMOVE, T_REMOVE_R = 70, 71
T_DISSOLVE, T_DISSOLVE_R = 76, 77
T_LIST, T_LIST_R = 78, 79
T_MEMBERS, T_MEMBERS_R = 80, 81
# GROUP-002
T_SEND, T_SEND_R, T_EVENT = 88, 89, 90
T_PULL, T_PULL_R = 91, 92
T_CONFIRM, T_CONFIRM_R = 93, 94
T_UNREAD, T_UNREAD_R = 95, 96

ROOT = Path(__file__).resolve().parents[3]
SERVER_LOG = ROOT / 'build' / 'group002' / 'server-console.log'
if '--server-log' in sys.argv:
    SERVER_LOG = Path(sys.argv[sys.argv.index('--server-log') + 1])
# FINDING-GATE-2 收口：本套件原先要求"外部已有 dist/Server.exe 在跑"，在 enforced 门禁下
# 前面的套件会清掉自己启动的进程，于是这里必然 ConnectionRefused（WinError 10061）。
# 改为自管生命周期（按 PID，端口被别人占用时直接报错、不盲杀），与其余套件同形。
DIST = ROOT / 'dist'
EXE = DIST / 'Server.exe'
sys.path.insert(0, str(ROOT / 'project-docs' / 'evidence' / '_harness'))
import server_ctl
_SRV = server_ctl.ServerCtl(str(EXE), str(DIST), HOST, PORT, log_path=str(SERVER_LOG))


def start_server():
    if not _SRV.start():
        print('[E] ' + str(_SRV.last_error))
        return False
    return True


def stop_server():
    _SRV.stop()


# 失败码（服务端为 quint32 负数，客户端按无符号读取）
N1001, N2001, N2002 = (1 << 32) - 1001, (1 << 32) - 2001, (1 << 32) - 2002
N2101, N2102, N2103, N2104 = (1 << 32) - 2101, (1 << 32) - 2102, (1 << 32) - 2103, (1 << 32) - 2104
results = []
notes = []


def note(line):
    notes.append(line)
    print('[NOTE] ' + line)


def check(label, cond, detail=''):
    print(('[PASS] ' if cond else '[FAIL] ') + label + (' :: ' + detail if detail else ''))
    results.append((label, bool(cond), detail))


def f32(s):
    return s.encode()[:31].ljust(32, b'\0')


def pdu(t, msg=b'', dat=b''):
    return struct.pack('<III', HDR + len(msg), len(msg), t) + dat[:64].ljust(64, b'\0') + msg


def u16(x): return struct.pack('<H', x)
def u32(x): return struct.pack('<I', x)
def u64(x): return struct.pack('<Q', x)
def i32(x): return struct.pack('<i', x)


def string(x):
    b = x.encode() if isinstance(x, str) else x
    return u32(len(b)) + b


def envelope(gid=0, body=b'', req=None):
    return u16(1) + u16(20) + u64(req or int(time.time_ns() & ((1 << 63) - 1))) + u64(gid) + body


def unpack_packets(buf):
    out = []
    while len(buf) >= HDR:
        tl, ml, ty = struct.unpack('<III', buf[:12])
        if tl < HDR or tl != HDR + ml or len(buf) < tl: break
        out.append((ty, buf[12:76], buf[76:tl])); buf = buf[tl:]
    return out, buf


class Rd:
    def __init__(self, data):
        self.d = data
        self.o = 0

    def u8(self):
        v = self.d[self.o]; self.o += 1; return v

    def u16(self):
        v = struct.unpack_from('<H', self.d, self.o)[0]; self.o += 2; return v

    def u32(self):
        v = struct.unpack_from('<I', self.d, self.o)[0]; self.o += 4; return v

    def u64(self):
        v = struct.unpack_from('<Q', self.d, self.o)[0]; self.o += 8; return v

    def string(self):
        n = self.u32(); s = self.d[self.o:self.o + n]; self.o += n; return s

    def done(self):
        return self.o == len(self.d)


class Conn:
    def __init__(self, name=None):
        self.s = socket.create_connection((HOST, PORT), timeout=10); self.buf = b''; self.name = name

    def pump(self, timeout=.5):
        self.s.settimeout(timeout)
        try:
            d = self.s.recv(262144)
            if not d: raise ConnectionError('peer closed')
            self.buf += d
        except socket.timeout:
            pass

    def packets(self):
        ps, self.buf = unpack_packets(self.buf)
        return ps

    def ask(self, pkt, expected=None, timeout=5):
        self.s.sendall(pkt); end = time.time() + timeout; got = []
        while time.time() < end:
            try:
                self.pump(.3)
            except ConnectionError:
                break
            got.extend(self.packets())
            if expected is not None and any(x[0] == expected for x in got): return got
        return got

    def drain(self, seconds):
        got = []; end = time.time() + seconds
        while time.time() < end:
            try:
                self.pump(min(.3, max(0.05, end - time.time())))
            except ConnectionError:
                break
            got.extend(self.packets())
        return got

    def close(self):
        try: self.s.close()
        except Exception: pass


def cap_login(name):
    c = Conn(name)
    r = c.ask(pdu(T_CAP), T_CAP_R); code = r[0][2].split(b'\0')[0] if r else b''
    r = c.ask(pdu(T_LOGIN, msg=code + b'\0', dat=f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()), T_LOGIN_R)
    ret = struct.unpack('<i', r[0][1][:4])[0] if r else -99
    if ret != 1:
        c.close(); raise RuntimeError(f'login {name} ret={ret}')
    return c


def ensure_user(name):
    c = Conn(name); r = c.ask(pdu(T_CAP), T_CAP_R); code = r[0][2].split(b'\0')[0] if r else b''
    c.ask(pdu(T_REG, msg=code + b'\0', dat=f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()), T_REG_R)
    c.close()
    cap_login(name).close()


def greq(c, typ, gid=0, body=b'', expected=None, timeout=5):
    """发送一个群请求；返回 (code, payload_body_after_code, payload_full)"""
    ps = c.ask(pdu(typ, msg=envelope(gid, body)), expected or typ + 1, timeout)
    want = expected or typ + 1
    for x in ps:
        if x[0] == want:
            d = x[2]
            code = struct.unpack('<I', d[20:24])[0] if len(d) >= 24 else None
            return code, d[24:], d
    return None, b'', b''


def send_msg(c, gid, key, content):
    code, body, _ = greq(c, T_SEND, gid, string(key) + string(content))
    r = Rd(body)
    out = {'code': code}
    try:
        out['key'] = r.string(); out['seq'] = r.u64(); out['msg_id'] = r.u64(); out['created'] = r.u64()
    except Exception:
        pass
    return out


def pull(c, gid, mode, cursor, limit):
    code, body, _ = greq(c, T_PULL, gid, bytes([mode]) + u64(cursor) + u16(limit))
    out = {'code': code, 'records': []}
    if code != 0 or not body: return out
    r = Rd(body)
    out['mode'] = r.u8(); out['has_more'] = r.u8(); out['next_cursor'] = r.u64()
    out['last_seq'] = r.u64(); out['unread'] = r.u64(); n = r.u32()
    for _ in range(n):
        out['records'].append({'seq': r.u64(), 'msg_id': r.u64(), 'sender': r.string(),
                               'type': r.u8(), 'created': r.u64(), 'content': r.string()})
    return out


def confirm_read(c, gid, seq):
    code, body, _ = greq(c, T_CONFIRM, gid, u64(seq))
    eff = None
    if code == 0 and len(body) >= 8: eff = struct.unpack('<Q', body[:8])[0]
    return {'code': code, 'effective': eff}


def unread_summary(c):
    code, body, _ = greq(c, T_UNREAD, 0, b'')
    out = {'code': code, 'rows': {}}
    if code != 0 or not body: return out
    r = Rd(body); n = r.u32()
    for _ in range(n):
        gid = r.u64(); last_seq = r.u64(); last_read = r.u64(); unread = r.u64()
        out['rows'][gid] = (last_seq, last_read, unread)
    return out


def events_of(pkts, gid=None, typ=T_EVENT):
    """从原始包列表提取 90 事件：group_id, seq, msg_id, sender, type, created, content"""
    out = []
    for t, _dat, payload in pkts:
        if t != typ: continue
        try:
            r = Rd(payload)
            r.u16(); r.u16(); r.u64(); ev_gid = r.u64(); code = r.u32()
            gid2 = r.u64(); seq = r.u64(); mid = r.u64(); sender = r.string()
            mt = r.u8(); created = r.u64(); content = r.string()
        except Exception:
            continue
        if code != 0: continue
        if gid is not None and ev_gid != gid: continue
        out.append({'gid': ev_gid, 'seq': seq, 'msg_id': mid, 'sender': sender,
                    'type': mt, 'created': created, 'content': content})
    return out


def db_conn():
    lines = (ROOT / 'Server' / 'connect.config').read_text(encoding='utf-8').splitlines()
    return pymysql.connect(host=lines[3], port=int(lines[4]), user=lines[6],
                           password=lines[7], database=lines[5], autocommit=True)


def db_one(sql, args=()):
    with db_conn() as c:
        with c.cursor() as cur:
            cur.execute(sql, args); return cur.fetchone()


def db_all(sql, args=()):
    with db_conn() as c:
        with c.cursor() as cur:
            cur.execute(sql, args); return cur.fetchall()


USERS = ['g2owner', 'g2m1', 'g2m2', 'g2m3', 'g2out', 'g2quit', 'g2gone']

# ---------- 环境与迁移前置 ----------
commit = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT,
                        capture_output=True, text=True).stdout.strip()
if not start_server():
    print('[E] 无法启动 dist/Server.exe，套件中止')
    sys.exit(1)
for n in USERS:
    ensure_user(n)
print('[SETUP] users ready; candidate commit ' + commit)
cols = db_one("SELECT COUNT(*) FROM information_schema.columns WHERE table_schema=DATABASE() "
              "AND table_name='group_info' AND column_name='last_seq'")[0]
tbl = db_one("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() "
             "AND table_name='group_message'")[0]
ver = db_one('SELECT MAX(version) FROM schema_version')[0]
check('migration_applied', cols == 1 and tbl == 1 and ver >= 2026100101, f'last_seq={cols} table={tbl} schema={ver}')
idx = [r[2] for r in db_all('SHOW INDEX FROM group_message')]
check('unique_indexes_present', 'uk_group_seq' in idx and 'uk_group_client_msg' in idx, ','.join(sorted(set(idx))))
# 隔离本 harness 的数据域
with db_conn() as c:
    with c.cursor() as cur:
        cur.execute("DELETE FROM group_message WHERE group_id IN (SELECT group_id FROM group_info WHERE group_name LIKE 'G2 %')")
        cur.execute("DELETE FROM group_invitation WHERE group_id IN (SELECT group_id FROM group_info WHERE group_name LIKE 'G2 %')")
        cur.execute("DELETE FROM group_member WHERE group_id IN (SELECT group_id FROM group_info WHERE group_name LIKE 'G2 %')")
        cur.execute("DELETE FROM group_info WHERE group_name LIKE 'G2 %'")
        cur.execute("DELETE FROM message_info WHERE seed_name LIKE 'g2%' OR rev_name LIKE 'g2%'")

owner, m1, m2, m3 = cap_login('g2owner'), cap_login('g2m1'), cap_login('g2m2'), cap_login('g2m3')
out, quit_c, gone = cap_login('g2out'), cap_login('g2quit'), cap_login('g2gone')


def make_group(creator, name, invitees):
    code, body, _ = greq(creator, T_CREATE, 0, string(name))
    gid = struct.unpack('<Q', body[:8])[0] if code == 0 and len(body) >= 8 else 0
    for who, conn in invitees:
        code, body, _ = greq(creator, T_INVITE, gid, string(who) + string('k-%s-%s' % (name, who)))
        iid = struct.unpack('<Q', body[:8])[0] if code == 0 and len(body) >= 8 else 0
        a, _, _ = greq(conn, T_ACCEPT, 0, u64(iid))
        if a != 0:
            raise RuntimeError(f'accept {who} in {name} code={a}')
    return gid


# ========== G2-A：幂等 / 序号 / 失败无广播 / 权限 ==========
gidA = make_group(owner, 'G2 A', [('g2m1', m1), ('g2m2', m2), ('g2m3', m3),
                                  ('g2quit', quit_c), ('g2gone', gone)])
check('setup_group_a', gidA > 0, f'gid={gidA}')

# 清空加入系统消息带来的积压事件，便于后续计数
m2.drain(0.6); owner.drain(0.6)

# --- P1 幂等 ---
r1 = send_msg(m1, gidA, 'idem-k1', 'idem-1')
check('p1_send_ok', r1['code'] == 0 and r1.get('seq', 0) > 0, str(r1))
seq1 = r1.get('seq')
ev = events_of(m2.drain(0.6), gidA)
check('p1_first_broadcast_once', len([e for e in ev if e['seq'] == seq1]) == 1,
      f"events={len([e for e in ev if e['seq']==seq1])}")
r2 = send_msg(m1, gidA, 'idem-k1', 'idem-1')
check('p1_serial_replay_same_seq', r2['code'] == 0 and r2.get('seq') == seq1 and r2.get('msg_id') == r1.get('msg_id'),
      f"seq={r2.get('seq')} msg_id={r2.get('msg_id')}")
# 同键内容不同：仍返回既有行，不覆盖、不新增
r3 = send_msg(m1, gidA, 'idem-k1', 'idem-1-changed')
row = db_one("SELECT COUNT(*), MAX(content) FROM group_message WHERE group_id=%s AND client_msg_id='idem-k1'", (gidA,))
check('p1_replay_returns_existing', r3['code'] == 0 and r3.get('seq') == seq1 and row[0] == 1 and row[1] == 'idem-1',
      f"code={r3['code']} rows={row[0]} content={row[1]}")
# 流水线双发同键（传输层交错）
pkts = m1.drain(0.2)
pk = pdu(T_SEND, msg=envelope(gidA, string('idem-k1') + string('idem-1')))
got = m1.ask(pk + pk, T_SEND_R, timeout=6)
resp = [g for g in got if g[0] == T_SEND_R]
codes = [struct.unpack('<I', g[2][20:24])[0] for g in resp]
seqs = []
for g in resp:
    rr = Rd(g[2][24:])
    try:
        rr.string(); seqs.append(rr.u64())
    except Exception:
        seqs.append(None)
row = db_one("SELECT COUNT(*) FROM group_message WHERE group_id=%s AND client_msg_id='idem-k1'", (gidA,))
check('p1_pipelined_replay_one_row', len(codes) == 2 and codes == [0, 0] and seqs == [seq1, seq1] and row[0] == 1,
      f"codes={codes} seqs={seqs} rows={row[0]}")
ev = events_of(m2.drain(0.6), gidA)
check('p1_replay_no_rebroadcast', len([e for e in ev if e['seq'] == seq1]) == 0, f"events={len(ev)}")
# -2104：同键不同发送者
r4 = send_msg(m2, gidA, 'idem-k1', 'hijack')
check('p1_cross_sender_rejected', r4['code'] == N2104, f"code={r4['code']}")  # -2104

# --- P2 序号：4 连接流水线对打 ---
burst_seqs = []
errs = []


def burst(conn, tag):
    try:
        for i in range(4):
            r = send_msg(conn, gidA, f'burst-{tag}-{i}', f'burst {tag} {i}')
            if r['code'] == 0: burst_seqs.append(r['seq'])
            else: errs.append(r)
    except Exception as e:
        errs.append(str(e))


threads = [threading.Thread(target=burst, args=(m1, 'm1')), threading.Thread(target=burst, args=(m2, 'm2')),
           threading.Thread(target=burst, args=(m3, 'm3')), threading.Thread(target=burst, args=(owner, 'o'))]
for t in threads: t.start()
for t in threads: t.join()
rows = db_all('SELECT seq FROM group_message WHERE group_id=%s ORDER BY seq', (gidA,))
seq_list = [r[0] for r in rows]
contiguous = seq_list == list(range(1, len(seq_list) + 1))
check('p2_seq_contiguous_unique', not errs and len(burst_seqs) == 16 and len(set(burst_seqs)) == 16
      and contiguous and len(seq_list) == 22,
      f'burst={len(burst_seqs)} uniq={len(set(burst_seqs))} rows={len(seq_list)} contig={contiguous} errs={errs[:2]}')

# --- P3 失败/离线不丢消息 ---
before = db_one('SELECT COUNT(*) FROM group_message WHERE group_id=%s', (gidA,))[0]
m2.drain(0.4)
big = 'x' * 2001
r5 = send_msg(m1, gidA, 'over-k', big)
row = db_one('SELECT COUNT(*) FROM group_message WHERE group_id=%s', (gidA,))[0]
ev = events_of(m2.drain(0.5), gidA)
check('p3_oversize_rejected_no_broadcast', r5['code'] == N2002 and row == before and len(ev) == 0,
      f"code={r5['code']} rows={row}/{before} events={len(ev)}")  # -2002（解析层拒绝）
r6 = send_msg(m1, gidA, 'empty-k', '')
check('p3_empty_content_rejected', r6['code'] == N2103, f"code={r6['code']}")  # -2103
# 离线成员：广播失败（连接不存在）→ 消息仍提交，重连可拉取
m3_before = unread_summary(m3)['rows'].get(gidA, (0, 0, 0))
m3.close()
r7 = send_msg(m1, gidA, 'p3-off', 'offline-catchup-1')
ev_owner = events_of(owner.drain(0.6), gidA)
check('p3_send_while_member_offline_persisted', r7['code'] == 0
      and len([e for e in ev_owner if e['seq'] == r7.get('seq')]) == 1, f"code={r7['code']}")
m3 = cap_login('g2m3')
summ = unread_summary(m3)['rows'].get(gidA)
p = pull(m3, gidA, 0, m3_before[1], 50)
contents = [rec['content'] for rec in p['records']]
check('p3_offline_catchup_pull', summ is not None and summ[2] == m3_before[2] + 1
      and b'offline-catchup-1' in contents,
      f"unread={summ[2] if summ else None} base={m3_before[2]} pulled={contents[-1:]}")

# --- P5 权限三断（非成员/已退出/被移除）+ 认证门（放在 G2-A 与 G2-D） ---
for who, conn in (('g2out', out),):
    a, _, _ = greq(conn, T_SEND, gidA, string('out-k') + string('x'))
    b, _, _ = greq(conn, T_PULL, gidA, bytes([0]) + u64(0) + u16(10))
    c, _, _ = greq(conn, T_CONFIRM, gidA, u64(1))
    check('p5_outsider_denied_all', a == N2101 and b == N2101 and c == N2101,
          f'send={a} pull={b} confirm={c}')  # -2101
le, _, _ = greq(quit_c, T_LEAVE, gidA)
gone_rm, _, _ = greq(owner, T_REMOVE, gidA, string('g2gone'))
le2, _, _ = greq(quit_c, T_SEND, gidA, string('q-k') + string('x'))
lv2, _, _ = greq(quit_c, T_PULL, gidA, bytes([0]) + u64(0) + u16(10))
lc2, _, _ = greq(quit_c, T_CONFIRM, gidA, u64(1))
check('p5_left_member_denied_all', le == 0 and gone_rm == 0
      and le2 == N2101 and lv2 == N2101 and lc2 == N2101,
      f'leave={le} send={le2} pull={lv2} confirm={lc2}')
gr2, _, _ = greq(gone, T_SEND, gidA, string('g-k') + string('x'))
gp2, _, _ = greq(gone, T_PULL, gidA, bytes([0]) + u64(0) + u16(10))
gc2, _, _ = greq(gone, T_CONFIRM, gidA, u64(1))
check('p5_removed_member_denied_all', gr2 == N2101 and gp2 == N2101 and gc2 == N2101,
      f'send={gr2} pull={gp2} confirm={gc2}')
# 系统消息同为消息行：quit 退出 / gone 被移除各产生一条 system 行
sys_rows = db_all("SELECT seq,content,msg_type,client_msg_id FROM group_message WHERE group_id=%s "
                  "AND msg_type='system' ORDER BY seq", (gidA,))
sys_text = ' | '.join(r[1] for r in sys_rows)
check('p5_system_messages_recorded', any('加入了群聊' in t for t in sys_text.split(' | '))
      and any('退出了群聊' in t for t in sys_text.split(' | '))
      and any('被移出群聊' in t for t in sys_text.split(' | ')),
      f'system_rows={len(sys_rows)}')
# 未认证连接：认证门（-1001）
raw = Conn()
code, _, _ = greq(raw, T_SEND, gidA, string('anon') + string('x'))
raw.close()
check('p5_unauth_gate_reject', code == 0xFFFFFC17, f'code={code}')  # -1001

# ========== G2-B：离线补齐 + 未读归零 ==========
gidB = make_group(owner, 'G2 B', [('g2m2', m2)])
s = unread_summary(m2)['rows'][gidB]
confirm_read(m2, gidB, s[0])
m2b_state = unread_summary(m2)['rows'][gidB]
m2.close()
for i in range(5):
    r = send_msg(owner, gidB, f'off-{i}', f'offline-msg-{i}')
    if r['code'] != 0: raise RuntimeError(f'offline send failed {r}')
m2 = cap_login('g2m2')
s2 = unread_summary(m2)['rows'][gidB]
p = pull(m2, gidB, 0, m2b_state[1], 50)
ok_rows = [rec['content'] for rec in p['records']]
check('p4_offline_unread_equals_m', s2[2] == 5, f'unread={s2[2]}')
check('p4_offline_pull_from_cursor', p['code'] == 0 and len(p['records']) == 5
      and ok_rows == [f'offline-msg-{i}'.encode() for i in range(5)], f'pulled={ok_rows}')
c = confirm_read(m2, gidB, s2[0])
s3 = unread_summary(m2)['rows'][gidB]
check('p4_confirm_read_zeroes_unread', c['code'] == 0 and c['effective'] == s2[0] and s3[2] == 0,
      f"eff={c['effective']} unread={s3[2]}")

# ========== G2-P：分页与边界 ==========
gidP = make_group(owner, 'G2 P', [('g2m1', m1)])
confirm_read(m1, gidP, 1)          # 群内只有 join 系统消息 seq=1
for i in range(12):
    send_msg(owner, gidP, f'pg-{i}', f'page-{i}')
p1 = pull(m1, gidP, 0, 0, 5)
p2 = pull(m1, gidP, 0, p1['next_cursor'], 5)
p3 = pull(m1, gidP, 0, p2['next_cursor'], 5)
check('p6_forward_pages', (p1['has_more'], p2['has_more'], p3['has_more']) == (1, 1, 0)
      and [r['seq'] for r in p1['records']] == [1, 2, 3, 4, 5]
      and [r['seq'] for r in p2['records']] == [6, 7, 8, 9, 10]
      and [r['seq'] for r in p3['records']] == [11, 12, 13],
      f"m={p1['has_more']}{p2['has_more']}{p3['has_more']} next={p3['next_cursor']}")
check('p6_pull_unread_field', p1['unread'] == 12 and p1['last_seq'] == 13, f"unread={p1['unread']} last={p1['last_seq']}")
# mode=1 语义（设计 §7）：seq<=cursor 含边界，next_cursor=本批最小 seq；
# 客户端续翻以 cursor=已渲染最小 seq-1 严格前移（客户端侧已同步），页面无重叠无缺。
b1 = pull(m1, gidP, 1, 13, 5)
b2 = pull(m1, gidP, 1, b1['next_cursor'] - 1, 5)
b3 = pull(m1, gidP, 1, b2['next_cursor'] - 1, 5)
bs = [[r['seq'] for r in b['records']] for b in (b1, b2, b3)]
union = sorted(s for page in bs for s in page)
ok_b = (all(page == sorted(page) for page in bs)
        and bs[0] == [9, 10, 11, 12, 13] and bs[1] == [4, 5, 6, 7, 8] and bs[2] == [1, 2, 3]
        and [b['has_more'] for b in (b1, b2, b3)] == [1, 1, 0]
        and [b['next_cursor'] for b in (b1, b2, b3)] == [9, 4, 1]
        and union == list(range(1, 14)))
check('p6_backward_pages_ascending', ok_b,
      f"pages={bs} has_more={[b['has_more'] for b in (b1, b2, b3)]} next={[b['next_cursor'] for b in (b1, b2, b3)]}")
lim0, _, _ = greq(m1, T_PULL, gidP, bytes([0]) + u64(0) + u16(0))
lim101, _, _ = greq(m1, T_PULL, gidP, bytes([0]) + u64(0) + u16(101))
lim100 = pull(m1, gidP, 0, 0, 100)
check('p6_limit_bounds', lim0 == N2002 and lim101 == N2002 and lim100['code'] == 0
      and len(lim100['records']) == 13, f'lim0={lim0} lim101={lim101} lim100={lim100["code"]}')  # -2002
mono = confirm_read(m1, gidP, 7)
back = confirm_read(m1, gidP, 3)
over = confirm_read(m1, gidP, 999)
check('p6_confirm_monotonic_clamped', mono['effective'] == 7 and back['effective'] == 7 and over['effective'] == 13,
      f"7->{mono['effective']} 3->{back['effective']} 999->{over['effective']}")

# ========== G2-D：解散后发送被拒且无广播 ==========
gidD = make_group(owner, 'G2 D', [('g2m1', m1)])
greq(owner, T_DISSOLVE, gidD)
m1.drain(0.4)
rows_before = db_one('SELECT COUNT(*) FROM group_message WHERE group_id=%s', (gidD,))[0]
rd, _, _ = greq(m1, T_SEND, gidD, string('dis-k') + string('x'))
ev = events_of(owner.drain(0.6), gidD)
rows_after = db_one('SELECT COUNT(*) FROM group_message WHERE group_id=%s', (gidD,))[0]
check('p3_dissolved_send_rejected_no_broadcast', rd == N2102 and rows_after == rows_before and len(ev) == 0,
      f'code={rd} rows={rows_after} events={len(ev)}')  # -2102

# ========== G2-Q：广播队列满（慢连接只丢实时投递） ==========
gidQ = make_group(owner, 'G2 Q', [('g2m1', m1), ('g2m2', m2)])
s = unread_summary(m2)['rows'][gidQ]
confirm_read(m2, gidQ, s[0])
q_last_read = s[0]                 # m2 已确认游标（两条 join 系统消息）
m1.drain(0.5); owner.drain(0.5)
stop = threading.Event()
fast_counts = {'m1': 0}
# m1=快读者（独立线程排空，只读 m1 socket，与主线程无共享）；m2=慢读者（全程不读）；
# owner=发送方，其 socket 由主线程 ask() 顺带排空，不另开线程避免双读者竞争
def drain_count(conn, key):
    while not stop.is_set():
        try:
            ev = events_of(conn.drain(0.15), gidQ)
        except Exception:
            break
        fast_counts[key] += len([e for e in ev if e['type'] == 0])


t1 = threading.Thread(target=drain_count, args=(m1, 'm1'))
t1.start()
N = 800
log_offset = SERVER_LOG.stat().st_size if SERVER_LOG.exists() else 0
send_ok = 0
payload = 'z' * 2000
for i in range(N):
    r = send_msg(owner, gidQ, f'slow-{i}', payload)
    if r['code'] == 0: send_ok += 1
stop.set(); t1.join()
fast_counts['m1'] += len([e for e in events_of(m1.drain(1.0), gidQ) if e['type'] == 0])
rows_q = db_one('SELECT COUNT(*) FROM group_message WHERE group_id=%s ORDER BY seq', (gidQ,))[0]
check('p7_burst_persisted', send_ok == N and rows_q == N + 2, f'sent={send_ok} rows={rows_q}')
m2_ev = events_of(m2.drain(3.0), gidQ)
m2_text = len([e for e in m2_ev if e['type'] == 0])
# 服务端控制台为本地代码页（GBK）编码；按字节匹配 ASCII 锚点（[GROUP]/user=/drops=）。
# 只取本次突发新增的日志段，避免历史运行残留造成假阳性。
log_bytes = SERVER_LOG.read_bytes()[log_offset:] if SERVER_LOG.exists() else b''
drop_m2 = re.findall(rb'\[GROUP\].*user=\s*"g2m2".*drops=\s*\d+', log_bytes)
drop_m1 = re.findall(rb'\[GROUP\].*user=\s*"g2m1".*drops=\s*\d+', log_bytes)
check('p7_slow_conn_dropped', len(drop_m2) > 0 and m2_text < N, f'drops_log={len(drop_m2)} recv={m2_text}/{N}')
check('p7_fast_conns_unaffected', fast_counts['m1'] == N and not drop_m1,
      f"m1={fast_counts['m1']}/{N} log_m1={len(drop_m1)}")
# 被丢实时投递后：拉取兜底可补齐全量
got = 0; cursor = q_last_read; guard = 0
while guard < 20:
    guard += 1
    p = pull(m2, gidQ, 0, cursor, 100)
    if p['code'] != 0: break
    got += len(p['records'])
    cursor = p['next_cursor']
    if not p['has_more']: break
check('p7_pull_catchup_complete', got == N, f'pulled={got}/{N}')
sq = unread_summary(m2)['rows'][gidQ]
check('p7_unread_reflects_all', sq[2] == N, f'unread={sq[2]}')

# ========== 回归 ==========
# a) 一对一在线：15 转发给接收方 + message_info issuccess=1
t = b'oct 01 10:00:00 2026'.ljust(30, b'\0')
content = 'p8-hello'.encode()
chat_msg = struct.pack('<II', 38 + len(content) + 1, len(content) + 1) + t + content + b'\0'
m2.drain(0.3)
m1.s.sendall(pdu(T_CHAT, msg=chat_msg, dat=f32('g2m1') + f32('g2m2')))
got = m2.drain(0.8)
fwd = [g for g in got if g[0] == T_CHAT]
chk = content in fwd[0][2] if fwd else False
row = db_one("SELECT issuccess FROM message_info WHERE seed_name='g2m1' AND rev_name='g2m2' ORDER BY id DESC LIMIT 1")
check('p8a_one_to_one_online', len(fwd) == 1 and chk and row and row[0] == 1, f'fwd={len(fwd)} issuccess={row[0] if row else None}')
# b) 一对一离线：目标下线后消息落库 issuccess=0
m2.close()
m1.s.sendall(pdu(T_CHAT, msg=chat_msg, dat=f32('g2m1') + f32('g2m2')))
m1.drain(0.5)
time.sleep(0.3)
row = db_one("SELECT issuccess FROM message_info WHERE seed_name='g2m1' AND rev_name='g2m2' ORDER BY id DESC LIMIT 1")
check('p8b_one_to_one_offline_row', row is not None and row[0] == 0, f'issuccess={row[0] if row else None}')
m2 = cap_login('g2m2')
# c) 文件分享外链全流程（F-1 场景回归）：建文件→生成两次链接→他人解析第二次链接→落盘
fsroot = DIST / 'filesys'   # 部署运行形态：服务端 cwd=dist，其 ./filesys 即 dist/filesys
(fsroot / 'g2m1').mkdir(parents=True, exist_ok=True)
(fsroot / 'g2m1' / 'g2p8.bin').write_text('G2-P8-CONTENT')   # 发布者 m1 本人根
r = m1.ask(pdu(T_CPL, msg=b'g2p8.bin'), T_CPL_R)
link = r[0][2].split(b'\0')[0].decode() if r and r[0][0] == T_CPL_R else ''
rr = m1.ask(pdu(T_CPL, msg=b'g2p8.bin'), T_CPL_R)
link2 = rr[0][2].split(b'\0')[0].decode() if rr and rr[0][0] == T_CPL_R else ''
res = out.ask(pdu(T_RES, msg=link2.split('cloud-disk://share/')[-1].encode()), T_RES_R)
res_ok = bool(res) and res[0][1][0] != 0
landed = list((fsroot / 'g2out' / '分享链接').glob('g2p8.bin')) if (fsroot / 'g2out' / '分享链接').is_dir() else []
check('p8c_publish_link_flow', bool(link) and bool(link2) and res_ok and bool(landed),
      f'res_ok={res_ok} landed={[p.name for p in landed]}')
# d) GROUP-001 生命周期整链（既有 harness 原样重跑）
ge = ROOT / 'build' / 'evidence' / 'req-ext-group-001-independent-8757ad6' / 'group_e2e.py'
if ge.exists():
    pr = subprocess.run([sys.executable, str(ge)], cwd=ROOT, capture_output=True, text=True, timeout=180)
    tail = (pr.stdout or '').strip().splitlines()[-1] if pr.stdout else ''
    check('p8d_group001_lifecycle_regression', pr.returncode == 0 and 'PASS' in tail, f'rc={pr.returncode} tail={tail}')
else:
    check('p8d_group001_lifecycle_regression', False, 'harness not found: ' + str(ge))

# ---------- 汇总 ----------
passed = sum(1 for _, ok, _ in results if ok)
print(f'\nRESULT {passed}/{len(results)} PASS  commit={commit}  {time.strftime("%Y-%m-%d %H:%M:%S")}')
for line in notes:
    print('NOTE ' + line)
outdir = ROOT / 'build' / 'group002'
(outdir / 'verify-output.log').write_text(
    '\n'.join(f"[{'PASS' if ok else 'FAIL'}] {label} :: {detail}" for label, ok, detail in results)
    + f'\nRESULT {passed}/{len(results)} PASS commit={commit}\n', encoding='utf-8')
(outdir / 'evidence.json').write_text(json.dumps(
    {'commit': commit, 'timestamp': time.strftime('%Y-%m-%d %H:%M:%S'), 'passed': passed, 'total': len(results),
     'results': [{'label': l, 'ok': o, 'detail': d} for l, o, d in results]}, ensure_ascii=False, indent=2), encoding='utf-8')
stop_server()
sys.exit(0 if passed == len(results) else 1)
