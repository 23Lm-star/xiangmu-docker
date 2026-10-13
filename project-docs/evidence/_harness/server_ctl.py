# -*- coding: utf-8 -*-
"""server_ctl -- 回归套件共用的服务进程控制。

为什么存在这个模块
------------------
本项目的 10 套回归套件（test_gate.suites，369 断言）每套都自带一份 `start_server()` /
`stop_server()`，而每一份都长��样：

    subprocess.run(["taskkill", "/F", "/IM", "Server.exe"], capture_output=True)   # 开跑前
    subprocess.Popen([EXE], cwd=DIST, ...)
    subprocess.run(["taskkill", "/F", "/IM", "Server.exe"], capture_output=True)   # 收尾时

`taskkill /IM Server.exe` 按**映像名**杀进程，会打到机器上**每一个** Server.exe，包括别的套件
正在跑的那个。实测代价：三次独立的"服务端突然消失"结论（其中一次被独立审计记为 P4 返回 -99
的非确定性失败），根因全部是套件互杀，而不是产品缺陷。审计报告写着"该差异无法在只读审计中
安全归因"—— 因为谁杀了谁，在镜像名杀进程的语义下根本不可知。

因此规则是：**只按 PID 杀自己启动的进程**（PID 由 `subprocess.Popen` 直接返回，无需猜测），
并且**启动前检测端口占用、明确报错而不是盲杀**。

同项目内`REQ-EXT-SESSION-001/run_supervised.py` 已经独立实现了同一思路
（`taskkill /PID <pid> /F`），本模块把它 generalize 出来，让 10 套共用一份。

用法
----
    from server_ctl import ServerCtl
    ctl = ServerCtl(EXE, DIST)          # cwd 默认与 exe 同目录
    if not ctl.start():
        raise SystemExit("server did not come up: %s" % ctl.last_error)
    try:
        ...                             # 跑套件
    finally:
        ctl.stop()

    # cross_tree 那种要指定 cwd 的：
    ctl = ServerCtl(EXE, os.path.join(REPO, "build/xtree-file001"))

设计约束
--------
* 不引入第三方依赖。所有套件都在 Windows + Qt 5.14.2 + py -3.13 下跑。
* Qt 的 DLL 搜索路径必须在启动**前**设好，否则 Server.exe 会被 PATH 里的其它 Qt（如 Anaconda
  自带版本）互杀，报0xC0000409。这条是实测踩出来的，不要"简化"掉。
* 端口被占时**不杀**，直接报错并把占用者信息交给调用者。盲杀会把别人的进程打死，而这正是
  本模块要消灭的行为。
* 所有路径由调用方传入且必须是 repo-root 相对推导出来的绝对路径（本模块不做路径解析）。
"""

import os
import socket
import subprocess
import time

# 实测固化：Qt 5.14.2 MinGW 工具链。PATH 顺序有意义 —— 必须在最前。
# 注意用正斜杠：PATH 里已有的段可能是 `C:/Qt/...` 也可能是 `C:\Qt\...`，两者指向同一目录但
# 字符串不等。用反斜杠硬编码会让"是否已存在"的去重判定失效，于是本函数把 Qt 再加一次到
# 前面，行为看似正确；可一旦别处按反斜杠判存在，就会漏掉真正的重复项。
QT_BIN = "C:/Qt/5.14.2/mingw73_64/bin"
MINGW_BIN = "C:/Qt/Tools/mingw730_64/bin"


def ensure_qt_path():
    """把 Qt/MinGW 移到 PATH 最前。必须在 Popen 之前调用。

    去重按规范化后的大小写不敏感比较（Windows 路径语义），并且无论原先是否已存在都
    **前移**——本模块存在的意义就是保证 Server.exe 加载到正确的 Qt DLL，所以"已经在
    PATH 里"不等于"排在前面"。
    """
    p = os.environ.get("PATH", "")
    parts = [x for x in p.split(";") if x]
    wanted = [QT_BIN, MINGW_BIN]

    def norm(x):
        return os.path.normcase(os.path.normpath(x))

    rest = [x for x in parts if norm(x) not in {norm(w) for w in wanted}]
    os.environ["PATH"] = ";".join(wanted + rest)
    return os.environ["PATH"]


def port_busy(host, port, timeout=1.0):
    try:
        socket.create_connection((host, port), timeout=timeout).close()
        return True
    except OSError:
        return False


def find_listener_pids(port):
    """返回监听 `port` 的进程 PID 列表（Windows）。

    用 netstat 而不是 `tasklist /IM Server.exe`：前者按端口定位，与进程名无关，
    因此既能报告占用者，又不会误伤同名进程。
    """
    pids = set()
    try:
        out = subprocess.run(["netstat", "-ano", "-p", "tcp"],
                             capture_output=True, text=True, errors="replace")
    except OSError:
        return pids
    needle = f":{port}"
    for ln in (out.stdout or "").splitlines():
        if needle not in ln:
            continue
        parts = ln.split()
        # TCP  0.0.0.0:5000  0.0.0.0:0  LISTENING  12345
        if len(parts) >= 5 and parts[0].upper().startswith("TCP"):
            state = parts[-2].upper()
            if state == "LISTENING":
                try:
                    pids.add(int(parts[-1]))
                except ValueError:
                    pass
    return pids


class ServerCtl:
    """按 PID 管理一个 Server.exe 实例。

    属性
    ----
    pid        当前实例的 PID（未启动为 None）
    last_error 最近一次失败的原因（业务语言，可直接打印）
    """

    def __init__(self, exe, cwd=None, host="127.0.0.1", port=5000,
                 startup_timeout=25, poll=0.4, log_path=None):
        self.exe = exe
        self.cwd = cwd or os.path.dirname(exe)
        self.host = host
        self.port = port
        self.startup_timeout = startup_timeout
        # log_path: 把服务端控制台接到该文件（追加）。默认 None = DEVNULL，保持既有套件行为不变；
        # 需要读服务端日志做断言的套件（如 group-002 的 p7_slow_conn_dropped）才传。
        self.log_path = log_path
        self._logf = None
        self.poll = poll
        self.pid = None
        self.last_error = None

    # -- 启动 ------------------------------------------------------------
    def start(self):
        """启动一个实例并等它监听端口。成功返回 True。

        端口已被占用时**不杀任何进程**：先前的进程可能属于另一个套件或主人手动开的实例，
        盲杀就是本模块要消灭的那个行为。改为报错，让调用方决定。
        """
        self.last_error = None

        if not os.path.isfile(self.exe):
            self.last_error = (f"Server.exe 不存在: {self.exe} "
                               f"（先跑 pipeline/build.ps1，或 artifacts.ps1 -Deploy dist）")
            return False

        if port_busy(self.host, self.port):
            pids = sorted(find_listener_pids(self.port))
            self.last_error = (
                f"端口 {self.port} 已被占用（监听 PID: {pids or '未知'}）。"
                f"本套件只按 PID 管理自己启动的进程，不会替你盲杀。"
                f"若确认那些进程属于本套件的残留，请手动按 PID 结束后再跑。")
            return False

        ensure_qt_path()
        if self.log_path:
            os.makedirs(os.path.dirname(os.path.abspath(self.log_path)), exist_ok=True)
            self._logf = open(self.log_path, "ab")
            out_dest, err_dest = self._logf, subprocess.STDOUT
        else:
            out_dest, err_dest = subprocess.DEVNULL, subprocess.DEVNULL
        # QT_ASSUME_STDERR_HAS_CONSOLE=1 是必需的：重定向到文件时 stderr 不是控制台，Qt 会
        # 改走 OutputDebugString，日志进不了文件（实测 0 字节）。置位后 qCWarning/qCDebug 才写
        # stderr。与 Server.pro 的 CONFIG += console 是一对：前者让子进程有可写的 stderr，
        # 后者让 Qt 认它是控制台。
        env = dict(os.environ)
        if self.log_path:
            env["QT_ASSUME_STDERR_HAS_CONSOLE"] = "1"
        try:
            self.pid = subprocess.Popen(
                [self.exe], cwd=self.cwd, env=env,
                stdout=out_dest, stderr=err_dest).pid
        except OSError as e:
            self.pid = None
            self.last_error = f"启动失败: {e}"
            return False

        dl = time.time() + self.startup_timeout
        while time.time() < dl:
            if port_busy(self.host, self.port):
                return True
            # 进程在监听之前就死了：立刻报错，不要空等满timeout
            if self._pid_gone():
                self.last_error = (f"Server.exe (PID {self.pid}) 在监听 {self.port} 之前退出。"
                                   f"常见原因: PATH 里Qt 版本冲突（0xC0000409）、"
                                   f"connect.config 行尾不是 CRLF、数据库不可连。")
                self.pid = None
                return False
            time.sleep(self.poll)

        self.last_error = (f"Server.exe (PID {self.pid}) {self.startup_timeout}s 内未监听 "
                           f"{self.port}。")
        return False

    # -- 停止 ------------------------------------------------------------
    def stop(self):
        """只结束自己启动的那个 PID。返回是否执行了结束动作。"""
        pid, self.pid = self.pid, None
        try:
            if not pid:
                return False
            if self._pid_gone(pid):
                return False
            subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
            # 等端口真正释放，否则紧接的 start 会撞上"端口已占用"
            dl = time.time() + 10
            while time.time() < dl:
                if not port_busy(self.host, self.port):
                    break
                time.sleep(self.poll)
            return True
        finally:
            if self._logf:
                try:
                    self._logf.close()
                except OSError:
                    pass
                self._logf = None

    def __enter__(self):
        if not self.start():
            raise RuntimeError(self.last_error or "server failed to start")
        return self

    def __exit__(self, *exc):
        self.stop()
        return False

    # -- 内部 ------------------------------------------------------------
    def _pid_gone(self, pid=None):
        pid = pid if pid is not None else self.pid
        if not pid:
            return True
        try:
            out = subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH"],
                                 capture_output=True, text=True, errors="replace")
        except OSError:
            return False
        if not out.stdout:
            return True
        return str(pid) not in out.stdout


# 便于一次性替换老套件的 taskkill /IM 调用：
#     server_ctl.kill_by_pid(pid)
def kill_by_pid(pid, port=None, host="127.0.0.1", wait=10.0):
    """按 PID 结束进程；给了 port 就等到端口释放。返回是否结束过。"""
    if not pid:
        return False
    subprocess.run(["taskkill", "/PID", str(pid), "/F"], capture_output=True)
    if port:
        dl = time.time() + wait
        while time.time() < dl:
            if not port_busy(host, port):
                return True
            time.sleep(0.4)
    return True


def refuse_if_port_busy(host, port):
    """老套件的 start_server() 用它替代盲杀：端口被占就报错。"""
    if not port_busy(host, port):
        return None
    pids = sorted(find_listener_pids(port))
    return (f"端口 {port} 已被占用（监听 PID: {pids or '未知'}）。"
            f"按 PID 结束残留实例后再跑；不要用 taskkill /IM Server.exe —— "
            f"它会连带杀掉并行套件的进程。")