# 独立测试节点的静态复核（设计 §9 批准稿 vs 实际代码 / 冻结面边界 / 关键实现点事实清单）。
# 只读：读 design.md（dev 工作树中的本地记录）、源码、git diff；不修改任何受版本控制的文件。
import hashlib, json, os, re, subprocess, sys

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
DEV = os.path.normpath(os.path.join(REPO, "..", "wt-claude"))     # design.md 只存在于 dev 工作树（未入库）
DESIGN = os.path.join(DEV, "project-docs", "tasks", "REQ-EXT-TRANSFER-003", "design.md")


def sha256(p):
    h = hashlib.sha256()
    with open(p, "rb") as fh:
        h.update(fh.read())
    return h.hexdigest()


def read(p):
    with open(p, encoding="utf-8") as fh:
        return fh.read()


def git(*args):
    return subprocess.run(["git"] + list(args), cwd=REPO, capture_output=True, text=True).stdout


out = []
def say(s):
    print(s)
    out.append(s)


say("=== 静态复核 REQ-EXT-TRANSFER-003 @b861351 ===")
say(f"artifacts:")
for label, p in [("design.md(dev worktree)", DESIGN),
                 ("Server/protocol.h", os.path.join(REPO, "Server", "protocol.h")),
                 ("Client/protocol.h", os.path.join(REPO, "Client", "protocol.h")),
                 ("verify.py", os.path.join(REPO, "project-docs", "evidence", "REQ-EXT-TRANSFER-003", "verify.py"))]:
    say(f"  {label}: sha256={sha256(p)[:16]}")

# ---- 1. 从 design.md §9 抽出批准稿代码块（§9 第一个 cpp 围栏）----
d = read(DESIGN)
sec9 = d.split("## 9. 冻结面逐字批准稿（F1）", 1)[1].split("## 10.", 1)[0]
m = re.search(r"```cpp\n(.*?)```", sec9, re.S)
approved = m.group(1)
say(f"\n[1] §9 批准稿（design.md 逐字抽取，{len(approved)} 字符 / {approved.count(chr(10))} 行）")

src = read(os.path.join(REPO, "Server", "protocol.h"))
lines = src.splitlines()
idx = next(i for i, ln in enumerate(lines) if ln.startswith("static constexpr uint MAX_INBOUND_PDU_SIZE"))
actual = "\n".join(lines[idx:idx + 5]) + "\n"
say(f"[2] protocol.h 第 {idx + 1}-{idx + 5} 行 == §9 批准稿（逐字节）：{actual == approved}")

# ---- 3. verify.py 内嵌的 APPROVED 常量 == §9（防"开发者同一份转写错两处"）----
vpy = read(os.path.join(REPO, "project-docs", "evidence", "REQ-EXT-TRANSFER-003", "verify.py"))
lit = re.search(r'APPROVED = """(.*?)"""', vpy, re.S).group(1)
say(f"[3] verify.py 的 APPROVED 字面量 == §9 批准稿：{lit == approved}")

# ---- 4. 分支 diff（merge-base..HEAD）事实 ----
base = git("merge-base", "master", "HEAD").strip()
diff = git("diff", "master...HEAD", "--", "Server/protocol.h")
adds = [l[1:] for l in diff.splitlines() if l.startswith("+") and not l.startswith("+++")]
dels = [l[1:] for l in diff.splitlines() if l.startswith("-") and not l.startswith("---")]
say(f"[4] merge-base={base[:7]}；Server/protocol.h 变更：+{len(adds)}/−{len(dels)}")
say(f"    新增行 == 批准稿各行：{adds == [l for l in approved.splitlines()]}")
say(f"    删除行 == 原两行常量：{sorted(x.strip() for x in dels) == sorted(['static constexpr uint MAX_INBOUND_PDU_SIZE = 16 * 1024;', 'static constexpr int MAX_PENDING_INBOUND_BYTES = 256 * 1024;'])}")
hunks = [l for l in diff.splitlines() if l.startswith("@@")]
say(f"    hunk 数 == 1：{len(hunks) == 1}（{hunks}）")
cli_diff = git("diff", "master...HEAD", "--", "Client/protocol.h")
say(f"[5] Client/protocol.h 零改动：{cli_diff.strip() == ''}")
say(f"[6] 全分支 diff 触及文件（排除 project-docs）：")
for ln in git("diff", "--name-only", "master...HEAD").splitlines():
    if not ln.startswith("project-docs"):
        say(f"    {ln}")

# ---- 7. 关键实现点事实清单（带行号）----
def grep_count(pattern, path):
    return [(i + 1, ln.strip()) for i, ln in enumerate(read(os.path.join(REPO, path)).splitlines())
            if re.search(pattern, ln)]

say("\n[7] 事实清单：")
for i, ln in grep_count(r"MAX_INBOUND_PDU_SIZE|MAX_PENDING_INBOUND_BYTES", "Server/mytcpsocket.cpp"):
    say(f"    mytcpsocket.cpp:{i}: {ln[:110]}")
mc = read(os.path.join(REPO, "Server", "mytcpsocket.cpp")).splitlines()
inj = next(i for i, ln in enumerate(mc) if "MAX_UPLOAD_DATA_CHUNK){" in ln)
infl = next(i for i, ln in enumerate(mc) if "cp->writeInFlight=true" in ln)
say(f"    D5 超长块拒绝（mytcpsocket.cpp:{inj + 1}）先于 writeInFlight=true（:{infl + 1}）：{inj < infl}")
perconn = [f"mytcpsocket.cpp:{i + 1}" for i, ln in enumerate(mc) if "new MsgHandler" in ln]
say(f"    MsgHandler 每连接一份（{perconn}）→ releaseOtherStagingHandles 的扫描域 = 单连接会话表")
mh = read(os.path.join(REPO, "Server", "msghandler.cpp")).splitlines()
for i, ln in enumerate(mh):
    if "releaseOtherStagingHandles" in ln and "void MsgHandler::" in ln:
        say(f"    releaseOtherStagingHandles 定义：msghandler.cpp:{i + 1}-{i + 16}")
        for j in range(i, min(i + 17, len(mh))):
            say(f"      {j + 1}: {mh[j]}")
        break
stale = [(i + 1, ln.strip()) for i, ln in enumerate(mc) if "256 KB" in ln or "256KB" in ln]
say(f"    遗留文案（常量已 1 MiB+8 KB，日志仍写 256 KB）：{[p for p, _ in stale]}")

os.makedirs(os.path.join(REPO, "build", "evidence", "current"), exist_ok=True)
with open(os.path.join(REPO, "build", "evidence", "current", "indtest-static-review.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(out) + "\n")
