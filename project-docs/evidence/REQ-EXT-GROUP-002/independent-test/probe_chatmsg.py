import sys, struct, time
from pathlib import Path
sys.path.insert(0, str(Path(__file__).parent))
from wire import *

with db() as c, c.cursor() as cur:
    cur.execute("DELETE FROM message_info WHERE seed_name LIKE 'it2p%' OR rev_name LIKE 'it2p%'")
for n in ('it2ps', 'it2pr'):
    ensure_user(n)
c1 = cap_login('it2ps')
def chat_pkt(seed, rev, text):
    msg = text.encode() + b'\0'; ml = len(msg)
    return pdu(T_CHAT, msg=struct.pack('<II', 38 + ml, ml) + b't'.ljust(30, b'\0') + msg, dat=f32(seed) + f32(rev))
c1.send(chat_pkt('it2ps', 'it2pr', 'probe-1'))
time.sleep(0.7)
print('after send issuccess =', q1("SELECT issuccess FROM message_info WHERE seed_name='it2ps' AND rev_name='it2pr' AND msg='probe-1'"))
c2 = cap_login('it2pr')
for i in (1, 2):
    got = c2.ask(pdu(T_GETCHAT, dat=f32('it2pr') + f32('it2ps')), T_GETCHAT_R, timeout=6)
    payload = got[0][2] if got and got[0][0] == T_GETCHAT_R else b''
    print('call %d payload_len=%d raw=%r' % (i, len(payload), payload[:80]))
    time.sleep(0.4)
    print('   issuccess =', q1("SELECT issuccess FROM message_info WHERE seed_name='it2ps' AND rev_name='it2pr' AND msg='probe-1'"))
c1.close(); c2.close()
