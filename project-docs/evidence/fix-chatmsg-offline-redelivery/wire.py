# -*- coding: utf-8 -*-
# Shared wire helpers for the REQ-EXT-GROUP-002 independent test (self-derived from
# Server/protocol.h + Server/msghandler.cpp + Client/connect.config).
import hashlib, os, socket, struct, sys, time
from pathlib import Path
import pymysql

HOST, PORT, HDR, PWD = '127.0.0.1', 5000, 76, '123456'
ROOT = Path(__file__).resolve().parents[2]
FILESYS = Path('E:/qt/xiangmu/dist/filesys')

T_REG, T_REG_R, T_LOGIN, T_LOGIN_R, T_CAP, T_CAP_R = 1, 2, 3, 4, 44, 45
T_CHAT, T_CHAT_R, T_GETCHAT, T_GETCHAT_R = 15, 16, 17, 18
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

RESULTS = []
def check(label, cond, detail=''):
    RESULTS.append((label, bool(cond), detail))
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

def last_seq(gid): return q1('SELECT last_seq FROM group_info WHERE group_id=%s', (gid,))[0]
def last_read(gid, u): return q1('SELECT last_read_seq FROM group_member WHERE group_id=%s AND user_name=%s', (gid, u))[0]
def member_status(gid, u):
    r = q1('SELECT status FROM group_member WHERE group_id=%s AND user_name=%s', (gid, u))
    return r[0] if r else None

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

def dump(path):
    import json
    json.dump([{'label': l, 'ok': o, 'detail': str(d)[:400]} for l, o, d in RESULTS],
              open(path, 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    print('TOTAL: %d/%d' % (sum(1 for _, o, _ in RESULTS if o), len(RESULTS)))
