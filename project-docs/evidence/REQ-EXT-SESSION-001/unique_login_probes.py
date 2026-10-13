# REQ-EXT-SESSION-001 独立测试：对抗性探针（仅报告，不入库；只走协议/DB 观察）
# 前置：候选 fc1b6c7 的 Server.exe 已在 127.0.0.1:5000 运行（独立 worktree 构建）
import hashlib, os, socket, struct, subprocess, sys, time

HOST, PORT = "127.0.0.1", 5000
HDR = 76
T_REG, T_LOGIN, T_LOGIN_R, T_FINDU, T_FINDU_R = 1, 3, 4, 5, 6
T_ONLINE, T_ONLINE_R, T_CAP, T_CAP_R = 7, 8, 44, 45
T_HB, T_HB_R, T_LOGOFF, T_LOGOFF_R = 50, 51, 48, 49
T_DOWN, T_DOWN_R = 40, 41
MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
U = "itprobe"

def f32(s): return s.encode("utf-8")[:31].ljust(32, b"\0")
def creds(n, p="123456"): return f32(n) + hashlib.md5(p.encode()).hexdigest().encode()
def pdu(t, msg=b"", dat=b""):
    dat = dat[:64].ljust(64, b"\0")
    return struct.pack("<III", HDR + len(msg), len(msg), t) + dat + msg
def sql(q):
    return subprocess.run([MYSQL] + DB + [q], capture_output=True, text=True).stdout.strip()

# 进程监护（审计修复 2026-09-30）：可选传入 Server.exe 的 PID。
# 探针失败先归因：若监护 PID 已消失，判 SERVER_DIED(3)=外部杀进程/崩溃，
# 不与产品断言混淆（审计曾出现 taskkill 按镜像名误杀探针实例导致的伪 P4 失败）。
SERVER_PID = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else None

def pid_alive(pid):
    if pid is None:
        return True
    r = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH"], capture_output=True)
    return str(pid).encode() in (r.stdout or b"")

def guard(tag):
    if not pid_alive(SERVER_PID):
        print(f"[SERVER_DIED] 监护 PID={SERVER_PID} 在阶段 '{tag}' 已不存在——"
              f"失败归因于服务器进程消失（外部按镜像名 taskkill 或崩溃），非探针断言失败")
        sys.exit(3)

def login_new(tag, cid=b""):
    guard(tag)
    c = C()
    code = cap_of(c)
    r = c.ask(pdu(T_LOGIN, msg=code + b"\0" + cid, dat=creds(U)))
    ret = struct.unpack("<i", r[0][1][:4])[0] if r else -99
    guard(tag + "-post")
    return c, ret

class C:
    def __init__(s):
        s.sock = socket.create_connection((HOST, PORT), timeout=5); s.buf=b""; s.eof=False
    def pump(s, t=0.5):
        s.sock.settimeout(t)
        try:
            d=s.sock.recv(65536)
            if d: s.buf+=d
            else: s.eof=True
        except socket.timeout: pass
        except OSError: s.eof=True
    def pkts(s):
        out=[]
        while len(s.buf)>=HDR:
            tl,ml,ty=struct.unpack("<III",s.buf[:12])
            if tl<HDR or tl!=HDR+ml or len(s.buf)<tl: break
            out.append((ty,s.buf[12:76],s.buf[76:tl])); s.buf=s.buf[tl:]
        return out
    def ask(s,p,n=1,t=3.0):
        try: s.sock.sendall(p)
        except OSError: s.eof=True
        got=[]; dl=time.time()+t
        while len(got)<n and time.time()<dl and not s.eof:
            s.pump(0.4); got=s.pkts()
        return got
    def close(s):
        try: s.sock.close()
        except OSError: pass

res=[]
def check(sid,desc,cond,detail):
    res.append(bool(cond)); print(f"[{'PASS' if cond else 'FAIL'}] {sid} {desc} :: {detail}")

def cap_of(s):
    d=s.ask(pdu(T_CAP))
    return d[0][2].split(b"\0")[0] if d else b"XXXXXX"

# 全新用户
guard("init")
sql(f"DELETE FROM login_record WHERE username='{U}'; DELETE FROM user_info WHERE name='{U}'")
c=C(); code=cap_of(c); c.ask(pdu(T_REG,msg=code+b"\0",dat=creds(U))); c.close()

# P1 超长 ID（200B）→ 服务端截断 ≤64
guard("P1"); c=C(); code=cap_of(c)
c.ask(pdu(T_LOGIN,msg=code+b"\0"+b"L"*200,dat=creds(U)))
c.close()
clen=sql(f"SELECT CHAR_LENGTH(client_id) FROM login_record WHERE username='{U}' ORDER BY id DESC LIMIT 1")
check("P1","超长 ID 截断存储 ≤64 且登录成功",sql(f"SELECT login_version FROM user_info WHERE name='{U}'")=="1" and clen and int(clen)<=64,f"len={clen}")

# P2 非打印字符 ID → 净化后仅保留可打印
guard("P2"); c=C(); code=cap_of(c)
c.ask(pdu(T_LOGIN,msg=code+b"\0"+b"\x01\x02AB\xff"+b"\x00"*25,dat=creds(U)))
c.close()
cid=sql(f"SELECT client_id FROM login_record WHERE username='{U}' ORDER BY id DESC LIMIT 1")
check("P2","非打印字节被净化（仅 AB）",cid=="AB",f"cid={cid!r}")

# P3 无 NUL 结尾验证码（6B 裸）→ 空 ID、老语义登录仍成立
guard("P3"); c=C(); code=cap_of(c)
r=c.ask(pdu(T_LOGIN,msg=code,dat=creds(U)))
ret=struct.unpack("<i",r[0][1][:4])[0] if r else -99
cid=sql(f"SELECT IFNULL(client_id,'NULL') FROM login_record WHERE username='{U}' ORDER BY id DESC LIMIT 1")
check("P3","无附加区/无NUL 兼容：登录成功、档案为空",ret==1 and cid in("NULL",""),f"ret={ret} cid={cid!r}")
s_fresh=c  # 当前权威=v3

# P4 同 socket 重复登录 → 自身存活、不踢自己（验证码有 60s 取码冷却，等待后取新码）
# 62s 窗口分段监护：外部按镜像名误杀 Server 进程会被归因为 SERVER_DIED，不再产生伪 -99
for _ in range(13):
    time.sleep(5); guard("P4-idle")
code=cap_of(s_fresh); guard("P4-recaptcha")
r=s_fresh.ask(pdu(T_LOGIN,msg=code+b"\0"+b"R"*32,dat=creds(U)))
ret=struct.unpack("<i",r[0][1][:4])[0] if r else -99
time.sleep(0.3)
r=s_fresh.ask(pdu(T_ONLINE))
alive=(not s_fresh.eof) and any(p[0]==T_ONLINE_R for p in r)
check("P4","同连接重复登录成功且不被自踢",ret==1 and alive,f"ret={ret} alive={alive}")

# P5 被踢旧连接尝试 LOGOFF（删号）→ 被切断，账号完好
guard("P5")
sA=C(); code=cap_of(sA); sA.ask(pdu(T_LOGIN,msg=code+b"\0"+b"S5A"*10+b"\0"*(2),dat=creds(U)))
sB=C(); code=cap_of(sB); sB.ask(pdu(T_LOGIN,msg=code+b"\0"+b"S5B"*10+b"\0"*(2),dat=creds(U)))
time.sleep(0.4)
r=sA.ask(pdu(T_LOGOFF,dat=creds(U)))
exists=sql(f"SELECT COUNT(*) FROM user_info WHERE name='{U}'")
check("P5","过期连接不能注销账号（切断+账号存在）",exists=="1",f"users={exists} eof={sA.eof}")

# P6 被踢旧连接下载请求 → 被切断无响应（先登录使其过期，再发下载）
guard("P6")
sC=C(); code=cap_of(sC); sC.ask(pdu(T_LOGIN,msg=code+b"\0"+b"S6C"*10+b"\0"*(2),dat=creds(U)))
sD=C(); code=cap_of(sD); sD.ask(pdu(T_LOGIN,msg=code+b"\0"+b"S6D"*10+b"\0"*(2),dat=creds(U)))
time.sleep(0.4)
r=sC.ask(pdu(T_DOWN,dat=f32("anything.bin")))
no_resp=not any(p[0]==T_DOWN_R for p in r)
check("P6","过期连接下载请求被切断且无响应",sC.eof and no_resp,f"eof={sC.eof} pkts={len(r)}")
sC.close(); sA.close(); sB.close(); sD.close()

# P7 未认证心跳仍 PONG（白名单未被版本校验吞掉）
c=C(); r=c.ask(pdu(T_HB))
check("P7","未认证心跳 PONG 正常",any(p[0]==T_HB_R for p in r),f"pkts={len(r)}")
c.close()

# P8 已认证过期连接重连后 LOGIN（新 socket）自愈：登录后功能恢复
guard("P8"); c=C(); code=cap_of(c)
r=c.ask(pdu(T_LOGIN,msg=code+b"\0"+b"S8X"*10+b"\0"*(2),dat=creds(U)))
ret=struct.unpack("<i",r[0][1][:4])[0] if r else -99
r=c.ask(pdu(T_ONLINE))
ok=ret==1 and any(p[0]==T_ONLINE_R for p in r)
check("P8","踢后重连重新登录完全自愈",ok,f"ret={ret}")
c.close()

nfail=res.count(False)
print(f"\nPROBES SUMMARY: {len(res)-nfail}/{len(res)} PASS, {nfail} FAIL")
sys.exit(0 if nfail==0 else 1)
