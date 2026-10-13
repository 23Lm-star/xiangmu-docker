#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
verify-integration-gate.py — 多 Agent 协同「变更门禁」校验器（只读，不修改仓库）

------------------------------------------------------------------------------
设计目标
    把 project-docs/governance/integration-policy.md 第 3 节的「变更门禁」从
    自然语言约束升级为可被机器自动拒绝的硬检查。

核心特性
    * 完全只读：只执行 git diff / rev-parse / log / merge-base 等查询命令，
      绝不修改任何文件、绝不产生提交、绝不切换分支。你的源代码不会被触碰。
    * 可独立运行：集成者（或 CI）在合并任一任务分支前运行一次，
      PASS 才允许合并；FAIL 会列出具体越界文件 / 缺失项。
    * 退出码：0 = 通过；1 = 存在越界或缺失（应拒绝合并）；2 = 配置/参数错误。

用法
    python verify-integration-gate.py --contract <task.yaml> [--repo <path>]
                                       [--branch <branch>] [--json] [--quiet]

    --contract  单个任务的契约 YAML（顶层即 task 字段，或含 example_task: 键）
    --repo      仓库根目录（默认当前目录）
    --branch    要校验的分支名（默认当前分支）
    --json      以 JSON 输出结果（便于 CI 解析）
    --selftest  运行内置逻辑自测（不读取任何仓库，验证路径匹配/等级逻辑）

依赖
    需要 PyYAML。未安装时脚本会给出明确的 pip 安装提示后退出（退出码 2）。
------------------------------------------------------------------------------
"""

import argparse
import json
import os
import re
import subprocess
import sys

try:
    import yaml
except ImportError:
    sys.stderr.write(
        "ERROR: 需要 PyYAML。请运行:  pip install pyyaml\n"
        "（本脚本只读校验，不修改仓库；安装 PyYAML 不影响你的项目。）\n"
    )
    sys.exit(2)


# ---------------------------------------------------------------------------
# 证据等级
# ---------------------------------------------------------------------------
LEVEL_INT = {"L0": 0, "L1": 1, "L2": 2, "L3": 3, "L4": 4}
LEVEL_NAME = {v: k for k, v in LEVEL_INT.items()}

# 触碰这些受保护区 → 至少需要 L3（真实动态）证据
HIGH_RISK_PATTERNS = ["*/protocol.h", "Server/operatedb.*", "*.sql", "*.pro"]


# ---------------------------------------------------------------------------
# git 查询（只读）
# ---------------------------------------------------------------------------
def git(repo, *args):
    """执行 git 查询命令，返回 stdout 文本。任何非 0 退出都会抛异常。"""
    proc = subprocess.run(
        ["git", "-C", repo, *args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            "git %s 失败 (rc=%d): %s" % (" ".join(args), proc.returncode, proc.stderr.strip())
        )
    return proc.stdout


def git_exists(repo, rev):
    proc = subprocess.run(
        ["git", "-C", repo, "cat-file", "-t", rev],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return proc.returncode == 0


def git_is_ancestor(repo, base, tip):
    proc = subprocess.run(
        ["git", "-C", repo, "merge-base", "--is-ancestor", base, tip],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return proc.returncode == 0


# ---------------------------------------------------------------------------
# 路径匹配（gitignore 风格 glob）
# ---------------------------------------------------------------------------
def _norm(p):
    return p.replace("\\", "/").strip()


def match_any(path, patterns):
    """path 是否匹配 patterns 中任意一个 glob。支持 ** 与 *.pro 这类写法。"""
    path = _norm(path)
    for pat in patterns or []:
        pat = _norm(pat)
        if not pat:
            continue
        # 目录递归： dir/** 或 dir/**
        if pat.endswith("/**") or pat == "**":
            base = pat[:-3] if pat.endswith("/**") else ""
            base = base.rstrip("/")
            if path == base or path.startswith(base + "/") or (base == "" and path):
                return True
            continue
        # fnmatch：* 可跨目录匹配，能覆盖 Client/msghandler.* / */protocol.h 等
        import fnmatch
        if fnmatch.fnmatch(path, pat):
            return True
        # 额外对 basename 匹配（*.pro 应匹配任意目录下的 .pro 文件）
        if fnmatch.fnmatch(os.path.basename(path), os.path.basename(pat)):
            return True
    return False


# ---------------------------------------------------------------------------
# YAML 加载（支持模板文件与裸 task 映射）
# ---------------------------------------------------------------------------
def load_task_contract(path):
    with open(path, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f) or {}
    if "task_id" in data:
        return data
    if "example_task" in data and isinstance(data["example_task"], dict):
        return data["example_task"]
    # 取第一个看起来像 task 的映射
    for v in data.values():
        if isinstance(v, dict) and "task_id" in v:
            return v
    raise RuntimeError("契约文件 %s 中未找到 task_id 字段" % path)


def load_locks(repo):
    p = os.path.join(repo, "project-docs", "governance", "active-locks.yaml")
    if not os.path.exists(p):
        return None
    with open(p, "r", encoding="utf-8") as f:
        data = yaml.safe_load(f) or {}
    return data.get("locks", []) or []


# ---------------------------------------------------------------------------
# 门禁检查
# ---------------------------------------------------------------------------
class Gate:
    def __init__(self, repo, branch, contract):
        self.repo = repo
        self.branch = branch
        self.c = contract
        self.findings = []   # (severity, code, message)
        self.tip = None

    def ok(self, msg):
        self.findings.append(("PASS", None, msg))

    def warn(self, code, msg):
        self.findings.append(("WARN", code, msg))

    def fail(self, code, msg):
        self.findings.append(("FAIL", code, msg))

    # --- 门禁 1：task_id 有效 + 锁已登记 ---
    def check_lock(self):
        tid = self.c.get("task_id")
        if not tid:
            self.fail("G1", "task_id 缺失")
            return
        self.ok("task_id = %s" % tid)
        lock_ids = self.c.get("lock_ids") or []
        if not lock_ids:
            self.warn("G1", "lock_ids 为空：未登记任务锁（集成者应在 active-locks.yaml 登记）")
            return
        locks = load_locks(self.repo)
        if locks is None:
            self.warn("G1", "active-locks.yaml 不存在，无法核验锁；请人工确认")
            return
        valid = [l for l in locks if l.get("task_id") == tid]
        for lid in lock_ids:
            if any(l.get("lock_id") == lid for l in valid):
                self.ok("锁 %s 已登记且绑定本任务" % lid)
            else:
                self.fail("G1", "锁 %s 未在 active-locks.yaml 中绑定本任务" % lid)

    # --- 门禁 2：base_commit 存在且为分支祖先 ---
    def check_base(self):
        base = self.c.get("base_commit")
        if not base:
            self.fail("G2", "base_commit 缺失")
            return
        if not git_exists(self.repo, base):
            self.fail("G2", "base_commit %s 在仓库中不存在（可能被强制覆盖）" % base)
            return
        if not git_is_ancestor(self.repo, base, self.tip):
            self.fail("G2", "base_commit %s 不是分支 %s 的祖先（基线不一致）" % (base, self.branch))
            return
        self.ok("base_commit %s 有效且为 %s 的祖先" % (base, self.branch))

    # --- 门禁 3：diff 完全落在 allowed_paths，且不触碰 protected_paths ---
    def check_paths(self, changed):
        allowed = self.c.get("allowed_paths") or []
        protected = self.c.get("protected_paths") or []
        if not allowed:
            self.fail("G3", "allowed_paths 为空：没有白名单，等于整库开放")
            return
        # 整库通配禁止
        if any(a.strip() in (".*", "*", "**") for a in allowed):
            self.fail("G3", "allowed_paths 含整库通配，违反最小授权")
            return
        out_of_allow = [f for f in changed if not match_any(f, allowed)]
        hit_protected = [f for f in changed if match_any(f, protected)]
        if out_of_allow:
            self.fail("G3", "越出白名单的文件 (%d): %s" % (len(out_of_allow), out_of_allow[:20]))
        else:
            self.ok("全部 %d 个改动文件均落在白名单内" % len(changed))
        if hit_protected:
            self.fail("G3", "触碰受保护区文件 (%d): %s" % (len(hit_protected), hit_protected[:20]))
        else:
            self.ok("未触碰任何受保护区文件")

    # --- 门禁 4：提交链 engineer→review→test 连续，且 test_commit == 待合并代码 ---
    def check_commit_chain(self, changed):
        ev = self.c.get("evidence_level")
        ev_i = LEVEL_INT.get(ev, 0) if ev else 0
        force_chain = (ev_i >= 3) or (self.c.get("requires_commit_chain") is True)
        if not force_chain:
            self.warn("G4", "证据等级 %s < L3 且未强制提交链：跳过提交链硬校验（如为代码任务应设为 requires_commit_chain: true）" % ev)
            return
        eng = self.c.get("engineer_commit")
        rev = self.c.get("review_commit")
        tst = self.c.get("test_commit")
        for name, val in (("engineer_commit", eng), ("review_commit", rev), ("test_commit", tst)):
            if not val:
                self.fail("G4", "%s 缺失（需要完整提交链）" % name)
                return
            if not git_exists(self.repo, val):
                self.fail("G4", "%s=%s 在仓库中不存在" % (name, val))
                return
        # 祖先链
        if not (git_is_ancestor(self.repo, eng, rev) and
                git_is_ancestor(self.repo, rev, tst) and
                git_is_ancestor(self.repo, tst, self.tip)):
            self.fail("G4", "提交链不是 engineer→review→test→tip 的连续祖先链")
            return
        self.ok("提交链 engineer→review→test→tip 连续")
        # test_commit 所测代码 == 待合并代码
        post = git(self.repo, "diff", "--name-only", tst, self.tip).splitlines()
        post = [p for p in post if p.strip()]
        if post:
            self.fail("G4", "test_commit 之后又有代码改动（被测 != 待合并）: %s" % post[:20])
        else:
            self.ok("test_commit 之后无代码改动（被测 == 待合并）")

    # --- 门禁 5：evidence_level 达到变更类型要求 ---
    def check_evidence(self, changed):
        ev = self.c.get("evidence_level")
        ev_i = LEVEL_INT.get(ev, 0) if ev else 0
        min_req = 1
        if any(match_any(f, HIGH_RISK_PATTERNS) for f in changed):
            min_req = 3
        if ev_i >= min_req:
            self.ok("evidence_level=%s 满足最低要求 L%d" % (ev, min_req))
        else:
            self.fail("G5", "evidence_level=%s 低于变更类型要求 L%d（触碰协议/DB/构建等高风险区必须 L3）" % (ev, min_req))

    # --- 门禁 6：required_approvals 已签署（字段级校验）---
    def check_approvals(self):
        appro = self.c.get("required_approvals") or []
        if not appro:
            self.warn("G6", "required_approvals 为空：如涉及跨模块/接口变更，应显式要求架构师批准")
            return
        roles = set()
        for a in appro:
            if isinstance(a, dict):
                roles.add(a.get("role"))
            else:
                roles.add(a)
        if None in roles:
            self.fail("G6", "required_approvals 条目缺少 role 字段")
            return
        self.ok("已声明审批角色: %s（集成者需确认其已实际签署）" % sorted(r for r in roles if r))

    # --- 门禁 7：集成者唯一性字段 ---
    def check_integrator(self):
        integ = self.c.get("unique_integrator")
        if not integ:
            self.fail("G7", "unique_integrator 缺失：未指定唯一集成者")
            return
        self.ok("unique_integrator = %s" % integ)

    def run(self):
        self.tip = git(self.repo, "rev-parse", self.branch).strip()
        base = self.c.get("base_commit") or self.tip
        changed = [l for l in git(self.repo, "diff", "--name-only", base, self.tip).splitlines() if l.strip()]
        self.ok("分支 %s (tip %s)，相对 base 改动文件数 = %d" % (self.branch, self.tip[:8], len(changed)))

        self.check_lock()
        self.check_base()
        self.check_paths(changed)
        self.check_commit_chain(changed)
        self.check_evidence(changed)
        self.check_approvals()
        self.check_integrator()

        fails = [f for f in self.findings if f[0] == "FAIL"]
        warns = [f for f in self.findings if f[0] == "WARN"]
        return len(fails) == 0, fails, warns


# ---------------------------------------------------------------------------
# 内置自测（不读取任何仓库）
# ---------------------------------------------------------------------------
def selftest():
    print("=== 内置自测（不涉及仓库） ===")
    # 路径匹配
    assert match_any("Client/msghandler.cpp", ["Client/msghandler.*"])
    assert match_any("Server/protocol.h", ["*/protocol.h"])
    assert match_any("a/b/protocol.h", ["*/protocol.h"])
    assert match_any("Client/foo.pro", ["*.pro"])
    assert match_any("project-docs/governance/x.md", ["project-docs/governance/**"])
    assert not match_any("Client/main.cpp", ["Server/**"])
    assert not match_any("README.md", ["Client/**", "Server/**"])
    print("[OK] 路径匹配（白名单/受保护区/目录递归）")
    # 等级
    assert LEVEL_INT["L3"] > LEVEL_INT["L1"]
    print("[OK] 证据等级映射")
    # 整库通配检测逻辑
    assert any(a.strip() in (".*", "*", "**") for a in [".*"])
    print("[OK] 整库通配检测")
    print("自测全部通过 ✅")
    return 0


# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="多 Agent 协同变更门禁校验器（只读）")
    ap.add_argument("--contract", help="任务契约 YAML 路径")
    ap.add_argument("--repo", default=".", help="仓库根目录（默认当前目录）")
    ap.add_argument("--branch", default=None, help="要校验的分支（默认当前分支）")
    ap.add_argument("--json", action="store_true", help="JSON 输出")
    ap.add_argument("--quiet", action="store_true", help="仅输出结论")
    ap.add_argument("--selftest", action="store_true", help="运行内置逻辑自测")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if not args.contract:
        sys.stderr.write("ERROR: 必须提供 --contract <task.yaml>\n")
        return 2

    repo = os.path.abspath(args.repo)
    contract = load_task_contract(args.contract)
    branch = args.branch or git(repo, "rev-parse", "--abbrev-ref", "HEAD").strip()

    gate = Gate(repo, branch, contract)
    try:
        passed, fails, warns = gate.run()
    except RuntimeError as e:
        sys.stderr.write("ERROR: %s\n" % e)
        return 2

    if args.json:
        out = {
            "branch": branch,
            "task_id": contract.get("task_id"),
            "passed": passed,
            "failures": [{"code": c, "msg": m} for (_, c, m) in fails],
            "warnings": [{"code": c, "msg": m} for (_, c, m) in warns],
        }
        print(json.dumps(out, ensure_ascii=False, indent=2))
    else:
        print("=" * 64)
        print("变更门禁校验  branch=%s  task=%s" % (branch, contract.get("task_id")))
        print("=" * 64)
        for sev, code, msg in gate.findings:
            if args.quiet and sev == "PASS":
                continue
            mark = {"PASS": "✅", "WARN": "⚠️ ", "FAIL": "❌"}[sev]
            print("%s [%s] %s" % (mark, code or "-", msg))
        print("-" * 64)
        print("结论: %s" % ("PASS ✅ 允许合并" if passed else "FAIL ❌ 拒绝合并"))
        if warns:
            print("（含 %d 条需人工确认的警告）" % len(warns))
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
