# -*- coding: utf-8 -*-
# FINDING-PRE1 probe: 1:1 offline chat redelivery + message_info.issuccess flip.
# Reproduces: user A sends a chat to OFFLINE user B; B then pulls twice.
#   BEFORE fix: both pulls return the payload, issuccess stays 0.
#   AFTER  fix: pull 1 returns the payload and flips issuccess to 1, pull 2 returns nothing.
# Run: py -3.13 probe_chatmsg_fix.py
import sys, struct, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import wire
# wire.ROOT must point at a checkout whose Server/connect.config exists (git-ignored,
# synced by pipeline/build.ps1). Point it at the live worktree under test.
wire.ROOT = Path(r'E:/qt/xiangmu/.claude/worktrees/agent-a4fe8a2a')
from wire import db, ensure_user, cap_login, pdu, f32, q1, T_CHAT, T_GETCHAT, T_GETCHAT_R

SEL = ("SELECT issuccess FROM message_info "
       "WHERE seed_name='it2ps' AND rev_name='it2pr' AND msg='probe-1'")

with db() as c, c.cursor() as cur:
    cur.execute("DELETE FROM message_info WHERE seed_name LIKE 'it2p%' OR rev_name LIKE 'it2p%'")
for n in ('it2ps', 'it2pr'):
    ensure_user(n)

c1 = cap_login('it2ps')

def chat_pkt(seed, rev, text):
    msg = text.encode() + b'\0'; ml = len(msg)
    return pdu(T_CHAT,
               msg=struct.pack('<II', 38 + ml, ml) + b't'.ljust(30, b'\0') + msg,
               dat=f32(seed) + f32(rev))

c1.send(chat_pkt('it2ps', 'it2pr', 'probe-1'))
time.sleep(0.7)
print('after send issuccess =', q1(SEL))

c2 = cap_login('it2pr')
for i in (1, 2):
    got = c2.ask(pdu(T_GETCHAT, dat=f32('it2pr') + f32('it2ps')), T_GETCHAT_R, timeout=6)
    payload = got[0][2] if got and got[0][0] == T_GETCHAT_R else b''
    print('call %d payload_len=%d raw=%r' % (i, len(payload), payload[:80]))
    time.sleep(0.4)
    print('   issuccess =', q1(SEL))

c1.close(); c2.close()
