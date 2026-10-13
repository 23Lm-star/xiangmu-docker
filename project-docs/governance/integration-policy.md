# 集成策略与变更门禁（Integration Policy & Change Gate）

> 本文件把《协作总则》里的“协作意识”升级为**可强制执行的变更门禁**。
> 设计目标：让系统“有能力阻止越界”，而不只是“要求 Agent 自律”。

## 1. 唯一集成者（Unique Integrator）
- 全仓库**同一时刻只有一个**集成者（默认 `project_architect`，可在 ADR-012 中改为独立角色）。
- 只有集成者能把任务分支合并进 `master`/主发布线。
- 架构师、工程师、Bug 查找师、测试师**均不得**直接 `commit`/`push`/`merge` 到共享主分支，也不得兼任集成者。
  - 若架构师被设为集成者，则其在“治理/设计文档”上的正常维护与“集成动作”必须分开：文档变更走文档流程，集成动作走本门禁。
- 任何人的产品代码改动，都必须先进入自己任务的独立分支/worktree，再经门禁由集成者并入。

## 2. 任务隔离模型（Per-Task Isolation）
- 每个任务从 `task-template.yaml` 的 `base_commit`（不可变基线）切出**独立分支或 git worktree**。
- 禁止多个任务共享同一工作树；禁止在主线工作树上直接开发。
- 工作树根目录（如当前 `E:\qt\xiangmu`）只作为“待拆分考古现场”，冻结写入，直到迁移完成。

## 3. 变更门禁（Change Gate）—— 合并前必须全部满足
1. `task_id` 有效且已在 `active-locks.yaml` 登记锁。
2. `base_commit` 与仓库当前基线一致（未被强制覆盖）。
3. `git diff --name-only` 结果**完全落在** `allowed_paths` 内，且**未触碰**任何 `protected_paths`。
   - 越界 diff 由 pre-merge 钩子/验证脚本**自动拒绝**（见第 5 节）。
4. `engineer_commit → review_commit → test_commit` 提交链连续，且 `test_commit` 所测代码 == 待合并代码（hash 一致）。
5. `evidence_level` 达到该变更类型要求（见 evidence-levels.md）：
   - 涉及协议/权限/数据持久化/跨模块 → 至少 L3（真实动态）；仅凭 L1/L2 不得标“已修复/完整回归通过”。
6. 所需的 `required_approvals` 均已签署。
7. 相关缺陷状态已按协作总则第八节同步；锁已释放。

## 4. 任务锁（Task Lock）
- 开工前在 `active-locks.yaml` 登记：`lock_id`、`task_id`、被锁路径、`holder_role`、到期时间。
- 同一路径同时只能被一个任务锁持有；冲突时由集成者仲裁。
- 任务 integrated 或 rejected 后必须释放锁。

## 5. 自动校验脚本（已落地 ✅）
- 脚本：`governance/verify-integration-gate.py`（纯只读，只跑 `git diff/rev-parse/log/merge-base`，**绝不修改任何文件、绝不产生提交、绝不切换分支**）。
- 校验范围：第 3 节全部 7 项门禁（G1 锁登记 / G2 基线 / G3 路径白黑名单 / G4 提交链 / G5 证据等级 / G6 审批 / G7 唯一集成者）。
- 退出码：`0` = PASS 允许合并；`1` = FAIL 拒绝合并；`2` = 参数/配置错误。
- 依赖：Python 3 + `PyYAML`（`pip install pyyaml`，脚本在缺失时会给出明确提示，不影响仓库）。

### 集成者用法（合并任一任务分支前手动运行）
```bash
python project-docs/governance/verify-integration-gate.py \
    --contract <本任务的契约.yaml> \
    --repo E:/qt/xiangmu \
    --branch <任务分支名(扁平名)>
# 可选： --json（CI 解析）  --quiet（仅看结论）  --selftest（逻辑自测，不读仓库）
```

### 实测结果（2026-07-30，冻结工作树未动）
- 用"仅治理文档"契约校验混合分支 `wip-mixed-pre-refactor` → **FAIL**：76 文件越白名单、8 文件碰受保护区、证据 L1<L3。
- 用同契约校验基线分支 `add_数据库连接池功能`（零改动）→ **PASS**。
- 证明：门禁能准确区分越界与合规，且全程只读。

### ⚠️ 接入方式提醒（保护共享主工作树）
- **不要**在 `E:\qt\xiangmu` 的 `.git/hooks` 里装会自动拦截提交的 `pre-commit`/`pre-merge` 钩子——会干扰你在本项目上的正常开发提交。
- 当前阶段采用**集成者手动运行兜底**：每次合并任务分支前跑一次上面的命令，PASS 才合并。
- 若日后要全自动：优先接 **CI 流水线**（在合并 PR 时跑），而非本地钩子；或仅对 `master` 受保护的远端仓库部署 `pre-receive`。

### 分支命名约束（本仓库 Git 环境已知坑）
- 本仓库 `git branch`/`checkout -b` 对**带 `/` 的嵌套分支名会静默失败**（ref 不落盘，分支变 unborn）。
- 所有任务分支**一律用扁平名**（如 `task-gov-archive`、`task-r18-publink`），不要用 `task/xxx`。详见 `migration-plan.md` §0.5。

## 6. 与既有《协作总则》的关系
- 本门禁是总则第七节“Agent 执行约束”的**强制化补强**，不替代总则；总则中的状态流转、交接规范、缺陷编号等继续有效。
- 本文件与 `task-template.yaml`、`path-ownership.yaml`、`evidence-levels.md`、`active-locks.yaml` 共同构成执行控制层。
