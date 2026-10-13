# T3 收口证据：修复前/后 LIKE '/filesys/%' 计数 + 幂等重放（同一夹具，同一库）
# 用法：py -3.13 build/evidence/small-t3/probe_count.py <label>
import os, socket, subprocess, sys, time

HOST, PORT = "127.0.0.1", 5000
MYSQL = r"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe"
DB = ["-h", "localhost", "-u", "root", "-p123456", "mydb", "-N", "-B", "-e"]
REPO = r"E:/qt/xiangmu"
DIST = os.path.join(REPO, "dist")
EXE = os.path.join(DIST, "Server.exe")
U = "probe3user"
LABEL = sys.argv[1] if len(sys.argv) > 1 else "?"

def sql(q):
    return subprocess.run([MYSQL] + DB + [q], capture_output=True, text=True).stdout.strip()

def start():
    os.environ["PATH"] = r"C:/Qt/5.14.2/mingw73_64/bin;C:/Qt/Tools/mingw730_64/bin;" + os.environ["PATH"]
    subprocess.run(["taskkill", "/F", "/IM", "Server.exe"], capture_output=True)
    time.sleep(1.0)
    subprocess.Popen([EXE], cwd=DIST, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    dl = time.time() + 25
    while time.time() < dl:
        try:
            socket.create_connection((HOST, PORT), timeout=1).close(); time.sleep(1.5); return True
        except OSError: time.sleep(0.4)
    return False

def stop():
    subprocess.run(["taskkill", "/F", "/IM", "Server.exe"], capture_output=True); time.sleep(1.0)

def counts(tag):
    uf = sql(f"SELECT COUNT(*) FROM user_file WHERE parent_path LIKE '/filesys/%'")
    ufrow = sql(f"SELECT COUNT(*) FROM user_file WHERE owner_name='{U}' AND display_name='pf.txt' "
                f"AND parent_path='/sub/'")
    ss = sql(f"SELECT COUNT(*) FROM upload_session WHERE owner_name='{U}' AND parent_path LIKE '/filesys/%'")
    ssrow = sql(f"SELECT COUNT(*) FROM upload_session WHERE owner_name='{U}' AND display_name='ps.txt' "
                f"AND parent_path='/sub/'")
    print(f"[{LABEL}/{tag}] LIKE '/filesys/%' user_file={uf} upload_session={ss} | "
          f"pf.txt@'/sub/'={ufrow} ps.txt@'/sub/'={ssrow}")

stop()
sql(f"DELETE FROM user_file WHERE owner_name='{U}'; DELETE FROM upload_session WHERE owner_name='{U}';")
ins = (f"INSERT INTO user_file (owner_name,parent_path,display_name,is_dir,object_id,status) "
       f"VALUES ('{U}','/filesys/{U}/sub/','pf.txt',0,NULL,'active'); "
       f"INSERT INTO upload_session (owner_name,parent_path,display_name,expected_size,staging_path,status,expires_at) "
       f"VALUES ('{U}','/filesys/{U}/sub/','ps.txt',10,'x','failed',NOW());")
subprocess.run([MYSQL] + DB + [ins], capture_output=True)
counts("seeded")
print(f"[{LABEL}/boot1] started={start()}"); counts("boot1")
print(f"[{LABEL}/boot2] started={start()}"); counts("boot2(replay)")
stop()
sql(f"DELETE FROM user_file WHERE owner_name='{U}'; DELETE FROM upload_session WHERE owner_name='{U}';")
