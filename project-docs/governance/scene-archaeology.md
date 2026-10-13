# 现场考古清单（Scene Archaeology Manifest）

> 生成日期：2026-07-30
> 目的：对当前共享主工作区（`E:\qt\xiangmu`）的混合变更按“疑似来源任务”分组，为后续隔离迁移提供拆分依据。
> 现状：`git status` 显示 **32 个已修改跟踪文件 + 43 个未跟踪文件**，全部混在同一工作树，未提交、未拆分。
> 原则：本清单只做**分类与标注**，不移动、不删除、不提交任何文件。迁移前每一项都需按 `task-template.yaml` 建契约并重新审计。

## 0. 受保护文件（越界红旗，必须架构师单独放行）
| 文件 | 本次状态 | 风险 |
|------|---------|------|
| `Client/protocol.h`、`Server/protocol.h` | 已修改（双端） | 协议/PDU 契约，属 `protected_paths`；任何改动须 `allowed_symbols` + 架构师批准 |
| `Server/operatedb.cpp`、`Server/operatedb.h` | 已修改 | 数据访问层，受保护；涉及 DB Schema/迁移 |
| `*.pro`、`Makefile*` | 已修改/新增 | 构建配置，默认受保护 |
| `Server/cloud.db` | 未跟踪新增 | 数据库文件被纳入仓库，应移出版本控制或明确归属 |

---

## 1. R17 低耦合心跳机制（心跳）
| 文件 | 状态 | 备注 |
|------|------|------|
| `Client/msghandler.cpp`、`Client/msghandler.h` | 已修改 | 心跳客户端逻辑 |
| `Server/mytcpsocket.cpp/.h`、`Server/mytcpserver.cpp/.h` | 已修改 | 心跳服务端逻辑 |
| `task_prompt_heartbeat_client.txt`、`task_prompt_heartbeat_server.txt` | 未跟踪 | 任务提示词 |
| `project-docs/fix-specs/R17-*-ARC-低耦合心跳与应用内分享链接-工程实施规格-*-评审.md` | 未跟踪 | 实施规格 |
| 关联：`project-docs/test-reports/R06-*-TST-Heartbeat_ShareLink_Regression.md`（至多 L1） | 未跟踪 | 证据不足，见 evidence-levels.md |

## 2. R18 / BUG-ARC-PUBLINK-001 分享链接安全加固（分享链接）
| 文件 | 状态 | 备注 |
|------|------|------|
| `Server/sharelinkservice.cpp/.h`、`Server/migrate_publish_links.sql`、`Server/share_link.sql` | 未跟踪新增 | 分享链接服务 + 迁移 |
| `Client/sharelinkdialog.cpp/.h/.ui` | 未跟踪新增 | 分享链接 UI |
| `task_prompt_publink_handler.txt`、`task_prompt_publink_server.txt` | 未跟踪 | 任务提示词 |
| `project-docs/fix-specs/R18-*-ARC-BUG-ARC-PUBLINK-001-安全加固与UI.md` | 未跟踪 | 实施规格 |
| `project-docs/defects/BUG-ARC-PUBLINK-001.md`、`BUG-REV-PUBLINK-001.md` | 未跟踪 | 状态冲突（已修复/待修复并存） |
| `regression_publink_test.py`、`tier2_publink_simulation.py` | 未跟踪 | 测试脚本（含 SQLite 模拟） |
| `project-docs/test-reports/R19-*-TST-PUBLINK_Regression.md` | 未跟踪 | 至多 L2（模拟，非真实端到端） |

## 3. REQ-AUTH-001 注销（账户删除）
| 文件 | 状态 | 备注 |
|------|------|------|
| `Client/file.cpp/.h`、`Client/file.ui` | 已修改 | 注销按钮/界面 |
| `task_prompt_req_auth_001.txt` | 未跟踪 | 任务提示词 |
| `project-docs/defects/BUG-REV-AUTH-004.md` | 未跟踪 | 注销相关缺陷 |
| `project-docs/fix-specs/BUG-REV-AUTH-004-*-ENG-logoff-fix.md` | 未跟踪 | 注销修复规格 |
| 已提交历史：`feat(auth): implement REQ-AUTH-001 account deletion (注销)`（880b19e 等） | 已提交 | 基线提交，需确认与未提交改动的关系 |

## 4. UI / 构建产物（易污染、需谨慎）
| 文件 | 状态 | 备注 |
|------|------|------|
| `Client/bg_index.jpg`、`Client/bg_main.jpg`、`Client/style.qss` | 未跟踪新增 | 皮肤/背景资源 |
| `Client/folderselectdialog.cpp/.h`、`Client/reconnectdialog.cpp/.h` | 未跟踪新增 | 新对话框 |
| `Client/Client.pro`、`Client/Client.pro.user`、`Server/Server.pro`、`Server/Server.pro.user` | 已修改 | Qt 工程文件（受保护） |
| `Client/Makefile.debug.Debug`、`Client/Makefile.debug.Release` | 未跟踪新增 | 构建产物，应 gitignore |

## 5. 治理文档 / 架构师产出（相对安全，但仍需按契约归档）
| 文件 | 状态 | 备注 |
|------|------|------|
| `project-docs/architecture.md`、`code-map.md`、`interfaces.md`、`modules.md`、`non-functional.md`、`requirements.md` | 已修改 | 架构/接口/需求文档 |
| `project-docs/decisions/README.md`、`defects/README.md`、`test-reports/README.md` | 已修改 | 索引文档 |
| `project-docs/decisions/ADR-010-*.md`（功能基线恢复）、`ADR-011-*.md`（低耦合心跳与分享链接） | 未跟踪 | 新决策记录 |
| `project-docs/defects/BUG-ARC-RECOVERY-001.md`、`BUG-REV-DB-005.md`、`BUG-REV-MEM-001.md`、`BUG-REV-MEM-002.md` | 未跟踪 | 新缺陷记录 |
| `project-docs/bug-reports/R07-*-REV-数据库连接池功能静态审查.md` | 未跟踪 | 静态审查报告 |
| `project-docs/变更状态同步.md`、`项目面试文档.md`（根） | 未跟踪 | 状态/面试文档 |
| 已删除：`project-docs/handoff-architect-to-engineer-R03-*.md`、`handoff-architect-to-engineer-REQ-AUTH-001-*.md`、`handoff-architect-to-tester-*.md` | 已删除 | 交接文档被移除，需确认是否应保留 |

## 6. 测试脚本与杂项（位置不规范）
| 文件 | 状态 | 备注 |
|------|------|------|
| `smoke_test.py`、`regression_publink_test.py`、`tier2_publink_simulation.py` | 未跟踪 | 应归入 `tests/` 而非仓库根 |
| `__pycache__/` | 未跟踪 | 应 gitignore，不应入库 |
| 根目录 `项目面试文档.md`（53KB） | 未跟踪 | 非治理文档，建议移出或归档 |

---

## 迁移前的待办（由集成者/用户执行）
- [ ] 对每个分组建 `task-template.yaml` 契约（含 base_commit、allowed_paths、protected_paths、approvals）。
- [ ] 确认 `protocol.h` / `operatedb.*` 改动的架构师批准与 `allowed_symbols`。
- [ ] 将测试脚本迁入 `tests/`，`__pycache__/` 与构建产物加入 `.gitignore`。
- [ ] 对 R06/R19 按 evidence-levels.md 重新定级，修正 BUG-ARC-PUBLINK-001 冲突状态。
- [ ] 仅将通过重新审计的最小 patch 移入隔离 worktree；最后再决定是否清理共享主工作区。
