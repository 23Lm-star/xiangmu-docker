# 一次性取证探针：分离"每块一次 open/close"的成本构成（raw Win32：CreateFileW / WriteFile / CloseHandle）。
# 回答两个质疑：(1) 30 ms 是否 = 文件句柄创建/销毁成本；(2) 是否 Python 侧假象。
# 用 ctypes 直接调 kernel32，绕开 Python io 层；与 Server 侧 QFile::open(ReadWrite) 同一 API 面。
# 用法：py -3.13 build/probe_open_cost.py <绝对路径|.>   （路径为目录时写 <dir>/_probe_open.tmp）
import ctypes
import os
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateFileW.restype = ctypes.c_void_p
k32.CreateFileW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
                            ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
k32.CloseHandle.argtypes = [ctypes.c_void_p]
k32.WriteFile.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32,
                          ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p]
k32.SetFilePointerEx.argtypes = [ctypes.c_void_p, ctypes.c_longlong,
                                 ctypes.POINTER(ctypes.c_longlong), ctypes.c_uint32]

GENERIC_READ, GENERIC_WRITE = 0x80000000, 0x40000000
SHARE_RW = 0x1 | 0x2
OPEN_EXISTING = 3
FILE_ATTRIBUTE_NORMAL = 0x80
INVALID = ctypes.c_void_p(-1).value
N = 256
CH = 4096


def create(path, acc=GENERIC_READ | GENERIC_WRITE):
    h = k32.CreateFileW(path, acc, SHARE_RW, None, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, None)
    if h == INVALID:
        raise ctypes.WinError(ctypes.get_last_error())
    return h


def prep(path):
    with open(path, "wb") as f:
        f.write(b"\0" * (CH * N))


def per_open_readwrite(path):
    """每块 CreateFileW(RW) → seek → WriteFile → CloseHandle = Server/uploadwritetask.cpp 形态"""
    sp = [0.0, 0.0, 0.0]
    for i in range(N):
        t0 = time.perf_counter()
        h = create(path)
        t1 = time.perf_counter()
        k32.SetFilePointerEx(h, i * CH, None, 0)
        w = ctypes.c_uint32(0)
        k32.WriteFile(h, b"x" * CH, CH, ctypes.byref(w), None)
        t2 = time.perf_counter()
        k32.CloseHandle(h)
        t3 = time.perf_counter()
        sp[0] += t1 - t0; sp[1] += t2 - t1; sp[2] += t3 - t2
    return sp


def per_open_rw_fsync(path):
    """每块 CreateFileW(RW)+write+FlushFileBuffers+CloseHandle：把"脏数据交给下一次 open 冲刷"的
    假设反过来验——显式在 close 前冲刷，看 CreateFileW 是否随之变快（成本只是换了位置）。"""
    sp = [0.0, 0.0, 0.0]
    for i in range(N):
        t0 = time.perf_counter()
        h = create(path)
        t1 = time.perf_counter()
        k32.SetFilePointerEx(h, i * CH, None, 0)
        w = ctypes.c_uint32(0)
        k32.WriteFile(h, b"x" * CH, CH, ctypes.byref(w), None)
        k32.FlushFileBuffers(h)
        t2 = time.perf_counter()
        k32.CloseHandle(h)
        t3 = time.perf_counter()
        sp[0] += t1 - t0; sp[1] += t2 - t1; sp[2] += t3 - t2
    return sp


def one_handle_fsync(path):
    """单句柄 + 每块 FlushFileBuffers：若修复方案要保留"每块落盘"语义，成本还剩多少"""
    sp = [0.0, 0.0, 0.0]
    t0 = time.perf_counter()
    h = create(path)
    t1 = time.perf_counter()
    for i in range(N):
        k32.SetFilePointerEx(h, i * CH, None, 0)
        w = ctypes.c_uint32(0)
        k32.WriteFile(h, b"x" * CH, CH, ctypes.byref(w), None)
        k32.FlushFileBuffers(h)
    t2 = time.perf_counter()
    k32.CloseHandle(h)
    t3 = time.perf_counter()
    sp[0] += t1 - t0; sp[1] += t2 - t1; sp[2] += t3 - t2
    return sp


def per_open_write_acc(path):
    """同上但只请求 GENERIC_READ|WRITE 的 open/close，不写"""
    sp = [0.0, 0.0, 0.0]
    for _ in range(N):
        t0 = time.perf_counter()
        h = create(path)
        t1 = time.perf_counter()
        k32.CloseHandle(h)
        t2 = time.perf_counter()
        sp[0] += t1 - t0; sp[2] += t2 - t1
    return sp


def per_open_readonly(path):
    """只读打开 + 关闭"""
    sp = [0.0, 0.0, 0.0]
    for _ in range(N):
        t0 = time.perf_counter()
        h = create(path, GENERIC_READ)
        t1 = time.perf_counter()
        k32.CloseHandle(h)
        t2 = time.perf_counter()
        sp[0] += t1 - t0; sp[2] += t2 - t1
    return sp


def one_handle(path):
    sp = [0.0, 0.0, 0.0]
    t0 = time.perf_counter()
    h = create(path)
    t1 = time.perf_counter()
    for i in range(N):
        k32.SetFilePointerEx(h, i * CH, None, 0)
        w = ctypes.c_uint32(0)
        k32.WriteFile(h, b"x" * CH, CH, ctypes.byref(w), None)
    t2 = time.perf_counter()
    k32.CloseHandle(h)
    t3 = time.perf_counter()
    sp[0] += t1 - t0; sp[1] += t2 - t1; sp[2] += t3 - t2
    return sp


def seq_throughput(path, mb=32):
    chunk = 1 << 20
    with open(path, "wb") as f:
        t0 = time.perf_counter()
        for _ in range(mb):
            f.write(b"z" * chunk)
        f.flush()
        os.fsync(f.fileno())
        dt = time.perf_counter() - t0
    print("  %-38s %6.2fs  %7.1f MiB/s" % ("顺序写 %d MiB（单句柄+fsync）" % mb, dt, mb / dt))


def run(label, path, fn):
    prep(path)
    t0 = time.perf_counter()
    sp = fn(path)
    dt = time.perf_counter() - t0
    print("  %-38s %6.2fs  %7.2f ms/op   [CreateFileW %.2f | write %.2f | CloseHandle %.2f]"
          % (label, dt, dt / N * 1000, sp[0] / N * 1000, sp[1] / N * 1000, sp[2] / N * 1000))


def main():
    arg = sys.argv[1] if len(sys.argv) > 1 else "."
    path = os.path.join(arg, "_probe_open.tmp") if os.path.isdir(arg) else arg
    print("[probe] %s" % os.path.abspath(path))
    run("每块 CreateFileW(RW)+write+CloseHandle", path, per_open_readwrite)
    run("每块 CreateFileW(RW)+write+fsync+CloseHandle", path, per_open_rw_fsync)
    run("每块 CreateFileW(RW)+CloseHandle（不写）", path, per_open_write_acc)
    run("每块 CreateFileW(只读)+CloseHandle", path, per_open_readonly)
    run("单句柄 seek+write（基线）", path, one_handle)
    run("单句柄 seek+write+每块 fsync", path, one_handle_fsync)
    seq_throughput(path)
    try:
        os.remove(path)
    except OSError:
        pass


if __name__ == "__main__":
    main()
