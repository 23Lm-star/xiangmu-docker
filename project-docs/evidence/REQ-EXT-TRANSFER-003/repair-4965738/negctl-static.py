# 本地工具（build/ 不入库）：负对照——把 verify.py 静态段的源码读取改成"从修复前 b861351 读"，
# 断言新增的 S-06a..S-06f 必须全 FAIL（证明这些断言真能拦住回归，不是永真）。
import os, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REV = sys.argv[1] if len(sys.argv) > 1 else "b861351"
src = open(os.path.join(REPO, "project-docs", "evidence", "REQ-EXT-TRANSFER-003", "verify.py"),
           encoding="utf-8").read()
static = src.split("# ================= 环境准备 =================")[0]
old_read = 'def read(p): return open(os.path.join(REPO, p), encoding="utf-8").read()'
new_read = ('def read(p):\n'
            f'    return subprocess.run(["git", "show", "{REV}:" + p], cwd=REPO,\n'
            '                          capture_output=True, text=True).stdout\n')
assert old_read in static, "read() 定义行未找到——verify.py 结构变了，负对照需同步"
static = static.replace(old_read, new_read)
g = {"__name__": "__main__", "__file__": os.path.join(REPO, "project-docs", "evidence",
                                                      "REQ-EXT-TRANSFER-003", "verify.py")}
exec(compile(static, "verify.py:static@old", "exec"), g)
res = {sid: cc for sid, _, cc in g["results"]}
new_ids = ["S-06a", "S-06b", "S-06c", "S-06d", "S-06e", "S-06f"]
leaked = [s for s in new_ids if res.get(s)]
print(f"\nNEGCTL rev={REV}: 新增断言 {len(new_ids) - len(leaked)}/{len(new_ids)} 在旧码上 FAIL"
      f"{'（永真泄漏: ' + ','.join(leaked) + '）' if leaked else '，无永真'}")
print("NEGCTL_OK" if not leaked else "NEGCTL_FAIL")
raise SystemExit(1 if leaked else 0)
