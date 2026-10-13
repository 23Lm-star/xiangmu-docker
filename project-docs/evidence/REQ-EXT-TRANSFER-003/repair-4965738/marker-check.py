# 本地工具（build/ 不入库）：双端 003/修复面特征标记核对（部署前后同法自证）。
# 客户端 G7 文案经 QStringLiteral 以 UTF-16 存储；qCWarning/qCDebug 的窄字面量按 UTF-8。
import hashlib, os, sys

MARKERS = [
    ("Server.exe", "D5-qCWarning", "超长上传块拒绝".encode("utf-8")),
    ("Client.exe", "G7-QStringLiteral", "与服务端断开连接，可重试".encode("utf-16-le")),
    ("Client.exe", "REPAIR-Da-qCWarning", "快照跳过：既无当前用户也无历史登录名".encode("utf-8")),
    ("Client.exe", "REPAIR-Da-fallback-qCWarning", "快照按最近一次登录名兜底写入".encode("utf-8")),
]


def main(dist):
    for fn, label, needle in MARKERS:
        p = os.path.join(dist, fn)
        if not os.path.exists(p):
            print(f"{fn:12s} {label:34s} MISSING-FILE")
            continue
        buf = open(p, "rb").read()
        print(f"{fn:12s} {label:34s} hit={buf.count(needle)}  "
              f"sha256={hashlib.sha256(buf).hexdigest()[:16]}  size={len(buf)}")


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "dist"))
