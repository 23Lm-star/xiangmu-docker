# -*- coding: utf-8 -*-
# REQ-EXT-GROUP-002 independent test — part 3: "commit-then-broadcast" invariant by
# negative-path observation (no source injection possible in a read-only test node):
#   failure => no row, no seq consumed, no event delivered;  drop => row retained + pullable.
import sys, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from wire import *   # noqa

print('==== REQ-EXT-GROUP-002 independent test — part 3 ====')
own = cap_login('it2own'); a = cap_login('it2a')
for n in ('it2a',):
    ensure_user(n)
a = cap_login('it2a')
g = make_group(own, 'IT2 CTB', [('it2a', a)])
own.drain(0.4); a.drain(0.4)
base = last_seq(g)

ok = send_msg(own, g, 'ctb-ok', 'committed')
check('CTB_baseline_send_ok', ok['code'] == 0 and ok['seq'] == base + 1, ok)
ev = events(a.drain(0.7), g)
check('CTB_baseline_broadcast', len([e for e in ev if e['seq'] == ok['seq']]) == 1, ev)
ls1 = last_seq(g)

# 1) content validation failure (fails before the transaction) => no row / no seq / no event
bad = send_msg(own, g, 'ctb-bad', '')
check('CTB_invalid_content_rejected', bad['code'] == -2103, bad)
time.sleep(0.4)
ev = events(a.drain(0.6), g)
check('CTB_failure_no_event', len(ev) == 0, 'events=%d' % len(ev))
check('CTB_failure_no_row', q1("SELECT COUNT(*) FROM group_message WHERE group_id=%s AND client_msg_id='ctb-bad'", (g,))[0] == 0)
check('CTB_failure_no_seq_consumed', last_seq(g) == ls1, 'last_seq=%s was=%s' % (last_seq(g), ls1))

# 2) non-member send rejected => no row, no event to real members
nm = send_msg(a, g, 'ctb-nm', 'x') if False else None
x = cap_login('it2x')
nm = send_msg(x, g, 'ctb-nm', 'x')
check('CTB_nonmember_rejected', nm['code'] == -2101, nm)
time.sleep(0.4)
ev = events(a.drain(0.6), g)
check('CTB_nonmember_no_event', len(ev) == 0, 'events=%d' % len(ev))
check('CTB_nonmember_no_row', q1("SELECT COUNT(*) FROM group_message WHERE group_id=%s AND client_msg_id='ctb-nm'", (g,))[0] == 0)
check('CTB_seq_unbroken_after_rejections', last_seq(g) == ls1, 'last_seq=%s' % last_seq(g))

# 3) next successful send continues the sequence with no gap
nxt = send_msg(a, g, 'ctb-next', 'after-rejects')
check('CTB_seq_contiguous_after_failures', nxt['code'] == 0 and nxt['seq'] == ls1 + 1, nxt)
dbseqs = [r[0] for r in qa('SELECT seq FROM group_message WHERE group_id=%s ORDER BY seq', (g,))]
check('CTB_db_seqs_dense', dbseqs == list(range(1, max(dbseqs) + 1)) and last_seq(g) == max(dbseqs), dbseqs)

# 4) broadcast-drop path (part 2) retains the message: every seq the stalled reader missed
#    must still exist as a row and be reachable by cursor pull.
gs_row = q1("SELECT group_id,last_seq FROM group_info WHERE group_name='IT2 SLOW' ORDER BY group_id DESC LIMIT 1")
if gs_row:
    gid, lseq = gs_row
    rows = q1('SELECT COUNT(*) FROM group_message WHERE group_id=%s', (gid,))[0]
    check('CTB_dropped_broadcasts_still_persisted', rows == lseq, 'rows=%s last_seq=%s' % (rows, lseq))
else:
    check('CTB_dropped_broadcasts_still_persisted', False, 'IT2 SLOW group not found')

x.close()
dump(Path(__file__).with_name('itest_results_part3.json'))
