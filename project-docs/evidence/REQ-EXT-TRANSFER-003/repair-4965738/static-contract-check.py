# 本地工具（build/ 不入库）：只跑 verify.py 的静态断言段，用于快速迭代（跳过服务端/动态面）。
import os
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(REPO, "project-docs", "evidence", "REQ-EXT-TRANSFER-003", "verify.py"),
           encoding="utf-8").read()
static = src.split("# ================= 环境准备 =================")[0]
g = {"__name__": "__main__", "__file__": os.path.join(REPO, "project-docs", "evidence",
                                                      "REQ-EXT-TRANSFER-003", "verify.py")}
exec(compile(static, "verify.py:static", "exec"), g)
res = g["results"]
bad = [r for r in res if not r[2]]
print(f"\nSTATIC {len(res) - len(bad)}/{len(res)} passed")
print("STATIC_OK" if not bad else "STATIC_FAIL")
raise SystemExit(1 if bad else 0)
