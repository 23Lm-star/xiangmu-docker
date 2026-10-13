# REQ-EXT-FILE-002 顺序上传秒传与去重 — 任务记录

## 档位
- **LARGE**（goal 定档 2026-10-01）。理由（风险面，非行数）：
  1. **冻结面命中**：需向 `Client/protocol.h` 与 `Server/protocol.h` 追加 PDU 枚举（manifest `frozen_paths`）→ 按 v1.7 规则自动升 LARGE，且改动须主人批准。
  2. **公共契约变更**：上传初始化协议（新 PDU + 新字段布局），另加 `upload_session` 表加列 + schema 版本提升。
  3. **跨会话**：客户端（摘要预计算/新分派）+ 服务端（预检查/命中事务/复算对照）+ DB 迁移，单会话放不下。
  4. **热路径**：改动上传状态机入口（新增命中分支），并新增服务端复算对照——登记已有对象所有权，回归面覆盖上传/下载/续传/分享/路径防护。
- 需求文档 `project-docs/requirements/REQ-EXT-FILE-002.md` 自声明 LARGE；本记录与之一致，不再改档。
- 流程版本：Development OS v1.7。分支：`codex/req-ext-file-002-fast-upload`，基线 `master@1123a98`。
- 节点计划：goal（本记录）→ design（`design.md` 随本记录落分支）→ **主人批准契约变更** → development → independent test（新会话）→ repair（如需）→ independent audit（新会话、只读）→ integration（三事实行）→ merge no-ff。
- `owner_agent: claude-code`（分支名沿用需求文档给定前缀 `codex/`，owner 以本行为准）。

## 依赖核查（2026-10-01 实测，非文档转述）
| 依赖 | 状态 |
|---|---|
| REQ-EXT-SEC-001 | ✅ merged；连接级 actor（`m_actorName`）、`pathInActorRoot`、`logicPathInActorRoot`、`isPublicRequestType` 门可用 |
| REQ-EXT-FILE-001 | ✅ merged `master@9faae6c`；四表 + `schema_version` + `FileObjectStore`（`hashStaging`/`commitUpload`/`attachEntry`/`releaseEntry`/`normalizeParent`/`logicPathInActorRoot`）+ `upload_session` CRUD + `migrateLegacyFiles` 全部在主干 |
| REQ-EXT-GROUP-002 | ✅ merged `master@ffaaacf`，本分支基线 `1123a98` 已含 |
| 并行冲突面 | 无活动开发区；现存分支均为已合并遗留（`codex/req-ext-group-001|002`、`small/*`）。本分支为唯一活动开发区 |

**复用盘点（代码事实，master@1123a98）** — 下列能力已存在，本需求只做接线与补强，不重写：
- `Server/migrate_ext_file_001.sql:41` `upload_session` 已含 `client_sha256 CHAR(64) NULL`（注释已声明"仅预检元数据，不作转正依据"）——秒传预检查的落点已备。
- `file_object.uk_sha256` **唯一键已存在**（`migrate_ext_file_001.sql:23`）——物理对象唯一性由 DB 保证，非逻辑约定。
- `FileObjectStore::commitUpload`（`Server/fileobjectstore.cpp:191`）已实现：对象 `FOR UPDATE` 命中 → 同名安置（`(1)` 递推）→ 条目建/复活 → 引用计数，**同一事务**；命中既有对象时丢弃 staging，物理只留一份。
- `ResumableUploadSession::beginStaged` 确定性 staging 路径（断点续传基础）；`OperateDB::{createUploadSession,updateUploadOffset,finishUploadSession}` 已就位。
- 缺口（本需求要补）：① 客户端从不计算摘要（`Server/msghandler.cpp:1162` 注释"客户端不提交摘要"）；② `commitUpload` 只比对**大小**，不比对客户端声明摘要；③ 无 PDU 层预检查入口；④ 无请求级幂等键。

## 契约影响（须主人批准）
| 面 | 变更 | 性质 |
|---|---|---|
| `Client/protocol.h` + `Server/protocol.h`（frozen） | 追加 `ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_REQUEST=97` / `_RESPOND=98`（现 max=96，**只追加不重编号**）+ 新字段偏移/magic 常量 | 已批准变更，见批准卡 |
| `upload_session` 表 | `ADD COLUMN client_request_id VARCHAR(64) NULL`、`ADD COLUMN file_id BIGINT NULL`、`ADD UNIQUE KEY uk_req (owner_name, client_request_id)` | 附加式（additive），NULL 行不冲突 |
| `schema_version` | 新增 `2026100101`，并入启动 `required` 计算 | 低版本启动 fail-fast（沿用 T1 语义） |
| 既有 PDU 编号 / 分块协议语义 | **零改动**（旧 `UPLOADFILE_INIT` 路径保留可用） | 回归面零破坏 |

## FILE-001 遗留事项核对（逐条定性，不整包接收）
| # | FILE-001 移交项 | 是否在本分支触碰面 | 处置 |
|---|---|---|---|
| L1 | `/filesys/<owner>/…/` 旧行被 `isLogicKeyForm` 判为 canonical → 静默保留、数据对查询不可见（判别盲区） | **是**（秒传目标目录解析必经 `normalizeParent`；`upload_session.parent_path` 为新写入域） | **本分支收口**：判别收紧为"不含 `:` 且首段不等于 fs 根 basename" |
| L2 | 带盘符 + 尾部斜杠旧行的告警泄漏（FILE-001 明文指派 FILE-002 收紧） | **是**（同一函数、同一列） | **本分支收口**：与 L1 同一处收紧，补夹具 |
| L3 | 路径 >191 字符旧文件记 `failed` 不入映射 | 否（存量迁移域） | 保持台账，不扩范围 |
| L4 | 未建目录条目的旧目录改名/移动后仅前缀级联 | 否（存量迁移域） | 保持台账；验收 5 回归时留意 |
| L5 | 隐藏文件不入迁移扫描 | 否 | 保持台账 |
| L6 | `claim-check` 缺 `manifest.yaml`；`validate-handoff.py` NODES 无 `independent-test` | 工具缺口，非产品 | 不属本分支；测试节点沿用 `node: test`，并在测试报告注明 |
| L7 | 32 字节展示名 + `(1)` 后缀溢出 | 否（`clampUtf8`/`splitName` 已实现） | 列为验证项，不重做 |
| L8 | F-6 并行节点错峰（共享 5000 端口 / MySQL / 整机 taskkill） | 环境纪律 | 本分支遵守：同一时刻单一活动开发区 |
| L9 | session-001 的 E7a 台账 | 他人需求面 | 不属本分支 |

> 说明：L1/L2 是同一函数的同一处判别缺陷，属"本需求自身触碰面内的既有缺陷"，非无关缺陷——按 v1.7「无关缺陷另开 SMALL 分支」不适用；若设计阶段实测证明其修复超出该函数局部（如需改 SQL 谓词语义），则降级为独立 SMALL 分支并记入台账。

## 门次与事件（append-only）
- 2026-10-01 goal：定档 LARGE，依赖四项实测通过，FILE-001 遗留 L1/L2 纳入本分支，契约变更批准卡提交主人。
- 2026-10-01 **主人批准**（原话"同意（推荐）"）：批准范围为设计 §1 的协议追加（`protocol.h` 两端新增 `UPLOADFILE_FASTCHECK_REQUEST=97` / `_RESPOND=98`，只追加不重编号 + 新偏移/magic 常量）与 §3 的 `upload_session` 加三列（`client_request_id`、`file_id`、`uk_req`）+ `schema_version` 提升至 `2026100101`。批准即签核 design（`design.md`）。**不含**：改动既有 PDU 编号、既有分块协议语义、新增外部依赖。
- 2026-10-01 development：代码提交 `c452eb3`（主工作树，唯一活动开发区）。交付见 `project-docs/evidence/REQ-EXT-FILE-002/development-report.md`。证据：新建 `fastupload_e2e.py` 46/46（dist 形态 + Server-cwd 形态各一次，重跑稳定）；FILE-001 四套件 13/43/37/48 全绿（旧 INIT 路径零破坏）；build GREEN；`audit.ps1` RED 项为预期冻结签名（判读口径见报告 §4.2）。
- 2026-10-01 development 偏差与发现（待测试/审计复核，报告 §4）：
  - **DEV-1（偏差，需主人知悉）**：`schema_version` 取 `2026100102`——`2026100101` 已被 GROUP-002 占用，同名会让 fail-fast 失效（库缺列仍通过启动检查）。仍属"新增版本号并入 required"的批准范围；`design.md:87`、本记录 2026-10-01 批准行的数值应更正为 `2026100102`。
  - **FINDING-DEV-1**：`pipeline/audit.ps1` 以 PS5.1 默认编码读 UTF-8 manifest，中文注释被破坏后只解析出**第一个**冻结路径（`Server/protocol.h` 未纳入冻结检查）。pipeline 脚本属仓库契约，本分支不改，另开 SMALL 任务修。
  - **FINDING-DEV-2**：FILE-001 四套回归依赖部署前运行形态（`filesPath=./filesys` + `cwd=Server` + exe 在 `build/build-Server/release/`），与现行绝对路径配置冲突；本次以临时相对配置复现原形态跑绿并已还原，建议另开 SMALL 任务统一运行形态。
  - **FINDING-DEV-3**：L1/L2 本分支只收口"判别"面（`isLogicKeyForm`）；`normalizeLogicKeys` 的 SQL 选择谓词仍把 `/filesys/<owner>/…` 视为 canonical，存量该形态行不会被转换（数据面未收口；当前库实测无此类行）。如需补谓词，另开 SMALL 任务并降级处理。
  - **FINDING-DEV-4**：验收 5 的"过期临时会话清理"在代码库本无调度器（FILE-001 亦无）；本节点不新增清理器，改为验证不变式（清理后对象/条目零影响）。
  - **FINDING-DEV-5**：`MKDIR` 非幂等（目录已存在回 `ret=0`）；用户根仅首次注册时创建——既有行为，仅记录。

- 2026-10-01 **independent test**：全新会话，detached worktree @ `c452eb3`（只读），从零构建并独立重跑。**结论 PASS**（0 条归因候选缺陷）：`fastupload_e2e.py` 46/46（dist 形态 ×2 + Server-cwd 形态 ×2，重跑稳定）；FILE-001 四套件 13/43/37/48 全绿；build GREEN；`test.ps1` honest-0；`audit.ps1` RED = **两个** frozen 命中（Client + Server/protocol.h），判为批准范围内预期签名。详见 `project-docs/evidence/REQ-EXT-FILE-002/independent-test/report.md`。
  - **偏差复核**：`schema_version=2026100102`（`operatedb.h:22` GROUP-002=2026100101、`:26` FILE-002=2026100102；`operatedb.cpp:96-98` 并入 required）——事实成立、属批准范围，`design.md:87` 数值应更正。
  - **FINDING-DEV-1 判为不可复现**：本机 PS5.1（默认 gb2312 / ACP 936）下，默认、`-Encoding Default`、cmd `chcp 936`、`chcp 65001`、LF/CRLF 六种组合，`audit.ps1` **均**解析出两条冻结路径并报两个 HIT；dev 的 `run-logs/audit.log` 仅一项，与当前环境不符。副结论：冻结检查实际覆盖 `Server/protocol.h`。待 dev 提供确切调用环境或撤回。
  - **FINDING-DEV-2/3/4/5 复核**：全部确认（2 属运行形态，本轮已复现其假红并规避；3 代码事实成立且当前库 `LIKE '/filesys/%'` 计数=0，潜伏未触发；4 无清理调度器；5 `MKDIR` 非幂等）。

- 2026-10-01 **independent audit**：新会话、detached 只读 `c452eb3`。**结论 PASS（审计）**：批准契约精确落地；冻结面三项（只追加不重编号 / 两端常量块逐字节一致 / 无既有 PDU 语义变更）独立复核通过；**0 条归因产品缺陷**。`schema_version=2026100102` 偏差判为批准范围内。**FINDING-DEV-1 推翻测试节点「不可复现」**：F1 事实成立（manifest LF + 全角 `）` 吞换行 → ACP936 主工作树只解析出 `Client/protocol.h`；全新 checkout 为 CRLF 则两项都解析出）。F1 属 `pipeline/audit.ps1` 仓库契约缺陷（medium），另开独立 SMALL。独立性声明：owner_agent 同为 `claude-code`（未满足跨代理默认，报告 §0 已声明限制）。详见 `independent-audit/report.md`。审计结论经主人接受（`state.ps1` 记 `accepted by user, 2026-10-01`）。
- 2026-10-01 **integration（本节点）**：三事实行逐项自证齐备 → 合并 `master`，提交 `abe2e5d`（no-ff，零冲突）。① 审计报告 PASS 且被接受；② 冻结面 diff 仅追加 97/98（旧 1–96 零改动，两端常量块逐字节一致），与批准清单相符；③ 合并态 `abe2e5d` 上 `build.ps1` GREEN、`test.ps1` GREEN-WARN(0)、`audit.ps1` PASS。任务状态 `COMPLETED`。报告 `evidence/REQ-EXT-FILE-002/integration/report.md`，handoff `handoffs/req-ext-file-002-merged-2026-10-01.txt`。

## 下一节点
无（流程收口）。push 待主人明示。遗留 SMALL 候选（不阻塞）：F1+F2（`pipeline/audit.ps1` 冻结面解析 + impact face）、FINDING-DEV-2（统一 e2e 运行形态）、FINDING-DEV-3（`normalizeLogicKeys` 数据面谓词收口）。观察项：残留 worktree `build/wt-test-group001-repair` 含未提交 GROUP-001 handoff，待主人裁决。
