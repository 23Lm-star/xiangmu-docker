# REQ-EXT-FILE-002 顺序上传秒传与去重 — 设计

> 基线 `master@1123a98`；分支 `codex/req-ext-file-002-fast-upload`。
> 设计原则：**复用 FILE-001 已落地的资产模型与事务边界，不重写**；本需求新增"预检查入口 + 命中分支 + 复算对照"三段接线。

## 1. 协议（D1）

### 1.1 枚举（两端逐字节一致，只追加）
```
ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_REQUEST = 97,   // 秒传预检查请求
ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_RESPOND = 98,   // 秒传预检查响应
```
现 max=96（`Server/protocol.h:124`），**不重编号任何既有枚举**。两端 `protocol.h` 同步追加同一对常量与下列偏移常量；一致性由独立测试脚本逐字节比对常量块。

### 1.2 请求布局（可变长度一律进 `caMsg`，`caData[64]` 只放定长与长度字段）
```
caData:
  +0   u32   version        = 1              // 版本化，服务端不等则拒
  +4   u32   magic          = FASTCHECK_MAGIC
  +8   qint64 fileSize
  +16  u32   shaHexLen       (= 64)
  +20  u32   nameLen
  +24  u32   dirLen
  +28  u32   reqIdLen
  +32  ...   保留 32B（全零，对齐既有 UPLOAD_FILE_SIZE_OFFSET 惯例）
caMsg（按上列长度顺序拼接）:
  shaHex(64) | displayName | dirPath | clientRequestId
```
- 服务端总长上限 `kFastCheckMaxMsgLen = 1024`（64 + 210 + 450 + 96 + 余量）；超限直接拒绝，不解析。
- **不把文件名/路径塞进固定控制区**——这是 REQ §3 的明文约束，也是既有 `UPLOADFILE_INIT` 把名字固定 32B 塞 `caData` 的已知局限；新 PDU 不再沿袭。

### 1.3 响应布局
```
caData:
  +0   bool  ret            // 请求整体被接受
  +1   bool  hit            // 秒传命中
  +2   u32   version
  +8   qint64 fileId        // hit 时的逻辑条目（仅对本人可见）
  +16  qint64 sessionId
  +24  qint64 nextOffset    // miss 时的续传起点
  +32  u32   nameLen
caMsg: 服务端最终展示名（hit 时非空）；miss 时为空
```

### 1.4 分派接线（易漏点）
新请求类型必须同时登记到 `MyTcpSocket::authRejectPdu`（`Server/mytcpsocket.cpp:618` 一带的 request→respond 映射表），否则未认证连接的 FASTCHECK 会落到兜底分支、响应类型错配。

## 2. 服务端流程（D2）

`MsgHandler::uploadfilefastcheck()`（新增），按序：

1. **解析与校验**：version/magic 匹配；`shaHexLen==64` 且全为 `[0-9a-f]`；各长度字段与总长在限内。任一不合 → `ret=false`，日志归 `lcFs`（**不落任何内容与路径明文**）。
2. **授权与目录归一**：`parent = FileObjectStore::normalizeParent(m_actorName, dirPath)`，并以 `logicPathInActorRoot(dirPath)` 做 actor 根约束。目标目录**尚未创建**，故用逻辑校验——物理存在性校验（`isPathInSandbox`）仅保留给仍走磁盘的操作（FILE-001 T3 已拆出该分层，本需求沿用）。
3. **请求级幂等短路**：`SELECT session_id,status,confirmed_offset,file_id FROM upload_session WHERE owner_name=? AND client_request_id=? FOR UPDATE`
   - `status='done'` → 回原 `file_id` + 服务端定名（`hit=true`），零写入。
   - `status='uploading'` → 回 `sessionId` + `confirmed_offset`（断点续传），零写入。
4. **摘要预检查（去重命中）**：`SELECT object_id,size_bytes,status FROM file_object WHERE sha256=? AND size_bytes=? FOR UPDATE`
   - 命中 → 调 `FileObjectStore::attachExistingObject(owner, parent, displayName, objectId, size)`：在**同一事务**内
     (a) 同名安置——复刻 `commitUpload` 步骤 2/3 语义：同对象同名=幂等命中零写入；同名不同对象（含目录占名）才按 `base(N).ext` 递推；`deleted` 同名行复活；
     (b) 建/复活 `user_file` 条目；
     (c) 引用计数 +1（**仅真正新增引用时**）；
     并落 `upload_session` 行 `status='done'`、`file_id`、`confirmed_offset=size`、`staging_path=''`。
     **不读客户端物理路径、不复制物理文件、不传输正文。**
   - 命中 `status='pending_reclaim'`（引用归零待回收但物理尚在）→ 允许复用：同事务内置回 `ready` 并 +1 引用（D3 语义：待回收 ≠ 已删除，FILE-001 决策一致）。
   - 未命中 → `ResumableUploadSession::beginStaged` 取确定性 staging 路径 + `createUploadSession(..., client_sha256=sha, client_request_id=reqId)`，回 `hit=false` + `sessionId` + `nextOffset=0`。**不建对象、不建条目。**
5. **未命中后的顺序上传**：客户端继续走既有 `UPLOADFILE_DATA_REQUEST` 分块（4096B）+ 连续 offset ACK。**该 PDU 与状态机零改动**（REQ §3 末条）。
6. **收齐与复算对照**：既有 `finalizeUploadOnComplete` → `commitUpload` **增参 `expectedSha`**。服务端**流式**复算 staging 的 SHA-256 与大小：
   - 大小 ≠ 登记大小 **或** 摘要 ≠ `upload_session.client_sha256` → 删 staging、会话 `failed`、**不建对象、不建条目、不转正**（验收 3）。
   - 一致 → 走既有转正事务（对象 find-or-create + 条目 + 引用计数）。
   - 不变式：**客户端摘要只用于命中判定，永不作为转正依据**（`migrate_ext_file_001.sql:47` 表注释已声明此语义，本设计把它变成执行事实）。
7. **并发（验收 4）**：两路相同文件并发 → 预检查双方都未命中 → 各自上传 → `commitUpload` 内 `SELECT ... FROM file_object WHERE sha256=? FOR UPDATE` 串行化，`uk_sha256` 唯一键兜底 → 先到者建对象并原子 `rename` 转正，后到者命中既有对象、**丢弃自己的 staging**、只建自己的条目（引用 +1）。**物理对象唯一性由 DB 唯一键保证，不依赖时序或应用层约定。** 失败方（取消/断连）以同 `client_request_id` 重试 → 命中步骤 3 或步骤 4 的幂等分支。
8. **过期会话清理**：只删 `filesys/tmp/` 下的 staging 与过期 `upload_session` 行；已完成对象与 `user_file` 条目**不在清理范围**（验收 5）。

### 2.1 新增的存储层方法（补强，不重写）
- `FileObjectStore::attachExistingObject(owner, parentPath, wantedName, objectId, size)` — `commitUpload` 去除"哈希+rename"两段后的同名安置+条目+计数事务；供命中分支复用，避免第二份实现。
- `FileObjectStore::commitUpload(..., const QString& expectedSha)` — 增参，`expectedSha` 为空时行为与现状**逐字节等价**（旧路径回归零破坏）。
- L1/L2 判别收紧：`isLogicKeyForm` 增两条否证——含 `':'`，或首段与 fs 根 basename 同名（`/filesys/<owner>/…/` 形态）→ 判为非 canonical，走转换而非静默保留。

## 3. 数据模型变更（D3，additive）

`Server/migrate_ext_file_002.sql`（幂等可重放；不新增表、不删数据）：
```sql
-- MySQL 5.7 无 ADD COLUMN IF NOT EXISTS → information_schema 存在性判断 + PREPARE/EXECUTE
ALTER TABLE upload_session ADD COLUMN client_request_id VARCHAR(64) NULL;
ALTER TABLE upload_session ADD COLUMN file_id BIGINT NULL;
ALTER TABLE upload_session ADD UNIQUE KEY uk_req (owner_name, client_request_id);
INSERT IGNORE INTO schema_version (version) VALUES (2026100102);
```
- 键长校验：`(32+64)*3 = 288B` < 767B（Antelope+COMPACT 上限）✓；既有行 `client_request_id` 为 NULL，MySQL 唯一键允许多 NULL，**历史行不冲突**。
- `OperateDB` 增 `kFastUploadSchemaVersion` 并入 `qMax(...)` 的 `required` 计算（`Server/operatedb.cpp:96`）；低版本启动 fail-fast，错误文案补上 `migrate_ext_file_002.sql`。
- **2026-10-01 development 更正**：本文件原写的版本号 `2026100101` 与 REQ-EXT-GROUP-002 的
  `kGroupMessageSchemaVersion = 2026100101` 重名，会让"只跑过 GROUP-002 迁移"的库通过启动检查
  （缺列仍启动）。实际实现取 **`2026100102`**（独立版本号），其余不变；理由与验证见
  `project-docs/evidence/REQ-EXT-FILE-002/development-report.md` §4.1。
- 回滚：`Server/migrate_ext_file_002_rollback.sql`（DROP KEY `uk_req` + DROP 两列；表内容零改动）。

## 4. 客户端改动（D4）

| 位置 | 改动 |
|---|---|
| `Client/protocol.h` | 追加两枚举 + 偏移/magic 常量（与 Server 逐字节一致） |
| `Client/file.cpp:391 on_upload_PB_clicked` | 选文件后**流式**算 SHA-256（`QCryptographicHash`，1MB 分块读 → 内存 O(1)，验收 6），生成 32hex `clientRequestId`，发 FASTCHECK 请求 |
| `Client/msghandler.cpp` | 新增 `uploadfilefastcheck()` 处理器 |
| `Client/client.cpp:165 handlMsg` | 分派新 respond |
| 行为 | `hit` → 提示秒传完成 + `flushfile()` 刷新列表（**零正文传输**）；`miss` → 复用既有 `Uploader`，从 `nextOffset` 起走原顺序分块路径 |

- 旧 `ENUM_MSG_TYPE_UPLOADFILE_INIT_REQUEST` 路径**保留不动**：服务端继续支持，客户端不再发送。既有 e2e 脚本、老客户端、断点续传回归面零破坏。
- 客户端 UI 无新增控件；文件名/路径仍走既有 `isValidFileName`。

## 5. 兼容、回归与审计判读

- **回归**：FILE-001 四套件（`dedup_e2e.py` 13、`entry_ops_e2e.py` 43、`cross_tree_e2e.py` 37、`sec001_regression.py` 48）走旧 INIT 路径，必须**原样全绿**。
- **新增**：`fastupload_e2e.py`（验收 1–5 端到端；Python 造 PDU + DB 状态断言 + 服务端日志断言，仿 SEC-001 模式）。
- **冻结面判读（重要）**：本分支 `audit.ps1` 的冻结面检查**必然报警**——因为改动正是 `protocol.h`。判读口径不是"零命中"，而是**与批准清单逐项比对**：① 只追加不重编号（旧编号值不变）② 两端 `protocol.h` 常量块一致 ③ 无既有 PDU 语义变更。审计报告须按此口径给出结论。

## 6. 验收映射（REQ §5）

| 验收 | 证据形态 |
|---|---|
| 1 命中不传正文、条目立即可读 | 命中后分块 PDU 计数 = 0；`flushfile` 立刻可见该名 |
| 2 同内容不同名只存一物理对象；同名不同内容稳定定名 | `file_object` 行数 = 1 且 `refcount = 2`；`(1)` 递推稳定 |
| 3 伪造摘要/大小/路径/上传者 | 伪造 sha 不命中既有对象（不泄露他人对象存在性）；复算不符 → 无条目无对象、会话 `failed`；跨用户目录被拒 |
| 4 并发相同文件单对象 | 两连接并发同文件 → 同 sha 单行、`refcount = 2`、两条目 |
| 5 中断续传；过期清理不伤已完成对象 | 重发同 `client_request_id` → `nextOffset` 续传；清理只覆盖 `tmp/` 与过期会话行 |
| 6 既有面回归 + 内存 O(1) | 四套件全绿；SHA 分块流式计算（1MB 窗口），无整文件读入 |

## 7. 停止条件（REQ §6）
协议兼容性、服务端复算、对象唯一性**任一无法被独立测试证明** → 停止，不进 GROUP-003。不实现：动态分片、乱序并行分片、多文件任务中心、对象存储/独立文件服务。

## 8. 允许路径
`Client/protocol.h`、`Client/msghandler.{h,cpp}`、`Client/client.{h,cpp}`、`Client/file.{h,cpp}`、`Client/uploader.{h,cpp}`、`Server/protocol.h`、`Server/msghandler.{h,cpp}`、`Server/mytcpsocket.{h,cpp}`、`Server/fileobjectstore.{h,cpp}`、`Server/operatedb.{h,cpp}`、`Server/resumableuploadsession.{h,cpp}`、`Server/main.cpp`、`Server/Server.pro`、`Server/migrate_ext_file_002.sql`、`Server/migrate_ext_file_002_rollback.sql`、`project-docs/tasks|evidence/REQ-EXT-FILE-002/`。

**禁止**：改动既有 PDU 编号或既有分块协议语义、群文件（GROUP-003）、乱序/动态分片、多文件任务中心、对象存储或独立文件服务、物理删除用户既有文件、新增外部依赖。

## 9. 运行环境事实（沿用 FILE-001 实测，独立测试节点照此准备）
- 本机 MySQL 5.7：Antelope+COMPACT，索引键 767B 上限，`character_set_server=utf8`。
- Qt 工具链：`C:\Qt\5.14.2\mingw73_64` + `C:\Qt\Tools\mingw730_64`；Anaconda 同名 DLL 会劫持，启动 Server.exe 前须 PATH 前置两个 bin 目录。
- Server.exe 运行目录必须是 `Server/`（`filesPath=./filesys` 为相对路径）；GUI 子系统，stdout 不可重定向，断言走端口/日志文件/DB。
- 新工作树首次构建前须拷贝未入库的 `Client/connect.config` 与 `Server/connect.config`，以及 `libmysql.dll` + `sqldrivers/qsqlmysql.dll` 到 exe 旁。
- Python 用 `py -3.13`（默认 3.6.5 无 `capture_output`）；harness 环境变量须以真实 env 传递（bash 内 `set` 无效会静默回落）。
