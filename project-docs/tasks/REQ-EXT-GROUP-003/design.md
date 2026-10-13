# REQ-EXT-GROUP-003 群文件目录与访问控制 — 设计

> 基线 `master@60a5fc9`；分支 `codex/req-ext-group-003-group-files`。
> 设计原则：**复用已落地的群权限事实源（GROUP-001/002）与文件资产层（FILE-001/002），不重写**；本需求新增"群文件协议段 + `group_file` 关联表 + 群作用域上传/下载/删除三段接线"。

## 1. 协议（D1：追加 PDU 99–106，只追加不重编号）

### 1.1 枚举（两端 `protocol.h` 逐字节一致，只追加）
```
// ===== REQ-EXT-GROUP-003：群文件目录与访问控制（仅追加，旧编号 1-98 不变） =====
ENUM_MSG_TYPE_GROUP_FILE_LIST_REQUEST          = 99,   // 群文件列表请求
ENUM_MSG_TYPE_GROUP_FILE_LIST_RESPOND          = 100,  // 群文件列表响应
ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_REQUEST   = 101,  // 群文件上传初始化（含秒传预检查）
ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_RESPOND   = 102,  // 命中/续传起点
ENUM_MSG_TYPE_GROUP_FILE_DELETE_REQUEST        = 103,  // 删除群文件引用请求
ENUM_MSG_TYPE_GROUP_FILE_DELETE_RESPOND        = 104,  // 删除群文件引用响应
ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_REQUEST      = 105,  // 群文件下载请求
ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_RESPOND      = 106,  // 群文件下载元数据响应（正文走既有 42/43）
```
现 max=98（`Server/protocol.h:128`），**不重编号任何既有枚举**。99–106 全部走 **GROUP-001/002 的 20B 群信封 + `code` + 业务字段**编码（`groupAppendU16/U32/U64/String` / `groupRead*`，小端手写），不新增编码原语。

### 1.2 信封与字段排布（偏移相对**完整 payload**，即 `caMsg` 起始）
信封（20B，与 60–96 完全相同）：`version`(u16)@0 | `envelope_length`(u16)@2 | `request_id`(u64)@4 | `group_id`(u64)@12。
响应在信封后附 `code`(u32，0 成功 / 负数为业务错误，沿用 §8.5 码段)。字符串 = `u32 length + UTF-8 bytes`。

**99 LIST 请求**
| 偏移 | 字段 | 类型 | 约束 |
|---|---|---|---|
| 0 | envelope | 20B | version=1、envelope_length=20 |
| 20 | offset | u32 | 分页起点（0 起） |
| 24 | limit | u16 | 1–200，越界拒绝 |

**100 LIST 响应**
| 偏移 | 字段 | 类型 | 说明 |
|---|---|---|---|
| 0 | envelope | 20B | 回填 request_id/group_id |
| 20 | code | u32 | 0 成功 |
| 24 | count | u32 | 本页记录数 |
| 28 | total | u32 | 该群 active 群文件总数 |
| 32 | records[] | N×记录 | 每记录：`group_file_id`(u64) + `size_bytes`(u64) + `created_at_ms`(u64) + `uploader`(string) + `display_name`(string) |
> 记录含名称、大小、上传者、时间四元数据（验收 1）；不含任何物理路径。

**101 UPLOAD_INIT 请求**
| 偏移 | 字段 | 类型 | 约束 |
|---|---|---|---|
| 0 | envelope | 20B | 群作用域（group_id 即目标群） |
| 20 | file_size | u64 | ≥0 |
| 28 | sha256 | string(u32 len) | 严格 `[0-9a-f]{64}`，小写十六进制 |
| … | display_name | string | 非空、≤255B、无 `/` `\` NUL 控制符；服务端 UTF-8 钳到 70 列 |

**102 UPLOAD_INIT 响应**
| 偏移 | 字段 | 类型 | 说明 |
|---|---|---|---|
| 0 | envelope | 20B | |
| 20 | code | u32 | 0 成功 |
| 24 | hit | u8 | 1=命中已有物理对象（**零正文**，群目录立即可见）；0=需分块上传 |
| 25 | reserved | 3B | 全零 |
| 28 | file_id | qint64 | 上传者 `user_file.file_id`（个人根逻辑条目；删除个人条目不级联群） |
| 36 | group_file_id | qint64 | hit 时非 0，直接可见；miss 时为 0 |
| 44 | session_id | qint64 | 复用的 `upload_session.session_id` |
| 52 | next_offset | qint64 | miss 后续传起点（hit 时为 file_size） |
| 60 | name_len | u32 | 服务端最终展示名长度 |
| 64 | display_name | bytes | 服务端最终展示名（含 `(N)` 去重结果） |

**103 DELETE 请求**：envelope 20B | `group_file_id`(u64)@20。
**104 DELETE 响应**：envelope 20B | `code`(u32)@20 | `group_file_id`(u64)@24。

**105 DOWNLOAD 请求**：envelope 20B | `group_file_id`(u64)@20。
**106 DOWNLOAD 响应（元数据）**：envelope 20B | `code`(u32)@20 | `size_bytes`(u64)@24 | `name_len`(u32)@32 | `display_name`(string)。
> 106 之后，正文经**既有** `DOWNLOADFILE_DATA`(42) 分块 + `DOWNLOADFILE_FINISH`(43) 传输，**42/43 语义零改动**；源路径由服务端解析（§4）。

### 1.3 数据面复用与显式不复用
- **复用**：群上传分块 = 既有 `UPLOADFILE_DATA_REQUEST`(31/32)（会话态在连接上，偏移连续性校验不变）；群下载分块 = 既有 42/43。
- **不复用**：FILE-002 秒传预检查 `UPLOADFILE_FASTCHECK`(97/98) —— 其为**个人根**路径语义。群上传的秒传预检查**并入 101/102**（服务端以 actor 身份+群作用域做对象去重），97/98 语义**一字不改**。
- **不新增**：不改 29–32、40–43、97/98 任一字段或语义。

### 1.4 分派接线（易漏点，与 FILE-002 §1.4 同型）
- 99/101/103/105 必须登记进 `MyTcpSocket::handlMsg` 的群请求 case 段（`mytcpsocket.cpp:240-256` 一带），使其**自动落在 SEC-001 actor 门之后**。
- 必须登记进 `authRejectPdu`（`mytcpsocket.cpp:660-694` 一带）request→respond 映射（`groupReject(100/102/104/106)`），否则未认证请求响应类型错配。
- **不得**加入 `isPublicRequestType` 白名单。
- 106 之后的分块 42/43 为既有下载流，无需新 case。

## 2. 服务端流程

### 2.1 入口
`MsgHandler::groupFileRequest(uint requestType)`（新增，与 `groupRequest` 平行）：
先按 §1.2 逐字段验信封与边界（version/envelope_length/长度字段/取值域），`offset != payload.size()` 判越界载荷拒绝（`-2002`，不解析、不落明文）。解析失败与越界一律拒绝并归 `lcNet`/`lcFs` 日志。

### 2.2 权限判定点（**全部服务端、每请求即时判定**，客户端不自报身份、不缓存授权）
统一落在新函数 `OperateDB::handleGroupFileOperation(GroupFileRequest&, const QString &actor)`，**在同一事务**内按序判定：

| # | 判定点 | 规则 | 失败码 |
|---|---|---|---|
| P0 | actor 有效 | `actor` 空 → 拒绝 | -1001 |
| P1 | 群存在且取状态 | `SELECT status FROM group_info WHERE group_id=? FOR UPDATE` | 无行 -2102 |
| P2 | **成员门** | `SELECT role FROM group_member WHERE group_id=? AND user_name=? AND status='active' FOR UPDATE`；`left`/`removed`/无行 → 拒绝（**退出/被移除即时失权**，老连接与新连接一致，因为每请求重查） | -2101 |
| P3 | **解散门（写）** | 读操作（LIST/DOWNLOAD）：P1 群存在即放行（D5：成员可只读历史）。写操作（UPLOAD_INIT/DELETE）：`status!='active'` → 一律拒绝 | -2102 |
| P4 | 群文件归属 | `SELECT ... FROM group_file WHERE group_file_id=? AND group_id=? AND status='active'`；`group_id` 与信封不一致或无行 → 拒绝（不泄露他群存在性） | -2103/-2101 |
| P5 | **删除授权** | DELETE：`role='owner'`（群主可删任意）**或** `uploader_name==actor`（上传者可删自己的）；否则拒绝（普通成员不能删他人文件） | -2103 |
| P6 | 名称合法性 | display_name 非空、≤255B、无 `/` `\` NUL 控制符；服务端钳到 70 列；同名冲突按 `(N)` 递推 | -2103 |
| P7 | 对象存在性 | UPLOAD_INIT 命中 / DOWNLOAD 时 `group_file.object_id → file_object` 必须存在且非物理缺失 | -2105 |

- **事务与并发**：P1/P2 行锁 + `group_file` 唯一键兜底，保证并发上传同名幂等（`(N)` 递推稳定）、并发删引用引用计数不漂移。
- **即时撤销证明点**：P2 每请求同事务 `FOR UPDATE` 重查成员状态——服务端即判，不依赖客户端，也不依赖连接建立时刻的授权快照。

### 2.3 上传（UPLOAD_INIT 101 → 分块 31 → 完成）
1. P0–P3、P6、P7 判定通过。
2. **对象去重预检查**：`SELECT object_id,size_bytes FROM file_object WHERE sha256=? AND size_bytes=? FOR UPDATE`。
   - **命中**：`FileObjectStore::attachGroupFile(actor, groupId, displayName, objectId, size)` 同事务 (a) 建/复活 `user_file` 条目（上传者个人根，逻辑文件标识）(b) 建 `group_file` 行 (c) `file_object.refcount++`（群引用）。回 `hit=1` + `group_file_id`，**不传正文**（验收 2 秒传）。
   - **未命中**：`ResumableUploadSession::beginStaged` 取确定性 staging + `createUploadSession`，回 `hit=0` + `session_id` + `next_offset=0`。
3. **分块**：客户端走既有 `UPLOADFILE_DATA`(31) 连续 offset ACK，**状态机零改动**。
4. **完成**：既有 `finalizeUploadOnComplete` → `FileObjectStore::commitUpload(staging, expectedSha)` **流式复算大小+摘要双对照**（沿用 FILE-002）；相符 → 转正对象 + `attachGroupFile` 建群引用；不符 → 会话 `failed`、删 staging、**不建条目、不建群引用、不转正**。

### 2.4 下载（105 → 106 → 42/43）
1. P0–P3（写门不适用，读门：成员 + 群存在）、P4、P7 判定通过。
2. 服务端由 `group_file.object_id → file_object.storage_path` 解析**物理绝对路径**（服务端生成，`objects/<sha前2位>/<object_id>`），**绝不接受客户端任何路径**。
3. 回 106 元数据，随后经既有 42 分块下发、43 完成。成员退出/被移除后，同一请求（老连接）与新请求（新连接）均在 P2 被拒。

### 2.5 删除（103 → 104）
1. P0–P5 判定通过。
2. `FileObjectStore::releaseGroupFile(groupFileId)` 同事务：`group_file.status='removed'`（软删）+ `file_object.refcount--`；归零且无活跃引用 → `pending_reclaim`（D3：**绝不物理删**）。
3. **只减逻辑引用**：不动 `file_object` 物理文件、不动上传者 `user_file` 条目、不动其他用户/群的引用（验收 4）。

### 2.6 列表（99 → 100）
P0–P2 + 群存在（含解散后成员只读）→ 以 `(group_id, status='active')` 谓词分页返回，元数据来自 `group_file` 快照列（名称/大小/上传者/时间），**不 join 上传者个人根路径**（避免耦合与路径泄露）。

## 3. 数据模型与迁移（D2：新增表，附加式）

`Server/migrate_ext_group_003.sql`（幂等可重放；不新增除下列外任何对象、不删数据）：
```sql
CREATE TABLE IF NOT EXISTS group_file (
  group_file_id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  group_id      BIGINT UNSIGNED NOT NULL,
  file_id       BIGINT          NULL,               -- → user_file.file_id（逻辑文件标识；删个人条目不级联）
  object_id     BIGINT          NOT NULL,           -- → file_object.object_id（物理对象；下载/去重/引用计数）
  uploader_name VARCHAR(32)     NOT NULL,
  display_name  VARCHAR(70)     NOT NULL,           -- 群目录展示名快照（与个人根解耦）
  size_bytes    BIGINT          NOT NULL,           -- 快照
  status        ENUM('active','removed') NOT NULL DEFAULT 'active',
  created_at    TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at    TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  UNIQUE KEY uk_group_file_name (group_id, display_name),
  KEY idx_group_file_group (group_id, status),
  KEY idx_group_file_uploader (group_id, uploader_name),
  CONSTRAINT fk_gf_obj FOREIGN KEY (object_id) REFERENCES file_object(object_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
INSERT IGNORE INTO schema_version (version) VALUES (2026100103);
```
- 键长校验：`uk_group_file_name` = `8 + 70*3 = 218B` < 767B（MySQL 5.7 Antelope+COMPACT 上限）✓；现库 `character_set_server=utf8`，`display_name VARCHAR(70)` 与 `user_file` 同列宽。
- **`schema_version` 取独立版本号 `2026100103`**（现 max=2026100102 @FILE-002；**不得**与 2026100101 GROUP-002 / 2026100102 FILE-002 重名，重名会让"未跑本迁移"的库通过启动检查——FILE-002 已踩此坑）。`OperateDB` 增 `kGroupFileSchemaVersion = 2026100103` 并入 `checkSchemaVersion()` 的 `qMax(...)` `required`（operatedb.cpp:96），低版本启动 fail-fast，错误文案补 `migrate_ext_group_003.sql`。
- 回滚：`Server/migrate_ext_group_003_rollback.sql`（`DROP TABLE group_file`；不回收引用计数变更——回滚仅用于开发，见 §7）。
- **引用计数语义扩展**：`file_object.refcount` 定义为"`user_file`(active) ∪ `group_file`(active) 的活跃逻辑引用数"。`FileObjectStore` 新增/复用 `attachGroupFile`(+1)、`releaseGroupFile`(−1)；`pending_reclaim` 判定须同时观察两表（扩展 `releaseEntry` 的归零判定为"跨 user_file+group_file 无活跃引用"）。

## 4. 路径与越权负向面（验收 5 的设计面）

| 面 | 设计对策 | 负向测试 |
|---|---|---|
| 裸路径拼接 | 群操作**不接受任何客户端路径**：身份是 `group_file_id`，服务端 `object_id→storage_path` 解析 | 请求中注入 `/`、`..`、绝对路径字段 → 无字段可注入/被拒 |
| 路径穿越/符号链接 | 客户端无路径入口；服务端 storage_path 恒为 `objects/<sha2>/<id>`（服务端生成） | 断言落盘路径前缀恒为对象仓，客户端输入不出现在路径中 |
| 超长名称 | 服务端 ≤255B 前置校验 + UTF-8 钳 70 列 | 256B/超长名 → 拒绝或钳制且不越界 |
| 同名冲突 | `uk_group_file_name` + `(N)` 递推 | 同名两次上传 → 两条记录名不同、稳定可重放 |
| 越权跨群 | `group_file_id` 必须与信封 `group_id` 同群（P4） | A 群 id + B 群 group_id → 拒绝 |
| 越权跨用户 | 无按个人文件名/id 下载的群路径 | 猜测 id 下载他人个人文件 → 无入口 |
| 伪造 group_file_id | 无行/已 removed → 拒绝，不泄露存在性 | 随机 id → 拒绝 |
| 非成员/已退出/被移除 | P2 每请求即时判定 | 退出后老+新连接 list/download/delete/upload 全拒（验收 3） |
| 解散群写 | P3 写门 | 解散后 upload/delete 拒（-2102）；list/download 成员仍可（D5） |
| 删他人文件 | P5 授权 | 普通成员删他人文件 → 拒绝；群主/上传者 → 成功 |

## 5. 客户端改动
| 位置 | 改动 |
|---|---|
| `Client/protocol.h` | 追加 99–106 与群文件偏移常量（与 Server 逐字节一致） |
| 新增 `Client/groupfile.{h,cpp}`（或扩展 `Client/group.{h,cpp}`） | 群文件目录面板：列表（名称/大小/上传者/时间）、上传（流式 SHA-256，复用 FILE-002 客户端摘要逻辑）、下载（复用既有下载落盘）、删除（按权限置灰，**仅展示，判定在服务端**） |
| `Client/msghandler.{h,cpp}` | 新增 100/102/104/106 分发 |
| `Client/client.cpp handlMsg` | 分派新响应；42/43 复用既有下载处理 |
| `Client/index.*` | 群模块内增加"群文件"入口页 |
| `Client/Client.pro` | 纳入新增源文件 |

## 6. 验收映射（REQ §4）
| 验收 | 证据形态（Python 造 PDU + DB 状态断言 + 服务端日志断言，仿 SEC-001/FILE-002 模式） |
|---|---|
| 1 列出元数据；非成员失败 | 成员收 100 且含名称/大小/上传者/时间；非成员 list → code≠0 |
| 2 上传出现逻辑记录；同内容复用物理对象；无越权引用 | 上传后 `group_file` 有 active 行；同内容二次 → `file_object` 单行且 refcount 反映两处引用 |
| 3 下载；退出/被移除后老+新连接都失效 | 成员下载成功；踢出后同连接与新连接 list/download/delete 全拒 |
| 4 群主/上传者删除符合规则；删群引用不影响他处物理对象 | owner 删任意成功、uploader 删己成功、member 删他 → 拒；删引用后 `file_object` 行仍在、refcount 减一、他人条目可继续下载 |
| 5 路径负向面 | §4 表逐项（穿越/符号链接/超长/同名冲突/群外不可达） |
| 6 既有面回归 | GROUP-001/002、一对一聊天、个人文件上传/下载、秒传四套件（13/43/37/48）+ fastupload(46) 全绿 |

## 7. 停止条件与开放项（REQ §5）
- **停止**：若"退出/踢出后访问撤销"无法做到**服务端即时判定**（P2 每请求同事务重查是设计硬约束），或群文件出现**裸客户端路径拼接**，或协议无法明确拒绝未知类型 → **停止合并**并回 goal。
- **开放项（development 自决，不超批准面）**：既有 42/43 下载流是否与"个人下载"在同一连接上串行复用（会话态在 socket 上）——development 首务核实；若冲突，改用群作用域下载会话句柄，仍走 42/43 传输、**不新增 PDU**。
- **不做**（§5 排除）：协作编辑、文件锁、版本管理、批量打包下载、群文件消息卡片、复杂 owner/editor/viewer 权限、群目录子目录、新外部依赖、`Server/filesys/` 入库。

## 8. 允许路径
`Client/protocol.h`、新增 `Client/groupfile.{h,cpp}`、`Client/group.{h,cpp}`、`Client/msghandler.{h,cpp}`、`Client/client.{h,cpp}`、`Client/index.{h,cpp,.ui}`、`Client/Client.pro`、`Server/protocol.h`、`Server/msghandler.{h,cpp}`、`Server/mytcpsocket.{h,cpp}`、`Server/operatedb.{h,cpp}`、`Server/fileobjectstore.{h,cpp}`、`Server/server.{h,cpp}`、`Server/main.cpp`、`Server/Server.pro`、`Server/migrate_ext_group_003.sql`、`Server/migrate_ext_group_003_rollback.sql`、`project-docs/tasks|evidence/REQ-EXT-GROUP-003/`、handoff。
**禁止**：改既有 PDU 编号/字段/语义；改成既有表（`group_info`/`group_member`/`group_message`/`user_file`/`file_object`/`upload_session`）列定义；新外部依赖；`Server/filesys/` 入库；群文件之外的功能扩面。

## 9. 运行环境事实（沿用 E2E-RUNFORM + FILE-002 实测）
- 统一 e2e 运行形态见 `project-docs/evidence/E2E-RUNFORM.md`：`cwd=<repo>/dist`、`dist/Server.exe`、`filesPath=./filesys`、脚本自管理 Server 生命周期、夹具自建。
- 本机 MySQL 5.7：Antelope+COMPACT，索引键 767B 上限，`character_set_server=utf8`；库 `mydb`，迁移脚本幂等可重放。
- Python 用 `py -3.13`；harness 环境变量以真实 env 传递（bash 内 `set` 无效）。
- `Server/connect.config` 第 3 行须 `./filesys` 且 **CRLF**（LF 会让服务端启动即 segfault）；新工作树首次构建前须补 `Client/connect.config`/`Server/connect.config` 与 `libmysql.dll`+`sqldrivers/qsqlmysql.dll`。
- 冻结面判读（本分支必然报警）：口径 = 与批准清单逐项比对（① 只追加不重编号 ② 两端常量块逐字节一致 ③ 无既有 PDU 语义变更），非"零命中"。
