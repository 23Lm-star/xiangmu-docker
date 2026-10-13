# 第二阶段迁移规划（Migration Plan — Phase 2, 设计稿）

> 生成日期：2026-07-30
> 状态：规划（待用户确认后按此执行；执行时每个任务必须先建 `task-template.yaml` 契约）
> 前提：第一阶段治理设计已完成（ADR-012 等 7 个文件）；现场已快照（`E:\qt\xiangmu.refactor-snapshot.2026-07-30` + 保护分支 `preserve/pre-refactor-2026-07-30` @ `880b19e`）。
> 核心约束：未通过门禁（integration-policy.md）的最小 patch，**不得**并入主线；`protocol.h`/`operatedb.*`/`*.pro`/`*.sql` 属受保护区，任何改动须架构师批准 + `allowed_symbols`。

## 0.5 本仓库 Git 环境已知问题（执行迁移前必读）
> **嵌套分支名在本仓库会静默失败。** `git branch` / `git checkout -b` 对带 `/` 的名称（如 `task/foo`）返回 exit 0 但不写 ref 文件，导致分支变 unborn、提交落不了库。已验证结论：
> - ❌ `git branch task/foo 880b19e` → 静默失败（ref 不落盘，HEAD 仍 unborn）
> - ✅ `git branch task-foo 880b19e`（扁平名，连字符）→ 正常解析
> - ✅ 或手动 `mkdir -p .git/refs/heads/<dir> && printf '<sha>\n' > .git/refs/heads/<dir>/<name>`
>
> **强制约定**：本迁移所有分支一律用**扁平名**（连字符，不用 `/`）。任务分支命名见下表。

## 0. 迁移总基线（Step 0：✅ 已完成 2026-07-30）
1. ✅ 已创建分支 `wip-mixed-pre-refactor`（`b2635e4`，父 `880b19e`）并整体提交混合现场（排除 `build-*`/`__pycache__`/`.ui_headers_backup_*`/`Server/cloud.db`）。该提交**非交付物**，仅用于后续拆分。
   - 验证：`git log` 链 `b2635e4 → 880b19e → 55fe766`；`git diff --name-only HEAD` = 0（工作树干净）。
   - 注：此分支 ref 实际落在 `.git/refs/heads/wip/` 子目录（手工建目录后写入），属已知环境怪象；后续任务分支统一改用扁平名避免。
2. 主工作树保持冻结，**不清空、不 reset**（审计明确要求）。后续任务从 `880b19e` 切扁平分支，用 `git checkout wip-mixed-pre-refactor -- <paths>` 迁出文件。

## 1. 任务拆分与顺序（按风险从低到高）
| 序 | 任务组 | task_id | 建议分支 | allowed_paths（从混合提交取） | 受保护区注意 | 并入前证据门槛 |
|----|--------|---------|----------|-------------------------------|--------------|----------------|
| 1 | 治理设计归档 | T-2026-0730-GOV | task-gov-archive | `project-docs/governance/**`, `project-docs/decisions/ADR-012*` | 无 | L1（架构师文档评审） |
| 2 | REQ-AUTH-001 注销 | T-2026-0730-AUTH001 | task-auth001-logoff | `Client/file.*`, `Client/file.ui`, `project-docs/defects/BUG-REV-AUTH-004.md`, `project-docs/fix-specs/*logoff-fix*` | `.pro` 若改需批 | L3（测试师真实注销 E2E） |
| 3 | R17 低耦合心跳 | T-2026-0730-R17 | task-r17-heartbeat | `Client/msghandler.*`, `Server/mytcpsocket.*`, `Server/mytcpserver.*`, `task_prompt_heartbeat_*` | `protocol.h` 若改需批+符号 | L3（真实心跳联调；R06 现仅 L1） |
| 4 | R18 / BUG-ARC-PUBLINK-001 分享链接 | T-2026-0730-R18 | task-r18-publink | `Server/sharelinkservice.*`, `Server/*.sql`, `Client/sharelinkdialog.*`, `task_prompt_publink_*`, `project-docs/defects/BUG-ARC-PUBLINK-001.md`, `BUG-REV-PUBLINK-001.md`, 测试脚本 | `protocol.h` 若改需批+符号 | L3（真实 Client+Server+MySQL 端到端；R19 现仅 L2，且需先解决状态冲突） |
| 5 | UI / 构建产物 | T-2026-0730-UI | task-ui-build | `Client/bg_*`, `Client/style.qss`, `Client/folderselectdialog.*`, `Client/reconnectdialog.*` | `*.pro` 受保护，改需批 | L1（构建通过）+ 关联功能冒烟 |
| 6 | 杂项治理 | T-2026-0730-MISC | task-misc | 测试脚本迁入 `tests/`、`.gitignore` 增补、`项目面试文档.md` 归档 | 无 | L1 |

## 2. 执行流程（每个任务）
1. 在 `active-locks.yaml` 登记锁（锁定本任务 `allowed_paths`）。
2. 从 `880b19e` 切**扁平名**任务分支（如 `task-gov-archive`）：`git branch task-gov-archive 880b19e`（禁用 `task/...` 嵌套名，见 §0.5）。
3. `git checkout wip/mixed-pre-refactor -- <allowed_paths>` 迁出对应文件。
4. 工程师自检（构建/静态/单元，**不称正式测试通过**）→ 写 `engineer_commit`。
5. Bug 查找师静态审查 → 写 `review_commit`。
6. 测试师按 evidence 门槛动态测试 → 写 `test_commit` + 报告（含被测 commit 与等级）。
7. 集成者运行 `verify-integration-gate.py`（待起草）核对 7 项门禁 → 合并入主线、释放锁。

## 3. 关键风险与待决
- **protocol.h 双端已改**：当前混合改动里 `Client/protocol.h`、`Server/protocol.h` 都动了。必须先由架构师确认这次协议变更的必要性、登记 `allowed_symbols`，否则整组迁移都卡在门禁（受保护区）。
- **BUG-ARC-PUBLINK-001 状态冲突**：台账同时“已修复/待修复”，在 L3 证据补齐前不得标“已修复”；R18 任务并入门槛必须 L3。
- **R06/R19 证据不足**：R06 至多 L1、R19 至多 L2，现有“通过”表述需降级（见 evidence-levels.md），不得作为 R17/R18 的合并依据。
- **`.pro` 受保护**：UI 与任何涉及工程文件的改动触发架构师批准，避免绕过。
- **门禁脚本尚未落地**：`verify-integration-gate.py` 仍是设计参考；在它就绪前，集成者每次合并前必须手动核对门禁清单（integration-policy.md 第 3 节）。

## 4. 完成判据
- 主工作树恢复为单一干净基线，所有功能性改动都已落在独立任务分支并经门禁并入。
- 无任务残留跨组污染；`active-locks.yaml` 无悬空锁。
- 所有缺陷状态与 evidence-level 一致，无“已修复”配 L1/L2 的错配。
