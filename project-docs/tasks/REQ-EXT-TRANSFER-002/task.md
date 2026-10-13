# REQ-EXT-TRANSFER-002 下载暂停/取消与断点续传 — 任务记录

## 档位
- **LARGE**（goal 定档 2026-10-08）。理由（风险面，非行数）：
  1. **冻结面命中**：需向 `Client/protocol.h` 与 `Server/protocol.h` 追加下载续传起点/采纳起点两个 u64 字段（manifest `frozen_paths`）→ 按 v1.7 规则自动升 LARGE，改动须主人批准。
  2. **公共契约变更**：下载族 PDU 40/41 与群下载 105/106 的 caData 扩展（只追加，`0` = 旧语义，旧编号零改动）。
  3. **跨会话**：服务端（107 泛化 + 流起点 + 上限判定交互）+ 客户端（暂停/续传/取消状态机 + 快照 + 面板）分属两个独立会话面。
  4. **热路径**：下载流式状态机核心改造，回归覆盖全部既有 12 套 e2e（其中 `parallel_e2e` 54 条是 id=0 旧路径与多文件并行的双重锚点）。
- 需求文档 `project-docs/requirements/REQ-EXT-TRANSFER-002-download-pause-cancel-resume.md` 自声明 LARGE；本记录与之一致，不再改档。
- 流程版本：Development OS v1.7。分支：`feat/req-ext-transfer-002-download-pause-resume`，基线 `master@d85e39f`。
- 节点计划：goal（本记录）→ design（逐字节落位 + 错误码 + 测试矩阵 → `design.md`）→ **主人批准契约变更** → development → independent test（新会话）→ repair（按需）→ independent audit（新会话、只读）→ integration（三事实行）→ merge no-ff。
- `owner_agent: claude-code`。

## 依赖核查（2026-10-08 实测，非文档转述）
| 依赖 | 状态 |
|---|---|
| REQ-EXT-TRANSFER-001 | ✅ merged `d2148bf`（祖先链含于 `d85e39f`）；TransferManager + per-transfer 状态表 + 下载 ring + 107/108 均在线上 |
| REQ-EXT-FILE-001 / 002 | ✅ merged；对象仓 `file_object.storage_path` 为下载源，个人续传不绕该层 |
| REQ-EXT-GROUP-003 | ✅ merged；105/106 群下载元数据响应体已带 size/name |
| 回归运行形态 | 已统一；12 套件按 E2E-RUNFORM 跑，`_harness/server_ctl.py` 管生命周期 |
| 并行冲突面 | 无活动开发区；`master@d85e39f` 为唯一基线 |

**复用盘点（代码事实）** — 本需求零新建设施：
- 107/108 请求-响应对已在线上且回带 transfer_id（`Server/mytcpsocket.cpp:392-415`）→ 取消/暂停无需新 PDU。
- 40 的 caData[32..47] 与 105 的 caData 除 [48..55] 外全空、41 的 caData[24..31] 与 106 同 → 起点字段零重排。
- `mkPDU` 全量 `memset` → 旧客户端不写该区间时**必为 0**，「0 = 从头」由构造保证。
- 快照框架 + 「恢复为 Paused」语义（上传已验证）。

## 用户决策（2026-10-08，AskUserQuestion 两项确认）
1. 目标层 = **下载暂停/取消 + 下载续传**（多文件并行已实现并合并，不作为本次目标；单文件分片并行与既有实现复核两项未选）。
2. 档位 = **由 AI 按事实定档** → 结论 LARGE（冻结面命中，理由见上）。

## 契约影响（须主人批准；批准卡随 design.md 提交）
| 面 | 变更 | 性质 |
|---|---|---|
| `Client/protocol.h` + `Server/protocol.h`（frozen） | 追加常量块（4 行）：40/105 请求 caData[32..39] `DOWNLOAD_RESUME_OFFSET`（u64）、41/106 响应 caData[24..31] `DOWNLOAD_ACCEPTED_OFFSET`（u64） | **只追加**，追加在 REQ-EXT-TRANSFER-001 常量块之后；不得插入其内部（`parallel_e2e.py` P1-frozen-block 正则终点） |
| 既有 PDU 编号 / 上传族语义 / id=0 旧路径 | **零改动** | 旧客户端与全部既有 e2e 脚本零改动通过 |
| 106 响应 caData 契约 | 由「全空」改为「[24..31] = 采纳起点」；其余区间仍全零 | 旧客户端不读该区间 |
| DB schema | **零变更**、无迁移 | 附加值 |

## 门次与事件（append-only）
- 2026-10-08 goal：分支自 `master@d85e39f` 切出。需求文档落盘。**目标层裁决**：多文件并行（REQ-EXT-TRANSFER-001）已在 master（`d2148bf`）——用户确认本需求不重复该项，改取其自声明非目标「下载暂停/取消 + 下载续传」。实施方案 = 最小增量：107 泛化释放下载槽 + 40/105 携带续传起点 + 41/106 回带采纳起点。现状缺陷取证 8 条（G1-G8，含 cancel 不发 107、pause 对下载早退、失败即删分片、快照注释自述整文件重传）。定档 LARGE（冻结面命中）。契约变更批准卡待 design 节点提交。
- 2026-10-08 design：`design.md` 落盘（D1-D10 裁决、F1-F7 冻结、§6 测试矩阵 3 静态 + 14 动态 + 8 GUI + 12 套回归、§9 逐字批准稿、§11 批准记录）。契约变更经主人**批准**，`design_status: CONFIRMED`。
- 2026-10-08 development：提交 `f820e61`（实现 + 套件 + manifest）与 `d45ae11`（S3 签名收紧，行为等价）。冻结面双端各 +4 行逐字节 == §9 批准稿、零删除零重编号（`audit.ps1` 仍为骨架 → 边界以 `git diff d85e39f..d45ae11` 人工核对 + 套件静态面 S-01..03 复证）。服务端 S1-S4、客户端 C1-C10 全部落地。新套件 `transfer002`（25 断言：静态 3 + 动态 19 + 环境 3）注册进 `test_gate.suites`（共 13 套）。build GREEN @d45ae11；test 门册 waived → 13 套手动串行：11 绿（transfer002 25/25 三轮幂等、parallel 54/54 零改动），group-002/unique-login 两红为既有且当场复现归因。**发现并修复 dist 部署链缺陷**：`artifacts.ps1 -Deploy` 会以 `build/*/release` 的陈旧 exe+DLL（Qt 5.9.5）覆盖 dist 并错误记溯源——本会话 Server 0xC0000139 的根因；dist 已重建为 5.14.2 运行库并复验全绿，脚本本体修复建议另立 SMALL（repo contract 不在本任务内动）。GUI 8 项 NEEDS-HUMAN 待主人一轮。
- 证据：`project-docs/evidence/REQ-EXT-TRANSFER-002/development/pipeline-2026-10-08.md`（本地，.gitignore 合同）；handoff `req-ext-transfer-002-development-2026-10-08.txt`（校验 OK）。

## 下一节点
independent test（新会话：干净 detached worktree @`d45ae11`，自跑全套——build + 13 套套件——只报告不改产品源码/验收标准）→ repair（按需）→ independent audit（新会话、只读）→ integration 三事实行 → merge no-ff。主人一轮 GUI 8 项（design §6.3）挂测试节点后完成。
