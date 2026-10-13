# -*- coding: utf-8 -*-
# REQ-EXT-GROUP-002 independent test harness (Development OS v1.7, LARGE test node).
# Written from scratch against candidate 03dc01d source; assertions derived by reading
# Server/operatedb.cpp handleGroupMessageOperation / msghandler.cpp groupRequest /
# mytcpsocket.cpp gates. Does NOT reuse or trust the development harness verify.py.
import hashlib, json, os, re, socket, struct, sys, threading, time
from pathlib import Path

import pymysql

HOST, PORT, HDR, PWD = '127.0.0.1', 5000, 76, '123456'
ROOT = Path(__file__).resolve().parents[2]          # worktree root
FILESYS = Path('E:/qt/xiangmu/dist/filesys')        # from connect.config line 3

T_REG, T_REG_R, T_LOGIN, T_LOGIN_R, T_CAP, T_CAP_R = 1, 2, 3, 4, 44, 45
T_CHAT, T_GETCHAT, T_GETCHAT_R = 15, 17, 18
T_CPL, T_CPL_R, T_RES, T_RES_R = 52, 53, 54, 55
T_CREATE, T_CREATE_R = 60, 61
T_INVITE, T_INVITE_R = 62, 63
T_ACCEPT, T_ACCEPT_R = 64, 65
T_LEAVE, T_LEAVE_R = 68, 69
T_REMOVE, T_REMOVE_R = 70, 71
T_DISSOLVE, T_DISSOLVE_R = 76, 77
T_EVENT = 90
T_SEND, T_SEND_R = 88, 89
T_PULL, T_PULL_R = 91, 92
T_CONFIRM, T_CONFIRM_R = 93, 94
T_UNREAD, T_UNREAD_R = 95, 96

results = []
def check(label, cond, detail=''):
    results.append((label, bool(cond), detail))
    print(('[PASS] ' if cond else '[FAIL] ') + label + (' :: ' + str(detail) if detail != '' else ''))
    sys.stdout.flush()

def f32(s): return s.encode()[:31].ljust(32, b'\0')
def pdu(t, msg=b'', dat=b''):
    return struct.pack('<III', HDR + len(msg), len(msg), t) + dat[:64].ljust(64, b'\0') + msg
def u16(x): return struct.pack('<H', x)
def u32(x): return struct.pack('<I', x)
def u64(x): return struct.pack('<Q', x)
def string(x):
    b = x.encode() if isinstance(x, str) else x
    return u32(len(b)) + b
def envelope(gid=0, body=b''):
    return u16(1) + u16(20) + u64(int(time.time_ns() & ((1 << 63) - 1))) + u64(gid) + body
def unpack(buf):
    out = []
    while len(buf) >= HDR:
        tl, ml, ty = struct.unpack('<III', buf[:12])
        if tl < HDR or tl != HDR + ml or len(buf) < tl: break
        out.append((ty, buf[12:76], buf[76:tl])); buf = buf[tl:]
    return out, buf

class Rd:
    def __init__(s, d): s.d, s.o = d, 0
    def u8(s): v = s.d[s.o]; s.o += 1; return v
    def u16(s): v = struct.unpack_from('<H', s.d, s.o)[0]; s.o += 2; return v
    def u32(s): v = struct.unpack_from('<I', s.d, s.o)[0]; s.o += 4; return v
    def u64(s): v = struct.unpack_from('<Q', s.d, s.o)[0]; s.o += 8; return v
    def string(s):
        n = s.u32(); v = s.d[s.o:s.o + n]; s.o += n; return v
    def done(s): return s.o == len(s.d)

class Conn:
    def __init__(self, name=None):
        self.s = socket.create_connection((HOST, PORT), timeout=10); self.buf = b''; self.name = name
    def pump(self, timeout=.4):
        self.s.settimeout(timeout)
        try:
            d = self.s.recv(262144)
            if not d: raise ConnectionError('peer closed')
            self.buf += d
        except socket.timeout:
            pass
    def packets(self):
        ps, self.buf = unpack(self.buf); return ps
    def send(self, pkt): self.s.sendall(pkt)
    def collect(self, want_type, count, timeout=20):
        got, end = [], time.time() + timeout
        while len([g for g in got if g[0] == want_type]) < count and time.time() < end:
            try: self.pump(.2)
            except ConnectionError: break
            got.extend(self.packets())
        return got
    def ask(self, pkt, want, timeout=8):
        self.send(pkt); return self.collect(want, 1, timeout)
    def drain(self, seconds):
        got, end = [], time.time() + seconds
        while time.time() < end:
            try: self.pump(min(.25, max(.05, end - time.time())))
            except ConnectionError: break
            got.extend(self.packets())
        return got
    def close(self):
        try: self.s.close()
        except Exception: pass

def cap_login(name):
    c = Conn(name)
    r = c.ask(pdu(T_CAP), T_CAP_R)
    code = r[0][2].split(b'\0')[0] if r else b''
    if not code: return None
    r = c.ask(pdu(T_LOGIN, msg=code + b'\0', dat=f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()), T_LOGIN_R)
    ret = struct.unpack('<i', r[0][1][:4])[0] if r else -99
    if ret != 1:
        c.close(); return None
    return c

def ensure_user(name):
    c = Conn(name)
    r = c.ask(pdu(T_CAP), T_CAP_R)
    code = r[0][2].split(b'\0')[0] if r else b''
    c.ask(pdu(T_REG, msg=code + b'\0', dat=f32(name) + hashlib.md5(PWD.encode()).hexdigest().encode()), T_REG_R)
    c.close()
    cc = cap_login(name)
    if cc is None: raise RuntimeError('cannot login ' + name)
    return cc

def greq(c, typ, gid=0, body=b'', timeout=8):
    ps = c.ask(pdu(typ, msg=envelope(gid, body)), typ + 1, timeout)
    for t, _dat, payload in ps:
        if t == typ + 1:
            code = struct.unpack('<i', payload[20:24])[0] if len(payload) >= 24 else None
            return code, payload[24:]
    return None, b''

def send_msg(c, gid, key, content, timeout=8):
    code, body = greq(c, T_SEND, gid, string(key) + string(content), timeout)
    o = {'code': code}
    if code == 0:
        r = Rd(body); o['key'] = r.string(); o['seq'] = r.u64(); o['msg_id'] = r.u64(); o['created'] = r.u64()
    return o

def pull(c, gid, mode, cursor, limit, timeout=8):
    code, body = greq(c, T_PULL, gid, bytes([mode]) + u64(cursor) + u16(limit), timeout)
    o = {'code': code, 'records': []}
    if code != 0: return o
    r = Rd(body)
    o['mode'] = r.u8(); o['has_more'] = r.u8(); o['next_cursor'] = r.u64()
    o['last_seq'] = r.u64(); o['unread'] = r.u64(); n = r.u32()
    for _ in range(n):
        o['records'].append({'seq': r.u64(), 'msg_id': r.u64(), 'sender': r.string(),
                             'type': r.u8(), 'created': r.u64(), 'content': r.string()})
    o['trailing_ok'] = r.done()
    return o

def confirm(c, gid, seq, timeout=8):
    code, body = greq(c, T_CONFIRM, gid, u64(seq), timeout)
    eff = struct.unpack('<Q', body[:8])[0] if code == 0 and len(body) >= 8 else None
    return {'code': code, 'effective': eff}

def unread_summary(c, timeout=8):
    code, body = greq(c, T_UNREAD, 0, b'', timeout)
    o = {'code': code, 'rows': {}}
    if code != 0: return o
    r = Rd(body); n = r.u32()
    for _ in range(n):
        gid = r.u64(); ls = r.u64(); lr = r.u64(); un = r.u64()
        o['rows'][gid] = (ls, lr, un)
    return o

def events(pkts, gid=None):
    out = []
    for t, _d, payload in pkts:
        if t != T_EVENT: continue
        try:
            r = Rd(payload); r.u16(); r.u16(); r.u64(); eg = r.u64(); code = r.u32()
            r.u64(); seq = r.u64(); mid = r.u64(); snd = r.string(); mt = r.u8(); ct = r.u64(); cont = r.string()
        except Exception:
            continue
        if code != 0: continue
        if gid is not None and eg != gid: continue
        out.append({'gid': eg, 'seq': seq, 'msg_id': mid, 'sender': snd.decode('utf-8', 'replace'),
                    'type': mt, 'created': ct, 'content': cont.decode('utf-8', 'replace')})
    return out

def db():
    cfg = (ROOT / 'Server' / 'connect.config').read_text(encoding='utf-8').splitlines()
    return pymysql.connect(host=cfg[3], port=int(cfg[4]), user=cfg[6], password=cfg[7], database=cfg[5], autocommit=True)

def q1(sql, args=()):
    with db() as c, c.cursor() as cur:
        cur.execute(sql, args); return cur.fetchone()

def qa(sql, args=()):
    with db() as c, c.cursor() as cur:
        cur.execute(sql, args); return cur.fetchall()

def last_seq(gid): return q1('SELECT last_seq FROM group_info WHERE group_id=%s', (gid,))[0],
def member_status(gid, u):
    r = q1('SELECT status FROM group_member WHERE group_id=%s AND user_name=%s', (gid, u))
    return r[0] if r else None
def last_read(gid, u): return q1('SELECT last_read_seq FROM group_member WHERE group_id=%s AND user_name=%s', (gid, u))[0]

USERS = ['it2own', 'it2a', 'it2b', 'it2c', 'it2x', 'it2q', 'it2rm', 'it2off', 'it2fast', 'it2slow',
         'it2c1', 'it2c2', 'it2c3']

def make_group(owner, name, members):
    code, body = greq(owner, T_CREATE, 0, string(name))
    if code != 0: raise RuntimeError('create %s code=%s' % (name, code))
    gid = struct.unpack('<Q', body[:8])[0]
    for who, conn in members:
        code, b2 = greq(owner, T_INVITE, gid, string(who) + string('k-%s-%s' % (name, who)))
        iid = struct.unpack('<Q', b2[:8])[0] if code == 0 and len(b2) >= 8 else 0
        ac, _ = greq(conn, T_ACCEPT, 0, u64(iid))
        if ac != 0: raise RuntimeError('accept %s in %s code=%s' % (who, name, ac))
    return gid

# ============================== SETUP ==============================
print('==== REQ-EXT-GROUP-002 independent test ====')
print('candidate: ' + os.popen('git -C "%s" rev-parse HEAD' % ROOT).read().strip())
print('server exe sha256: ' + hashlib.sha256((ROOT / 'dist' / 'Server.exe').read_bytes()).hexdigest())
CONSQ = [n for n in USERS]
conns = {}
for n in CONSQ:
    if n in ('it2off', 'it2c3'):   # kept offline until needed
        ensure_user(n).close()
    else:
        ensure_user(n).close()
        conns[n] = cap_login(n)
print('[SETUP] users ready')

# --- S1 migration / schema / indexes ---
cols = q1("SELECT COUNT(*) FROM information_schema.columns WHERE table_schema=DATABASE() AND table_name='group_info' AND column_name='last_seq'")[0]
tbl = q1("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() AND table_name='group_message'")[0]
ver = q1('SELECT MAX(version) FROM schema_version')[0]
idx = sorted({r[2] for r in qa('SHOW INDEX FROM group_message')})
check('S1_migration_applied', cols == 1 and tbl == 1 and ver >= 2026100101, 'last_seq=%s table=%s schema=%s' % (cols, tbl, ver))
check('S1_unique_indexes', 'uk_group_seq' in idx and 'uk_group_client_msg' in idx, ','.join(idx))
uqm = q1("SELECT COUNT(*) FROM information_schema.statistics WHERE table_schema=DATABASE() AND table_name='group_message' AND index_name='uk_group_client_msg' AND column_name='client_msg_id' AND nullable='YES'")[0]
check('S1_client_msg_id_nullable', uqm == 1, 'nullable=%s' % uqm)

owner_a, a, b, c_, x = conns['it2own'], conns['it2a'], conns['it2b'], conns['it2c'], conns['it2x']
q_, rm = conns['it2q'], conns['it2rm']

# ============ G-A: idempotency / broadcast / seq / permissions ============
gA = make_group(owner_a, 'IT2 A', [('it2a', a), ('it2b', b), ('it2c', c_), ('it2q', q_), ('it2rm', rm)])
check('S2_group_a_created', gA > 0, 'gid=%s' % gA)
for cn in (a, b, owner_a): cn.drain(0.5)

# --- T-Idem1: serial replay, same key & content -> same seq/msg_id, 1 row ---
r1 = send_msg(a, gA, 'idem-1', 'hello-1')
check('IDEM_serial_first_ok', r1['code'] == 0 and r1.get('seq', 0) > 0, r1)
ev_b = events(b.drain(0.7), gA)
check('IDEM_first_broadcast_once', len([e for e in ev_b if e['seq'] == r1.get('seq')]) == 1,
      'events=%d' % len([e for e in ev_b if e['seq'] == r1.get('seq')]))
check('IDEM_broadcast_fields', any(e['seq'] == r1['seq'] and e['content'] == 'hello-1' and e['sender'] == 'it2a'
                                   and e['msg_id'] == r1['msg_id'] and e['type'] == 0 for e in ev_b), ev_b[:1])
r2 = send_msg(a, gA, 'idem-1', 'hello-1')
check('IDEM_serial_replay_same_ids', r2['code'] == 0 and r2.get('seq') == r1.get('seq') and r2.get('msg_id') == r1['msg_id'], r2)
ev_b2 = events(b.drain(0.8), gA)
check('IDEM_replay_no_rebroadcast', len([e for e in ev_b2 if e['seq'] == r1['seq']]) == 0, 'rebroadcast=%d' % len(ev_b2))
r3 = send_msg(a, gA, 'idem-1', 'DIFFERENT-CONTENT')
row = q1('SELECT seq,content FROM group_message WHERE group_id=%s AND client_msg_id=%s', (gA, 'idem-1'))
check('IDEM_replay_diff_content_returns_original', r3['code'] == 0 and r3['seq'] == r1['seq'] and row[1] == 'hello-1', 'row=%s r3=%s' % (row, r3))
n = q1('SELECT COUNT(*) FROM group_message WHERE group_id=%s AND client_msg_id=%s', (gA, 'idem-1'))[0]
check('IDEM_single_row', n == 1, 'rows=%d' % n)

# --- T-Idem2: cross-sender same key in same group -> reject -2104 ---
rc = send_msg(b, gA, 'idem-1', 'other-sender')
check('IDEM_cross_sender_same_key_rejected', rc['code'] == -2104, rc)
n = q1('SELECT COUNT(*) FROM group_message WHERE group_id=%s AND client_msg_id=%s', (gA, 'idem-1'))[0]
check('IDEM_cross_sender_no_new_row', n == 1, 'rows=%d' % n)

# --- T-Idem3: pipelined concurrent replay of same key on one connection ---
key = 'idem-pipe-1'
pkts = b''.join(pdu(T_SEND, msg=envelope(gA, string(key) + string('pipe')), ) for _ in range(8))
a.send(pkts)
got = a.collect(T_SEND_R, 8, timeout=15)
codes = [struct.unpack('<i', p[2][20:24])[0] for p in got if p[0] == T_SEND_R]
seqs = []
for p in got:
    if p[0] == T_SEND_R and struct.unpack('<i', p[2][20:24])[0] == 0:
        rr = Rd(p[2][24:]); rr.string(); seqs.append(rr.u64())
check('IDEM_pipelined_all_ok', len(codes) == 8 and all(cd == 0 for cd in codes), codes)
check('IDEM_pipelined_single_seq', len(set(seqs)) == 1 and len(seqs) == 8, seqs)
rows = q1('SELECT COUNT(*) FROM group_message WHERE group_id=%s AND client_msg_id=%s', (gA, key))[0]
check('IDEM_pipelined_one_row', rows == 1, 'rows=%s' % rows)

# --- T-Perm: three-cut (non-member / left / removed) x (send / pull / confirm) ---
# it2x: never a member; it2q/it2rm were accepted in make_group -> leave / remove them
check('PERM_leave_ok', greq(q_, T_LEAVE, gA, b'')[0] == 0)
check('PERM_remove_ok', greq(owner_a, T_REMOVE, gA, string('it2rm'))[0] == 0)
for cn in (a, b, owner_a, rm, q_): cn.drain(0.4)
check('PERM_state_left', member_status(gA, 'it2q') == 'left', member_status(gA, 'it2q'))
check('PERM_state_removed', member_status(gA, 'it2rm') == 'removed', member_status(gA, 'it2rm'))
for label, cn in (('nonmember', x), ('left', q_), ('removed', rm)):
    s = send_msg(cn, gA, 'perm-%s' % label, 'x')
    p = pull(cn, gA, 0, 0, 10)
    k = confirm(cn, gA, 1)
    check('PERM_%s_send_rejected' % label, s['code'] == -2101, s)
    check('PERM_%s_pull_rejected' % label, p['code'] == -2101, p['code'])
    check('PERM_%s_confirm_rejected' % label, k['code'] == -2101, k)

# --- T-Auth: unauthenticated connection, group requests -> -1001 at the gate ---
raw = Conn('raw')
for req, resp in ((T_SEND, T_SEND_R), (T_PULL, T_PULL_R), (T_CONFIRM, T_CONFIRM_R), (T_UNREAD, T_UNREAD_R)):
    body = envelope(gA, string('k') + string('v')) if req == T_SEND else (bytes([0]) + u64(0) + u16(10) if req == T_PULL else (u64(1) if req == T_CONFIRM else b''))
    ps = raw.ask(pdu(req, msg=body), resp, timeout=6)
    cd = struct.unpack('<i', ps[0][2][20:24])[0] if ps and len(ps[0][2]) >= 24 else None
    check('AUTH_unauth_type%d_rejected_-1001' % req, cd == -1001, 'code=%s' % cd)
raw.close()

# --- T-Dissolve: dissolved group -> all group-message writes rejected -2102 ---
gd = make_group(owner_a, 'IT2 D', [('it2b', b)])
check('DISS_dissolved_ok', greq(owner_a, T_DISSOLVE, gd, b'')[0] == 0)
b.drain(0.4)
check('DISS_send_rejected', send_msg(owner_a, gd, 'd-1', 'x')['code'] == -2102, send_msg(owner_a, gd, 'd-1', 'x')['code'])
check('DISS_pull_rejected', pull(owner_a, gd, 0, 0, 10)['code'] == -2102)
check('DISS_confirm_rejected', confirm(owner_a, gd, 1)['code'] == -2102)
check('DISS_db_state', q1('SELECT status FROM group_info WHERE group_id=%s', (gd,))[0] == 'dissolved')

# --- T-Sys: system messages share the same seq channel ---
sysrows = qa("SELECT seq,msg_type,client_msg_id,content FROM group_message WHERE group_id=%s AND msg_type='system' ORDER BY seq", (gA,))
check('SYS_rows_exist', len(sysrows) >= 4, 'system rows=%d' % len(sysrows))
check('SYS_client_msg_id_null', all(r[2] is None for r in sysrows), [r[2] for r in sysrows])
sysseqs = {r[0] for r in sysrows}
allseqs = [r[0] for r in qa('SELECT seq FROM group_message WHERE group_id=%s ORDER BY seq', (gA,))]
check('SYS_share_seq_space', len(allseqs) == len(set(allseqs)) and max(allseqs) == last_seq(gA)[0] and sorted(allseqs) == list(range(1, max(allseqs) + 1)),
      'n=%d max=%s last_seq=%s' % (len(allseqs), max(allseqs), last_seq(gA)[0]))
ps = pull(b, gA, 0, 0, 100)
sys_in_pull = [r for r in ps['records'] if r['type'] == 1]
check('SYS_delivered_via_pull', len(sys_in_pull) == len(sysrows), '%d vs %d' % (len(sys_in_pull), len(sysrows)))

print('---- phase A done: %d/%d so far' % (sum(1 for _, o, _ in results if o), len(results)))
sys.stdout.flush()

# ================= G-CC: concurrent sequence allocation =================
gc = make_group(owner_a, 'IT2 CC', [('it2a', a), ('it2b', b), ('it2c', c_)])
for cn in (a, b, c_, owner_a): cn.drain(0.5)
base = last_seq(gc)[0]
check('CC_base_is_system_seqs', base == 3, 'base=%s (3 join system msgs)' % base)
PER = 50
senders = [a, b, c_, owner_a]
def burst(cn, tag):
    payload = b''.join(pdu(T_SEND, msg=envelope(gc, string('%s-%d' % (tag, i)) + string('m%d' % i))) for i in range(PER))
    cn.send(payload)
ths = [threading.Thread(target=burst, args=(cn, cn.name)) for cn in senders]
for t in ths: t.start()
for t in ths: t.join()
seqs_all, codes_all = [], []
for cn in senders:
    got = cn.collect(T_SEND_R, PER, timeout=45)
    rs = [p for p in got if p[0] == T_SEND_R]
    codes_all.extend([struct.unpack('<i', p[2][20:24])[0] for p in rs])
    for p in rs:
        if struct.unpack('<i', p[2][20:24])[0] == 0:
            rr = Rd(p[2][24:]); rr.string(); seqs_all.append(rr.u64())
check('CC_all_ok', len(codes_all) == PER * 4 and all(x == 0 for x in codes_all), 'n=%d bad=%s' % (len(codes_all), [x for x in codes_all if x != 0][:5]))
check('CC_no_duplicates', len(set(seqs_all)) == len(seqs_all), 'n=%d uniq=%d' % (len(seqs_all), len(set(seqs_all))))
check('CC_contiguous', sorted(seqs_all) == list(range(base + 1, base + 1 + PER * 4)),
      'min=%s max=%s n=%s expect=%s' % (min(seqs_all), max(seqs_all), len(seqs_all), PER * 4))
dbrows = q1('SELECT COUNT(*),MAX(seq),MIN(seq) FROM group_message WHERE group_id=%s', (gc,))
check('CC_db_matches', dbrows[0] == base + PER * 4 and dbrows[1] == max(seqs_all) and last_seq(gc)[0] == max(seqs_all),
      'rows=%s max=%s last_seq=%s' % (dbrows[0], dbrows[1], last_seq(gc)[0]))

# ================= G-OFF: offline catch-up + unread =================
go = make_group(owner_a, 'IT2 OFF', [('it2off', cap_login('it2off'))])
off = cap_login('it2off')
off.drain(0.4)
greq(off, T_CONFIRM, go, u64(last_seq(go)[0]))
off.close()
time.sleep(0.5)
b0 = last_seq(go)[0]
M = 7
for i in range(M):
    rr = send_msg(owner_a, go, 'off-%d' % i, 'offline-%d' % i)
    assert rr['code'] == 0, rr
check('OFF_messages_persisted', last_seq(go)[0] == b0 + M, 'last=%s base=%s' % (last_seq(go)[0], b0))
off2 = cap_login('it2off')
p = pull(off2, go, 0, last_read(go, 'it2off'), 100)
check('OFF_unread_count_equals_M', p['unread'] == M, 'unread=%s M=%s' % (p['unread'], M))
check('OFF_catchup_all_M', len(p['records']) == M and [r['content'].decode() for r in p['records']] == ['offline-%d' % i for i in range(M)],
      [r['content'] for r in p['records']])
check('OFF_has_no_more', p['has_more'] == 0 and p['last_seq'] == b0 + M, 'hm=%s lastseq=%s' % (p['has_more'], p['last_seq']))
k = confirm(off2, go, b0 + M)
check('OFF_confirm_effective', k['code'] == 0 and k['effective'] == b0 + M, k)
check('OFF_unread_zero_after', pull(off2, go, 0, b0, 100)['unread'] == 0)
us = unread_summary(off2)
check('OFF_summary_zero', us['rows'].get(go, (0, 0, 1))[2] == 0, us['rows'].get(go))

# --- read confirm monotonic clamp / no rollback / cap at last_seq ---
check('READ_no_rollback', confirm(off2, go, 1)['effective'] == b0 + M, confirm(off2, go, 1))
kk = confirm(off2, go, b0 + M + 999)
check('READ_clamped_to_last_seq', kk['code'] == 0 and kk['effective'] == b0 + M, kk)
check('READ_db_persisted', last_read(go, 'it2off') == b0 + M, last_read(go, 'it2off'))
other = confirm(owner_a, go, 2)
check('READ_does_not_affect_other_member', last_read(go, 'it2own') == (other['effective'] or -1) and last_read(go, 'it2off') == b0 + M,
      'owner_eff=%s owner_db=%s off_db=%s' % (other['effective'], last_read(go, 'it2own'), last_read(go, 'it2off')))
off2.close()

print('---- phase B done: %d/%d so far' % (sum(1 for _, o, _ in results if o), len(results)))
sys.stdout.flush()

json.dump([{'label': l, 'ok': o, 'detail': str(d)[:300]} for l, o, d in results],
          open(Path(__file__).with_name('itest_results_part1.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
print('PART1 TOTAL: %d/%d' % (sum(1 for _, o, _ in results if o), len(results)))
