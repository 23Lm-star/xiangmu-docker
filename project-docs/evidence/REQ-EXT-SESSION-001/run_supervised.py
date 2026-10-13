# REQ-EXT-SESSION-001 监督运行器（审计修复 2026-09-30）：
# 启动指定树的 Server.exe 并记录 PID → 带 PID 监护跑对抗探针 → 只按 PID 清理。
# 全程不按镜像名 taskkill，避免审计伪失败（外部实例互杀归因不清）。
# 用法：py -3.13 run_supervised.py <tree_root>   # tree_root 含 build/build-Server/release/Server.exe 与 Server/
import os, socket, subprocess, sys, time

tree = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
EXE = os.path.join(tree, "build", "build-Server", "release", "Server.exe")
SRV = os.path.join(tree, "Server")
PROBES = os.path.join(os.path.dirname(os.path.abspath(__file__)), "unique_login_probes.py")
HOST, PORT = "127.0.0.1", 5000

if not os.path.isfile(EXE):
    print(f"[E] missing {EXE}"); sys.exit(2)
try:
    socket.create_connection((HOST, PORT), timeout=1).close()
    print("[E] 端口 5000 已被占用：先按 PID 停掉现有 Server.exe（taskkill /PID <pid> /F），不要用 /IM Server.exe 与并行套件互杀")
    sys.exit(2)
except OSError:
    pass

env = dict(os.environ)
env["PATH"] = r"C:/Qt/5.14.2/mingw73_64/bin;C:/Qt/Tools/mingw730_64/bin;" + env["PATH"]
p = subprocess.Popen([EXE], cwd=SRV, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print("server pid:", p.pid)
dl = time.time() + 25
up = False
while time.time() < dl:
    try:
        socket.create_connection((HOST, PORT), timeout=1).close(); up = True; break
    except OSError:
        if p.poll() is not None:
            print("[E] server 启动即退出 rc=", p.returncode); sys.exit(2)
        time.sleep(0.3)
if not up:
    print("[E] server 25s 未监听"); p.kill(); sys.exit(2)

try:
    penv = dict(env); penv["PYTHONIOENCODING"] = "utf-8"
    r = subprocess.run([sys.executable, PROBES, str(p.pid)], capture_output=True,
                       text=True, encoding="utf-8", errors="replace", env=penv, timeout=600)
    sys.stdout.write(r.stdout)
    if r.stderr: sys.stderr.write(r.stderr)
    if r.returncode == 3:
        print("[ATTRIBUTION] SERVER_DIED：失败源于被监护进程消失（崩溃或外部按镜像名误杀），非产品断言失败")
    print("probes rc=", r.returncode, "alive_end=", p.poll() is None)
finally:
    if p.poll() is None:
        subprocess.run(["taskkill", "/PID", str(p.pid), "/F"], capture_output=True)
