import hashlib, socket, struct, time, threading
from pathlib import Path
import pymysql

HOST, PORT = '127.0.0.1', 5000
HDR = 76
PWD = '123456'
T_REG, T_REG_R, T_LOGIN, T_LOGIN_R = 1, 2, 3, 4
T_CAP, T_CAP_R = 44, 45
T_CREATE, T_CREATE_R = 60, 61
T_INVITE, T_INVITE_R = 62, 63
T_ACCEPT, T_ACCEPT_R = 64, 65
T_REJECT, T_REJECT_R = 66, 67
T_LEAVE, T_LEAVE_R = 68, 69
T_REMOVE, T_REMOVE_R = 70, 71
T_SETROLE, T_SETROLE_R = 72, 73
T_TRANSFER, T_TRANSFER_R = 74, 75
T_DISSOLVE, T_DISSOLVE_R = 76, 77
T_LIST, T_LIST_R = 78, 79
T_MEMBERS, T_MEMBERS_R = 80, 81
T_INVITES, T_INVITES_R = 82, 83
T_SUMMARY, T_SUMMARY_R = 86, 87

def f32(s): return s.encode()[:31].ljust(32, b'\0')
def pdu(t, msg=b'', dat=b''):
    return struct.pack('<III', HDR + len(msg), len(msg), t) + dat[:64].ljust(64,b'\0') + msg
def u16(x): return struct.pack('<H',x)
def u32(x): return struct.pack('<I',x)
def u64(x): return struct.pack('<Q',x)
def string(x):
    b=x.encode() if isinstance(x,str) else x
    return u32(len(b))+b
def envelope(gid=0, body=b'', req=None):
    return u16(1)+u16(20)+u64(req or int(time.time_ns() & ((1<<63)-1)))+u64(gid)+body

def unpack_packets(buf):
    out=[]
    while len(buf)>=HDR:
        tl,ml,ty=struct.unpack('<III',buf[:12])
        if tl < HDR or tl != HDR+ml or len(buf)<tl: break
        out.append((ty,buf[12:76],buf[76:tl])); buf=buf[tl:]
    return out,buf
class Conn:
    def __init__(self,name=None):
        self.s=socket.create_connection((HOST,PORT),timeout=8); self.buf=b''; self.name=name
    def pump(self,timeout=.5):
        self.s.settimeout(timeout)
        try: self.buf += self.s.recv(65536)
        except socket.timeout: pass
    def ask(self,pkt,expected=None,timeout=5):
        self.s.sendall(pkt); end=time.time()+timeout; got=[]
        while time.time()<end:
            self.pump(.3); ps,self.buf=unpack_packets(self.buf); got.extend(ps)
            if expected is None or any(x[0]==expected for x in got): return got
        return got
    def close(self):
        try:self.s.close()
        except:pass

def check(label, cond, detail=''):
    print(('[PASS] ' if cond else '[FAIL] ')+label+(' :: '+detail if detail else ''))
    results.append((label,bool(cond),detail))

def cap_login(name):
    c=Conn(name); r=c.ask(pdu(T_CAP),T_CAP_R); code=r[0][2].split(b'\0')[0] if r else b''
    r=c.ask(pdu(T_LOGIN,msg=code+b'\0',dat=f32(name)+hashlib.md5(PWD.encode()).hexdigest().encode()),T_LOGIN_R)
    ret=struct.unpack('<i',r[0][1][:4])[0] if r else -99
    if ret!=1: c.close(); raise RuntimeError(f'login {name} ret={ret}')
    return c

def ensure_user(name):
    c=Conn(name); r=c.ask(pdu(T_CAP),T_CAP_R); code=r[0][2].split(b'\0')[0] if r else b''
    r=c.ask(pdu(T_REG,msg=code+b'\0',dat=f32(name)+hashlib.md5(PWD.encode()).hexdigest().encode()),T_REG_R)
    c.close()
    try: cap_login(name).close()
    except RuntimeError as e: raise RuntimeError(f'ensure {name}: {e}')

def group(c, typ, gid=0, body=b'', expected=None):
    ps=c.ask(pdu(typ,msg=envelope(gid,body)),expected or typ+1)
    for x in ps:
        if x[0]==(expected or typ+1):
            d=x[2]; code=struct.unpack('<I',d[20:24])[0] if len(d)>=24 else -999
            return code,d
    return -998,b''

def response_body(d): return d[24:]
def parse_u64(d, off=0): return struct.unpack('<Q',d[off:off+8])[0]

def db_conn():
    lines=Path('Server/connect.config').read_text(encoding='utf-8').splitlines()
    return pymysql.connect(host=lines[3],port=int(lines[4]),user=lines[6],password=lines[7],database=lines[5],autocommit=True)

results=[]
for n in ('grpowner','grpadmin','grpuser','grptarget','grpoutsider'):
    try: ensure_user(n)
    except Exception as e: print('[SETUP]',n,e); raise
# isolate this run's group rows
conn=db_conn()
with conn.cursor() as cur:
    cur.execute("DELETE FROM group_invitation WHERE group_id IN (SELECT group_id FROM group_info WHERE creator_name LIKE 'grp%')")
    cur.execute("DELETE FROM group_member WHERE user_name LIKE 'grp%'")
    cur.execute("DELETE FROM group_info WHERE creator_name LIKE 'grp%'")
conn.close()
owner=cap_login('grpowner'); admin=cap_login('grpadmin'); user=cap_login('grpuser'); target=cap_login('grptarget'); outsider=cap_login('grpoutsider')
# old business login regression is covered by all five successful logins.
check('old_login_regression', True, 'five users authenticated')
code,d=group(owner,T_CREATE,body=string('Independent Group')); gid=parse_u64(response_body(d)) if code==0 and len(response_body(d))>=8 else 0
check('create_group',code==0 and gid>0,f'code={code} gid={gid}')
code,d1=group(owner,T_INVITE,gid,string('grptarget')+string('idem-1')); iid=parse_u64(response_body(d1)) if code==0 and len(response_body(d1))>=8 else 0
code2,d2=group(owner,T_INVITE,gid,string('grptarget')+string('idem-1'))
iid2=parse_u64(response_body(d2)) if code2==0 and len(response_body(d2))>=8 else 0
check('invite_idempotent',code==0 and code2==0 and iid==iid2,f'first={code}/{iid} repeat={code2}/{iid2}')
code,d=group(target,T_ACCEPT,0,u64(iid)); check('accept_invitation',code==0,f'code={code}')
code,d=group(owner,T_MEMBERS,gid); check('member_list_authorized',code==0,f'code={code}')
code,d=group(outsider,T_MEMBERS,gid); check('member_list_unauthorized',code!=0,f'code={code}')
code,d=group(owner,T_SETROLE,gid,string('grptarget')+string('admin')); check('owner_promote_admin',code==0,f'code={code}')
code,d=group(admin,T_REMOVE,gid,string('grpowner')); check('admin_cannot_remove_owner',code!=0,f'code={code}')
code,d=group(owner,T_TRANSFER,gid,string('grptarget')); check('owner_transfer',code==0,f'code={code}')
# create a second invitation, then replay two accepts on one authenticated connection
code,d=group(target,T_INVITE,gid,string('grpuser')+string('race-1')); iid2=parse_u64(response_body(d)) if code==0 and len(response_body(d))>=8 else 0
race_conn=cap_login('grpuser'); pkt=pdu(T_ACCEPT,msg=envelope(0,u64(iid2)))
race_conn.s.sendall(pkt+pkt); end=time.time()+5; received=[]
while time.time()<end and len([x for x in received if x[0]==T_ACCEPT_R])<2:
    race_conn.pump(.3); ps,race_conn.buf=unpack_packets(race_conn.buf); received.extend(ps)
race=[struct.unpack('<I',x[2][20:24])[0] for x in received if x[0]==T_ACCEPT_R]; race_conn.close()
check('concurrent_accept_unique',len(race)==2 and sorted(race)==[0,4294966277],f'results={race}')
# old owner is now admin and must be denied; new owner can dissolve
code,d=group(owner,T_DISSOLVE,gid); check('non_owner_cannot_dissolve',code!=0,f'code={code}')
code,d=group(target,T_INVITE,gid,string('grpadmin')+string('after-dissolve')); iid3=parse_u64(response_body(d)) if code==0 and len(response_body(d))>=8 else 0
code,d=group(target,T_DISSOLVE,gid); check('owner_dissolve',code==0,f'code={code}')
code,d=group(admin,T_REJECT,0,u64(iid3)); check('dissolved_reject_write_gate',code!=0,f'code={code} expected_nonzero')
with db_conn() as c:
    with c.cursor() as cur:
        cur.execute('SELECT status FROM group_info WHERE group_id=%s',(gid,)); gs=cur.fetchone()[0]
        cur.execute('SELECT COUNT(*) FROM group_member WHERE group_id=%s AND status=\'active\'',(gid,)); active=cur.fetchone()[0]
        cur.execute('SELECT status FROM group_invitation WHERE invitation_id=%s',(iid3,)); inv=cur.fetchone()[0]
check('db_dissolved_state',gs=='dissolved',f'group_status={gs}')
check('db_no_duplicate_active_members',active==3,f'active_members={active}')
print('dissolved_invitation_status='+str(inv))
for c in (owner,admin,user,target,outsider): c.close()
print(f'RESULT {sum(x[1] for x in results)}/{len(results)} PASS')
raise SystemExit(0 if all(x[1] for x in results) else 1)
