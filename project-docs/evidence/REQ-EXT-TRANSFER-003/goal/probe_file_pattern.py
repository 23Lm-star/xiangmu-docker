# 一次性取证探针（goal 节点）：区分"每块 open+close"与"单句柄 seek+write"的成本。
# 目的：判定服务端 UploadWriteTask 每块 QFile{open,seek,write,flush,close} 的固定成本来源。
# 运行：cd dist && py -3.13 ../build/probe_file_pattern.py（写 dist/filesys/.staging 下的临时文件，收尾自删）
import os
import time

P = "filesys/.staging/_probe_pattern.tmp"
N = 256
CH = 4096


def prep():
    if os.path.exists(P):
        os.remove(P)
    with open(P, "wb") as f:
        f.write(b"\0" * (CH * N))


def bench(label, fn):
    prep()
    t0 = time.time()
    fn()
    dt = time.time() - t0
    print("%-36s %6.2fs  每块 %6.2f ms" % (label, dt, dt / N * 1000))


def per_open():
    for i in range(N):
        f = open(P, "r+b")
        f.seek(i * CH)
        f.write(b"x" * CH)
        f.close()


def per_open_fsync():
    for i in range(N):
        f = open(P, "r+b")
        f.seek(i * CH)
        f.write(b"x" * CH)
        f.flush()
        os.fsync(f.fileno())
        f.close()


def one_handle():
    f = open(P, "r+b")
    for i in range(N):
        f.seek(i * CH)
        f.write(b"x" * CH)
    f.close()


def one_handle_flush():
    f = open(P, "r+b")
    for i in range(N):
        f.seek(i * CH)
        f.write(b"x" * CH)
        f.flush()
    f.close()


if __name__ == "__main__":
    print("[P] %d 块 x %dB，文件 %s" % (N, CH, P))
    bench("每块 open+write+close", per_open)
    bench("每块 open+write+flush+fsync+close", per_open_fsync)
    bench("单句柄 seek+write", one_handle)
    bench("单句柄 seek+write+flush", one_handle_flush)
    bench("每块 open+write+close（重复）", per_open)
    os.remove(P)
