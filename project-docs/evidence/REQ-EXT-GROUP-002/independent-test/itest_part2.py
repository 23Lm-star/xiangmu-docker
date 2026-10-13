# -*- coding: utf-8 -*-
# REQ-EXT-GROUP-002 independent test — part 2: pagination (800/800), payload validation,
# bounded broadcast under a stalled reader, and regression (1:1 online/offline, publish link).
import os, re, struct, sys, threading, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from wire import *   # noqa

LOG = ROOT / 'dist' / 'server-console.log'
print('==== REQ-EXT-GROUP-002 independent test — part 2 ====')
sys.stdout.flush()

# reset regression fixtures so a re-run starts from a clean, deterministic state
with db() as _c, _c.cursor() as _cur:
    _cur.execute("DELETE FROM message_info WHERE seed_name LIKE 'it2c%%' OR rev_name LIKE 'it2c%%'")
    _cur.execute("DELETE FROM share_link WHERE owner_name LIKE 'it2c%%'")

own = cap_login('it2own')
for n in ('it2fast', 'it2slow', 'it2c1', 'it2c2'):
    ensure_user(n)
fast = cap_login('it2fast'); slow = cap_login('it2slow')
c1 = cap_login('it2c1'); c2 = cap_login('it2c2')

# ======================= T-PG: pagination 800/800 =======================
gp = make_group(own, 'IT2 PG', [])
N = 800
t0 = time.time()
for base_i in range(0, N, 50):
    batch = b''.join(pdu(T_SEND, msg=envelope(gp, string('pg-%d' % i) + string('m-%d' % i))) for i in range(base_i, base_i + 50))
    own.send(batch)
    got = own.collect(T_SEND_R, 50, timeout=30)
    codes = [struct.unpack('<i', p[2][20:24])[0] for p in got if p[0] == T_SEND_R]
    if len(codes) != 50 or any(c != 0 for c in codes):
        check('PG_seed_batch_%d' % base_i, False, codes[:5]); break
own.drain(0.5)
check('PG_seeded_800', last_seq(gp) == N, 'last_seq=%s' % last_seq(gp))

def page_forward(limit):
    cur, acc, pages, ok = 0, [], 0, True
    while True:
        p = pull(own, gp, 0, cur, limit)
        if p['code'] != 0 or not p.get('trailing_ok', False):
            return acc, pages, 'code=%s trailing=%s' % (p['code'], p.get('trailing_ok'))
        recs = p['records']; acc += [r['seq'] for r in recs]; pages += 1
        if recs and sorted(x['seq'] for x in recs) != [r['seq'] for r in recs]:
            ok = False
        if recs and p['next_cursor'] != recs[-1]['seq']:
            return acc, pages, 'next_cursor=%s boundary=%s' % (p['next_cursor'], recs[-1]['seq'])
        if len(recs) > limit:
            return acc, pages, 'page_exceeds_limit %d>%d' % (len(recs), limit)
        if not p['has_more']:
            break
        if p['next_cursor'] == cur:
            return acc, pages, 'cursor_stuck'
        cur = p['next_cursor']
        if pages > 900: return acc, pages, 'page_guard'
    return acc, pages, 'ok' if ok else 'not_ascending'

acc, pages, why = page_forward(37)
check('PG_mode0_800_no_missing_no_dup', acc == list(range(1, N + 1)), 'n=%d pages=%d why=%s' % (len(acc), pages, why))
check('PG_mode0_pages_expected', pages == 22, 'pages=%d (ceil(800/37)=22)' % pages)

acc1, pages1, why1 = page_forward(1)
check('PG_mode0_limit1_full', acc1 == list(range(1, N + 1)), 'n=%d pages=%d why=%s' % (len(acc1), pages1, why1))

# mode1 backward: client contract is cursor = next_cursor - 1 (Client/groupchat.cpp loadOlder)
def page_backward(limit):
    cur, acc, pages = N, [], 0
    while True:
        p = pull(own, gp, 1, cur, limit)
        if p['code'] != 0: return acc, pages, 'code=%s' % p['code']
        recs = p['records']; acc = [r['seq'] for r in recs] + acc; pages += 1
        if recs and p['next_cursor'] != recs[0]['seq']:
            return acc, pages, 'next_cursor=%s boundary=%s' % (p['next_cursor'], recs[0]['seq'])
        if not p['has_more']:
            break
        cur = p['next_cursor'] - 1
        if cur <= 0: break
        if pages > 900: return acc, pages, 'page_guard'
    return acc, pages, 'ok'

accb, pagesb, whyb = page_backward(37)
check('PG_mode1_800_no_missing_no_dup', accb == list(range(1, N + 1)), 'n=%d pages=%d why=%s' % (len(accb), pagesb, whyb))

# naive mode1 (feed next_cursor unchanged) — documents the inclusive-upper-bound contract
p1 = pull(own, gp, 1, N, 10)
p2 = pull(own, gp, 1, p1['next_cursor'], 10)
overlap = set(r['seq'] for r in p1['records']) & set(r['seq'] for r in p2['records'])
check('PG_mode1_boundary_is_inclusive', overlap == {p1['next_cursor']}, 'overlap=%s next_cursor=%s' % (sorted(overlap), p1['next_cursor']))

# --- boundary / clamping / rejection ---
p = pull(own, gp, 0, N, 10)
check('PG_mode0_at_watermark_empty', p['code'] == 0 and p['records'] == [] and p['has_more'] == 0, p)
p = pull(own, gp, 0, 0, 1)
check('PG_mode0_limit1_first', [r['seq'] for r in p['records']] == [1] and p['has_more'] == 1 and p['next_cursor'] == 1, p['records'])
p = pull(own, gp, 1, 1, 10)
check('PG_mode1_at_first', [r['seq'] for r in p['records']] == [1] and p['has_more'] == 0, p['records'])
p = pull(own, gp, 1, 0, 10)
check('PG_mode1_cursor0_empty', p['code'] == 0 and p['records'] == [], p)
check('PG_limit0_rejected', pull(own, gp, 0, 0, 0)['code'] == -2002)
check('PG_limit101_rejected', pull(own, gp, 0, 0, 101)['code'] == -2002)
check('PG_limit100_ok', pull(own, gp, 0, 0, 100)['code'] == 0 and len(pull(own, gp, 0, 0, 100)['records']) == 100)
check('PG_mode2_rejected', pull(own, gp, 2, 0, 10)['code'] == -2002)
# trailing garbage in pull payload -> -2002
code, _ = greq(own, T_PULL, gp, bytes([0]) + u64(0) + u16(10) + b'xx')
check('PG_trailing_bytes_rejected', code == -2002, 'code=%s' % code)
# unread correctness on this group (owner never confirmed -> unread == 800)
p = pull(own, gp, 0, 0, 10)
check('PG_unread_800', p['unread'] == N, 'unread=%s' % p['unread'])

# --- payload validation on send ---
check('VAL_empty_content_rejected', send_msg(own, gp, 'v-1', '')['code'] == -2103, send_msg(own, gp, 'v-2', '')['code'])
check('VAL_empty_key_rejected', send_msg(own, gp, '', 'x')['code'] == -2103)
big = send_msg(own, gp, 'v-3', 'a' * 2001)
check('VAL_content_2001_rejected', big['code'] == -2002, big)
ok2000 = send_msg(own, gp, 'v-4', 'a' * 2000)
check('VAL_content_2000_ok', ok2000['code'] == 0, ok2000.get('code'))
check('VAL_key_65_rejected', send_msg(own, gp, 'k' * 65, 'x')['code'] == -2002)
dup = send_msg(own, gp, 'v-4', 'a' * 2000)
check('VAL_2000b_key_replay_idempotent', dup['code'] == 0 and dup['seq'] == ok2000['seq'], dup)

own.drain(0.5)
print('---- pagination/validation done: %d/%d' % (sum(1 for _, o, _ in RESULTS if o), len(RESULTS)))
sys.stdout.flush()

# ======================= T-BCAST: bounded broadcast =======================
gs = make_group(own, 'IT2 SLOW', [('it2fast', fast), ('it2slow', slow)])
own.drain(0.4); fast.drain(0.4); slow.drain(0.4)
offset = LOG.stat().st_size if LOG.exists() else 0

fast_seen = []
stop = threading.Event()
def fast_reader():
    while not stop.is_set():
        try:
            fast.pump(.15)
        except ConnectionError:
            break
        for e in events(fast.packets(), gs):
            fast_seen.append(e['seq'])
th = threading.Thread(target=fast_reader, daemon=True); th.start()

BQ = 800
CHUNK = 'x' * 1500
for bi in range(0, BQ, 25):
    batch = b''.join(pdu(T_SEND, msg=envelope(gs, string('sl-%d' % i) + string(CHUNK))) for i in range(bi, bi + 25))
    own.send(batch)
    got = own.collect(T_SEND_R, 25, timeout=30)
    if len([p for p in got if p[0] == T_SEND_R]) != 25:
        check('BC_bypass_sender_pipeline', False, 'lost responses at batch %d' % bi); break
time.sleep(1.5)
stop.set(); th.join(timeout=3)
fast.drain(0.5)
fast_count = len(set(fast_seen))
check('BC_fast_member_unaffected', fast_count == BQ, 'fast received %d/%d' % (fast_count, BQ))

tail = b''
with open(LOG, 'rb') as f:
    f.seek(offset)
    tail = f.read()
m = re.search(rb'\[GROUP\][^\r\n]*?"?it2slow"?[^\r\n]*?drops=\s*(\d+)', tail)
m2 = re.search(rb'\[GROUP\][^\r\n]*?drops=\s*(\d+)', tail)
check('BC_slow_conn_drop_logged', bool(m2), 'match=%s' % (m.group(0)[:120] if m else (m2.group(0)[:120] if m2 else b'')))
check('BC_slow_is_the_dropped_conn', bool(m) and int(m.group(1)) > 0, 'slow_drops=%s' % (m.group(1) if m else None,))

# slow connection now drains what it did receive
got = slow.drain(3.0)
slow_seqs = [e['seq'] for e in events(got, gs)]
check('BC_slow_realtime_partial', len(set(slow_seqs)) < BQ, 'slow realtime=%d/%d' % (len(set(slow_seqs)), BQ))

# backfill by cursor pagination on the slow connection (3rd: pull is the compensating path)
cur, pulled = 0, []
while True:
    p = pull(slow, gs, 0, cur, 100)
    if p['code'] != 0: break
    pulled += [r['seq'] for r in p['records']]
    if not p['has_more']: break
    cur = p['next_cursor']
check('BC_slow_pull_backfill_full', sorted(pulled) == list(range(1, last_seq(gs) + 1)),
      'pulled=%d uniq=%d last_seq=%s (expect 800 user + 2 system)' % (len(pulled), len(set(pulled)), last_seq(gs)))

print('---- broadcast done: %d/%d' % (sum(1 for _, o, _ in RESULTS if o), len(RESULTS)))
sys.stdout.flush()

# ======================= T-REG: 1:1 chat + publish link =======================
def chat_pkt(seed, rev, text):
    msg = text.encode() + b'\0'
    ml = len(msg)
    return pdu(T_CHAT, msg=struct.pack('<II', 38 + ml, ml) + b'2026-10-01 12:00:00'.ljust(30, b'\0') + msg, dat=f32(seed) + f32(rev))

c1.drain(0.4); c2.drain(0.4)
c1.send(chat_pkt('it2c1', 'it2c2', 'reg-online-1'))
got = c2.collect(T_CHAT, 1, timeout=8)
fwd = [p for p in got if p[0] == T_CHAT]
check('REG_chat_online_forwarded', bool(fwd), 'packets=%s' % [p[0] for p in got])
if fwd:
    mlen = struct.unpack('<I', fwd[0][2][4:8])[0]
    check('REG_chat_online_content', fwd[0][2][38:38 + mlen - 1] == b'reg-online-1', fwd[0][2][38:38 + mlen])
row = q1("SELECT issuccess FROM message_info WHERE seed_name='it2c1' AND rev_name='it2c2' AND msg='reg-online-1' ORDER BY seed_time DESC LIMIT 1")
check('REG_chat_online_persisted', row is not None and row[0] == 1, row)

# offline: it2c3 exists but is not connected
c1.send(chat_pkt('it2c1', 'it2c3', 'reg-offline-1'))
time.sleep(0.8)
row = q1("SELECT issuccess FROM message_info WHERE seed_name='it2c1' AND rev_name='it2c3' AND msg='reg-offline-1'")
check('REG_chat_offline_stored_undelivered', row is not None and row[0] == 0, row)
c3 = cap_login('it2c3')
# chatmsg: caData[0:32] is force-overwritten with the actor (self); the peer must go in caData[32:64]
got = c3.ask(pdu(T_GETCHAT, dat=f32('it2c3') + f32('it2c1')), T_GETCHAT_R, timeout=8)
recs, off = [], 0
payload = got[0][2] if got and got[0][0] == T_GETCHAT_R else b''
while off + 38 <= len(payload):
    tl, ml = struct.unpack_from('<II', payload, off)
    if tl < 38 or off + tl > len(payload): break
    recs.append(payload[off + 38:off + 38 + ml - 1]); off += tl
check('REG_chat_offline_catchup', any(r == b'reg-offline-1' for r in recs), recs)

# PRE-EXISTING BASELINE FINDING (not introduced by this candidate — getchatMsg is
# byte-identical on master, sha256 690cbe79...): getchatMsg never flips issuccess to 1,
# so pending offline messages are re-delivered on EVERY call.
got2 = c3.ask(pdu(T_GETCHAT, dat=f32('it2c3') + f32('it2c1')), T_GETCHAT_R, timeout=8)
payload2 = got2[0][2] if got2 and got2[0][0] == T_GETCHAT_R else b''
redelivered = b'reg-offline-1' in payload2
row = q1("SELECT issuccess FROM message_info WHERE seed_name='it2c1' AND rev_name='it2c3' AND msg='reg-offline-1'")
check('FINDING-PRE1_offline_msg_redelivered_and_flag_stuck',
      not (redelivered and row is not None and row[0] == 0),
      'second getchatmsg re-delivered=%s db_issuccess=%s (expected flag=1, no redelivery)' % (redelivered, row))
c3.close()

# publish link regression (create / dedup / resolve to a second account)
d1 = FILESYS / 'it2c1'; d2 = FILESYS / 'it2c2'
d1.mkdir(parents=True, exist_ok=True); d2.mkdir(parents=True, exist_ok=True)
(d1 / 'it2-pub.bin').write_bytes(b'REQ-EXT-GROUP-002 regression payload')
r = c1.ask(pdu(T_CPL, msg=b'it2-pub.bin'), T_CPL_R, timeout=8)
link1 = r[0][2].split(b'\0')[0].decode() if r and r[0][0] == T_CPL_R else ''
r = c1.ask(pdu(T_CPL, msg=b'it2-pub.bin'), T_CPL_R, timeout=8)
link2 = r[0][2].split(b'\0')[0].decode() if r and r[0][0] == T_CPL_R else ''
check('REG_publink_created', link1.startswith('cloud-disk://share/'), link1[:50])
# dedup contract: 2nd create for same owner+path returns the stored 64-hex hash form (resolvable),
# not the identical token string — both must resolve (BUG-ARC-PUBLINK-001 F-1 fix).
check('REG_publink_dedup_hash_form', len(link2.split('/')[-1]) == 64 and all(ch in '0123456789abcdef' for ch in link2.split('/')[-1]),
      'l1_len=%d l2_len=%d' % (len(link1.split('/')[-1]), len(link2.split('/')[-1])))
r1 = c2.ask(pdu(T_RES, msg=link1.split('cloud-disk://share/')[-1].encode()), T_RES_R, timeout=10)
ok1 = r1[0][1][0] != 0 if r1 and r1[0][0] == T_RES_R else False
tok = link2.split('cloud-disk://share/')[-1]
rs = c2.ask(pdu(T_RES, msg=tok.encode()), T_RES_R, timeout=10)
okd = rs[0][1][0] != 0 if rs and rs[0][0] == T_RES_R else False
landed = sorted(os.listdir(d2 / '分享链接')) if (d2 / '分享链接').is_dir() else []
check('REG_publink_resolves', okd and ok1, 'token_ok=%s hash_ok=%s' % (ok1, okd))
check('REG_publink_lands_for_receiver', 'it2-pub.bin' in landed, landed)

for cn in (own, fast, slow, c1, c2):
    cn.close()

dump(Path(__file__).with_name('itest_results_part2.json'))
