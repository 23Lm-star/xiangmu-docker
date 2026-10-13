# INDEX.md — project-docs 导航索引

> 用途：人工浏览 `project-docs/` 大目录时的导航辅助（240+ 文件）。
> **不是会话开启依赖**：会话入口固定为 `pipeline/state.ps1` + 最新 handoff（+ `project-manifest.yaml`），
> 见 Development OS v1.7 / references/bootstrap.md「Canonical session entry」。

## 会话与流程入口（优先级最高）

| 文件/目录 | 内容 |
|---|---|
| `project-manifest.yaml` | 流程身份：workflow_version、trunk、sensitive_paths、test_gate |
| `governance/development-os-adapter.yaml` | v1.7 项目适配：工具链、敏感路径、管线入口、人工检查 |
| `handoffs/`（29 份） | 节点交接记录，`<任务ID>-<节点>-<日期>.txt`，最新的即当前状态 |
| `tasks/`（14 份） | 任务卡与节点骨架 |
| `governance/`（9 份） | 流程治理：README、connect-config-embedded、静态审查裁决等 |
| `requirements/`（22 份） | 需求记录（REQ-*） |

## 核心顶层文档

| 文件 | 内容 |
|---|---|
| `architecture.md` | 系统架构（ClientTask/ClientPDU + MyTcpServer/MyTcpSocket） |
| `code-map.md` | 代码地图（模块→文件定位） |
| `interfaces.md` | 公共接口契约 |
| `modules.md` | 模块划分 |
| `test-strategy.md` | 测试策略 |
| `requirements.md` | 需求总览 |
| `non-functional.md` | 非功能需求 |
| `业务功能文档.md` / `融合式扩展功能路线图.md` / `项目扩展前景.md` | 业务与扩展规划 |
| `init_test_data.sql` | 测试初始化数据 |
| `SOUL.md` | 项目协作约定 |

## 报告类目录（按性质检索）

| 目录 | 数量 | 内容 |
|---|---|---|
| `bug-reports/` | 21 | 缺陷报告（BUG-REV-*，含多轮复审 R01–R16） |
| `defects/` | 21 | 缺陷登记与跟踪 |
| `test-reports/` | 14 | 测试报告（R*-TST-*，含回归失败记录） |
| `decisions/` | 18 | 架构/门禁裁决（ADR-010~013、ARC 裁决） |
| `fix-specs/` | 20 | 修复规格 |
| `bug-fix-slove/` | 1 | 缺陷修复记录 |
| `evidence/` | 81 | 已归档证据（按 REQ-* 分目录；工作证据在 `build/evidence/`，不入 git） |

## 检索建议

- 查某任务全过程：`handoffs/` 按任务 ID 前缀过滤（如 `req-fs-003-*`），再进 `evidence/<REQ-ID>/`。
- 查某缺陷来龙去脉：`bug-reports/` → `fix-specs/` → `decisions/`（裁决）→ `test-reports/`（回归）。
- 查接口/协议变更是否受限：先看 `project-manifest.yaml` 的 `sensitive_paths`，再看 `interfaces.md`。
