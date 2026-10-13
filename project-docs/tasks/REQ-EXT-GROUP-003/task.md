# REQ-EXT-GROUP-003 群文件目录与访问控制 — 任务记录（goal 节点，2026-10-01）

## 档位
- **LARGE**（goal 定档 2026-10-01）。理由（风险面，非行数）：
  1. **冻结面命中**：需向 `Client/protocol.h` 与 `Server/protocol.h`（manifest `frozen_paths`）追加群文件 PDU → v1.7 自动升 LARGE，且改动须主人批准。
  2. **公共契约变更**：新增群文件协议段（PDU 99–106）+ 新表 `group_file` + `schema_version` 提升（新增独立版本号）。
  3. **跨会话**：客户端（群文件目录/上传/下载/删除 UI + 新分派）+ 服务端（新入口 `groupFileRequest` / 权限判定 / 对象仓挂接）+ DB 迁移，单会话放不下。
  4. **权限与路径安全热路径**：复用文件资产层（`file_object`/`user_file`/`FileObjectStore`）与上传下载链路，验收覆盖越权（退出/被移除即时失权）、解散写拒绝、路径负向面——回归面覆盖 GROUP-001/002、FILE-001/002 与一对一聊天。
  5. 需求文档 `project-docs/requirements/REQ-EXT-GROUP-003.md` 自声明 LARGE；本记录与之一致，不再改档。
- 流程版本：Development OS v1.7。分支：`codex/req-ext-group-003-group-files`，基线 `master@60a5fc9`。
- 节点计划：goal（本记录 + `design.md`，本会话，不动生产源码）→ **主人批准契约变更** → development → independent test（新会话）→ repair（如需）→ independent audit（新会话、不同代理、只读）→ integration（三事实行）→ merge no-ff。
- `owner_agent: claude-code`（分支名沿用需求文档给定前缀 `codex/`，owner 以本行为准）。

## 依赖核查（2026-10-01 goal 节点实测，逐条以代码/DB 事实确认，非文档转述）
| 依赖 | 实测事实（本树 / 本库） | 结论 |
|---|---|---|
| REQ-EXT-SEC-001 连接级 actor | `MyTcpSocket::handlMsg` 以 `isPublicRequestType` 白名单门控，未认证走 `authRejectPdu`（`mytcpsocket.cpp:53/60`）；群请求类型不在白名单即自动要求 `m_actorAuthenticated`，handler 用 `m_actorName` 覆盖自称字段 | ✅ 可用 |
| REQ-EXT-GROUP-001 成员事实源 | `group_member` 存活事实：主键 `(group_id,user_name)`、`role('owner'/'admin'/'member')`、`status('active'/'left'/'removed')`、`last_read_seq`、索引 `idx_group_member_user_group(user_name,group_id)`、`idx_group_member_status(group_id,status)`（DB `SHOW COLUMNS` 实测一致）；`group_info.status('active'/'dissolved')` | ✅ 可用 |
| GROUP-001 D5 解散语义 | `handleGroupOperation`：解散 = `UPDATE group_info SET status='dissolved'`（operatedb.cpp:1577）；群消息门判定 `gi.status='active'`（operatedb.cpp:1655，非 active 回 -2102）——**写操作拒绝已成型，本需求照此对群文件上传/删除生效；只读历史仍放行** | ✅ 语义可复用 |
| REQ-EXT-GROUP-002 群消息 PDU 88–96 | `Server/protocol.h:116-124` 88–96 已在位；群信封 `GROUP_PROTOCOL_VERSION=1`/`GROUP_ENVELOPE_LENGTH=20`、`groupAppendU16/U32/U64/String`、`groupRead*` 就位（`groupRequest` msghandler.cpp:1786）；事件伪造拒绝（mytcpsocket.cpp:258-264） | ✅ 复用信封 |
| REQ-EXT-FILE-001 文件资产层 | 四表实测在位：`file_object(uk_sha256,refcount,status ready/pending_reclaim)`、`user_file(uk_loc,is_dir,object_id FK,status active/deleted)`、`upload_session`、`file_migration_map`；`FileObjectStore`（`hashStaging`/`commitUpload`/`attachEntry`/`attachExistingObject`/`releaseEntry`/`normalizeParent`/`logicPathInActorRoot`）；`schema_version` 表在位 | ✅ 可用 |
| REQ-EXT-FILE-002 PDU 97/98 秒传 | `Server/protocol.h:127-128` 97/98 在位；`upload_session` 加列实测：`client_request_id VARCHAR(64) NULL`、`file_id BIGINT NULL`、唯一键 `uk_req(owner_name,client_request_id)` | ✅ 可用（本需求对群上传**不复用** 97/98，见 design §1.3） |
| `schema_version` 现值 | DB 实测：`2026092901`(FILE-001)、`2026093001`(GROUP-001)、`2026100101`(GROUP-002)、`2026100102`(FILE-002)；`checkSchemaVersion()` 取 `MAX(version)` 比对四常量 `qMax`（operatedb.cpp:96） | ✅ 新版本号须独立且更大 |
| PDU 编号水位 | `Server/protocol.h` 最大业务枚举 = **98**，`ENUM_MSG_TYPE_MAX=99999`；群生命周期 60–87、群消息 88–96、文件 97–98 | ✅ 群文件从 **99** 起追加 |
| master 基线整合面 | 实测**不存在** `small/normalize-logic-keys-predicate` 分支（`git branch -a` 无此名；`git log --all` 无对应提交）；当前唯一 master 头 `60a5fc9`，工作区 clean | ✅ 基线无在飞冲突；集成前仍会将 master 合入本分支吸收任何新到的 SMALL |

**结论：依赖全部满足。开发启动条件 = 主人批准契约变更（本文档"契约影响"批准卡）+ 从 master 切分支（已完成）。**

## 契约影响（须主人批准 · 业务语言批准卡见文末）
| 面 | 变更 | 性质 |
|---|---|---|
| `Client/protocol.h` + `Server/protocol.h`（frozen） | 追加群文件 PDU **99–106**（`GROUP_FILE_LIST/ UPLOAD_INIT/ DELETE/ DOWNLOAD` 四对，**只追加不重编号**）+ 群文件偏移常量块（两端逐字节一致） | 冻结面变更 |
| 新表 `group_file` | 群文件逻辑关联（群、逻辑文件、上传者、展示名/大小快照、状态、时间）+ 唯一键与索引 | 附加式（新增表，不动旧表） |
| `schema_version` | 新增独立版本号 **`2026100103`** 并入 `required` 计算 | 低版本启动 fail-fast |
| 数据面传输 | 群上传复用既有分块 `UPLOADFILE_DATA`(31/32)；群下载复用既有分块 `DOWNLOADFILE_DATA/FINISH`(42/43)，源由服务端 `group_file→object_id→storage_path` 解析 | **既有分块语义零改动** |
| 既有 PDU 编号 / 既有语义 / 既有表 | **零改动**（97/98 秒传、29–32 上传、40–43 下载、群 60–96 全部保留原样） | 回归面零破坏 |

> 批准前**不得写实现代码**。

## 数据模型（详见 `design.md` §3）
- `group_file`：`group_file_id` 主键；`group_id`；`file_id`（→`user_file.file_id`，**逻辑文件标识**，可空）；`object_id`（→`file_object.object_id`，物理对象，下载/去重/引用计数入口）；`uploader_name`；`display_name`（群目录展示名**快照**，与上传者个人根解耦）；`size_bytes`（快照）；`status('active'/'removed')`；`created_at`/`updated_at`。唯一键 `uk_group_file_name(group_id, display_name)`；索引 `(group_id,status)`、`(group_id,uploader_name)`。
- **引用计数扩展**：`file_object.refcount` 语义扩展为"`user_file`(active) ∪ `group_file`(active) 的活跃逻辑引用数"；建群文件引用 +1、删群引用 −1；归零转 `pending_reclaim`（D3：绝不物理删）。**删除一个群引用不影响其他用户/群的同一物理对象**（验收 4）。

## 不做的事 / 停止条件
- **不做**（需求 §5 明文排除）：协作编辑、文件锁、版本管理、批量打包下载、群文件消息卡片、复杂 `owner/editor/viewer` 权限；群目录**子目录**（首版扁平群根，需求允许"受控相对路径"，扁平即可满足验收）；新外部依赖；`Server/filesys/` 入库；改动既有 PDU 编号或既有分块协议语义。
- **停止条件**（需求 §5）：① 退出/踢出后的访问撤销**不是服务端即时判定**（若靠客户端或缓存）→ 停止合并且回 goal；② 群文件**仍依赖裸客户端路径拼接**（非 `group_file_id→object_id→服务端 storage_path`）→ 停止合并且回 goal；③ 协议新旧版本无法明确拒绝未知类型 → 停止；④ 基线出现与本盘点冲突的事实 → 回 goal 重估，不在开发节点即兴扩面。

## 允许路径
`Client/protocol.h`、新增 `Client/groupfile.{h,cpp}`（或扩展 `Client/group.{h,cpp}`）、`Client/msghandler.{h,cpp}`、`Client/client.{h,cpp}`、`Client/index.{h,cpp,.ui}`、`Client/Client.pro`、`Server/protocol.h`、`Server/msghandler.{h,cpp}`、`Server/mytcpsocket.{h,cpp}`（入口门 case + 事件拒绝）、`Server/operatedb.{h,cpp}`、`Server/fileobjectstore.{h,cpp}`、`Server/server.{h,cpp}`（版本检查文案）、`Server/main.cpp`（迁移挂钩点）、`Server/Server.pro`、`Server/migrate_ext_group_003.sql`、`Server/migrate_ext_group_003_rollback.sql`、`project-docs/tasks|evidence/REQ-EXT-GROUP-003/`、handoff。
**禁止**：改既有枚举编号/删字段；改既有 PDU 语义；写 `user_file`/`file_object` 既有语义分支（仅经 `FileObjectStore` 窄接口扩展）；新外部依赖；`Server/filesys/` 入库；群文件之外的功能扩面（群公告/消息卡片等）。

## 门次与事件（append-only）
- 2026-10-01 **goal**：定档 LARGE。依赖 8 项逐条以代码/DB 实测通过（见上表）。产出 `task.md` + `design.md`（协议 99–106 逐字段/偏移、`group_file` DDL、权限判定点、路径负向面、验收 1–6 验证手段、允许路径）。契约变更批准卡已提交主人，**待批准**。本会话不写实现代码、不开 development 节点。
- 2026-10-01 **approval**：主人批准契约变更（PDU 99–106 追加 + `group_file` 新表 + `schema_version=2026100103`），并指示开 development 节点。冻结面命中（两端 `protocol.h`）按批准清单执行，判读口径见 design §9。
- 2026-10-01 **development**（提交 `0b83aab`）：实现协议 99–106（只追加，两端常量块逐字节一致）、`group_file` 迁移（`migrate_ext_group_003.sql` + 版本 `2026100103`）、`FileObjectStore::attachGroupFile/releaseGroupFile`（refcount = user_file(active) ∪ group_file(active)）、`OperateDB::handleGroupFileOperation`（P0–P7 即时判定）、`MsgHandler::groupFileRequest` + 分派登记 + `authRejectPdu` 映射、客户端群文件窗口（上传/下载/删除/列表，32 确认按上传上下文路由）。群上传复用 31/32、群下载复用 42/43，源路径仅由服务端 `object_id→storage_path` 解析。pipeline：build GREEN、test GREEN-WARN(0 注册用例，如实)、audit 仅报预期冻结面命中。验收：`groupfile_e2e.py` 45/45；既有回归 entry_ops 43/43、dedup 13/13、cross_tree 37/37、sec001 48/48、fastupload 46/46；群生命周期 15/15。**未 push**；未跨分支合并。

## 下一节点
independent test（新会话、异代理）：在 `0b83aab` 的干净 detached worktree 复跑 pipeline 与 `groupfile_e2e.py`，证据落 `project-docs/evidence/REQ-EXT-GROUP-003/independent-test/`。之后 repair（如需）→ independent audit（新会话、异代理、只读）→ integration 三事实行 → merge no-ff。

## independent test 节点收口（2026-10-01）
- **候选**：`0b83aab`；分离工作树 `build/wt-test-group003`（`git worktree add --detach`，干净，只读——本节点零生产源码改动）；本工作树自跑 `pipeline/build.ps1` 现构，`dist/Server.exe` 于 14:01 生成。
- **结论**：**PASS**（0 条归因候选的缺陷，可交 independent audit）。三门：build GREEN、test GREEN-WARN（0 注册用例，如实）、audit RED = **预期冻结面命中**（两端 `protocol.h`，属已批准契约变更；`impact-check: NOT_APPLICABLE`）。
- **验收复跑**：`groupfile_e2e.py` **45/45**；既有回归 dedup **13/13**、entry_ops **43/43**、cross_tree **37/37**、sec001 **48/48**、fastupload **46/46**；群生命周期 `group_e2e_regress.py` **15/15**。均与开发节点声称一致。
- **独立核验（本轮新增）**：`git diff master...0b83aab -- Client/protocol.h Server/protocol.h` 无删除行（append-only）；新增枚举仅 99–106，既有 97/98 原样保留未重编号。
- **环境**：DB `mydb` 已含 `schema_version=2026100103`，表 `group_file` 在位；统一 e2e 运行形态（cwd=dist / `dist/Server.exe` / `filesPath=./filesys` 相对 / CRLF 配置）；解释器 `py -3.13`。群生命周期 harness 不自管 Server，由外部预置 Qt PATH 启动后运行。
- **未执行**：GUI 冒烟（无法自动化）；冻结面深度复核与 CodeGraph 影响闭包（属 independent audit 节点，不采信本轮结论）。
- **下一节点**：independent audit（新会话、不同代理、detached worktree 只读 `0b83aab`）。

## independent audit 节点收口（2026-10-01）
- **审计对象**：`0b83aab`；detached worktree 只读，**零提交、零生产源码改动**，报告落盘后删除。
- **结论**：**PASS** —— 0 条阻塞缺陷；**6 条非阻塞观察**（归档待后续，不进 repair，不阻断 integration）：
  ① 权限门 P1–P5 非事务/非行锁（退出与被移除的竞态窗口不可由客户端操纵，顺序撤销验收不受影响）；
  ② 群下载失败静默（客户端先判 `<36B`，失败响应仅 24B，无提示）；③ 102 展示名语义偏差（未命中回请求名，影响为零）；
  ④ 全局内容去重可"无实体入群"（既有内容寻址模型属性，非本候选引入）；⑤ 名称钳制按 70 字节而非 70 字符（保守无碍）；
  ⑥ 客户端上传单例假设（既有模式）。既有基线 `FINDING-PRE1` 不在本回归面。
- **独立性**：审计者 claude-code（新会话、只读），与 development owner 同名——**跨代理未满足**，报告如实记录；
  经主人裁决接受（`audit: accepted by user, 2026-10-01`）。
- **独立复证**：两端 `protocol.h` 99–106 仅追加、两端常量块逐字节一致；候选之上零代码提交（`0b83aab..9129900` 仅 `project-docs/`）。

## integration / merge 节点收口（2026-10-01）· LARGE 收尾
- **三事实行**（原始核验 `project-docs/evidence/REQ-EXT-GROUP-003/integration/merge-facts.txt`）：
  ① 独立审计报告**已存在且被接受**（主人 2026-10-01 接受报告与同代理独立性）；
  ② **冻结面零越批准改动**：`git diff 0b83aab..495e552 --stat -- Client/protocol.h Server/protocol.h` 为空；
  ③ **合并结果 pipeline GREEN**：build GREEN、test GREEN-WARN(honest-0)、audit PASS（`495e552`）。
- **合并**：`codex/req-ext-group-003-group-files@9129900` → `master`，**no-ff**，合并提交 `495e552`（父 `285b4b0`+`9129900`），文件集无交集、零冲突。
- **冻结命中层级**：分支层 `independent-test/pipeline-audit.log` = `audit RED: frozen:Client/protocol.h, frozen:Server/protocol.h`（设计信号，已批准）；合并后 trunk `audit PASS`（冻结检查按分支范围，合并增量属历史）。
- **本次发布**：群文件目录与访问控制——群文件协议 99–106（只追加）、`group_file` 表 + `schema_version=2026100103`、`FileObjectStore::attachGroupFile/releaseGroupFile`（refcount 联合语义）、`OperateDB::handleGroupFileOperation` P0–P7 每请求即时判定、客户端群文件窗口。
- **审计发现**：0 阻塞；6 条非阻塞观察（见上，归档待后续）。
- **回归面**：`groupfile_e2e.py` 45/45；既有 13/43/37/48/46；群生命周期 15/15。**新增回归测试**：无独立注册用例入库（仓库 0 注册自动化测试，回归由 e2e socket harness 承担，与既有口径一致）；GUI 冒烟维持 NEEDS-HUMAN。
- **收尾**：审计 + 集成证据归档 `project-docs/evidence/REQ-EXT-GROUP-003/`；worktree 释放、分支保留待清。
