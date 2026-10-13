# REQ-EXT-FILE-001 设计 — 文件资产与内容寻址（分支计划文档）

## 1. 架构一句话

物理字节只存一份（`file_object`，内容寻址），用户看到的每个文件是一条逻辑条目（`user_file`，归属+目录+展示名→对象）；上传经持久化会话（`upload_session`）临时写、服务端复算后原子转正；删除=解引用，引用归零标待回收；旧路径经 `file_migration_map` 双向可审计映射。客户端协议与 UI 零改动（D4）。

## 2. 表结构（T1，utf8/InnoDB——实测本机 MySQL 5.7 Antelope+COMPACT，索引键上限 767B，列宽已适配；见 `migrate_ext_file_001.sql` 头注）

```sql
-- schema_version：单行版本表，启动检查依据（D1）
CREATE TABLE IF NOT EXISTS schema_version (
  version INT NOT NULL PRIMARY KEY, applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP);
INSERT IGNORE INTO schema_version (version) VALUES (2026092901);  -- FILE-001 目标版本

CREATE TABLE IF NOT EXISTS file_object (
  object_id   BIGINT AUTO_INCREMENT PRIMARY KEY,
  sha256      CHAR(64) NOT NULL,
  size_bytes  BIGINT NOT NULL,
  storage_path VARCHAR(255) NOT NULL,          -- 服务端生成: objects/{sha前2}/{object_id}
  refcount    INT NOT NULL DEFAULT 0,
  status      ENUM('ready','pending_reclaim') NOT NULL DEFAULT 'ready',
  created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_sha256 (sha256));              -- 同一内容=同一物理对象（验收1）

CREATE TABLE IF NOT EXISTS user_file (
  file_id      BIGINT AUTO_INCREMENT PRIMARY KEY,
  owner_name   VARCHAR(32) NOT NULL,           -- 服务端 actor，客户端字段不参与（T5）
  parent_path  VARCHAR(150) NOT NULL,          -- 规范逻辑路径，用户根内，'/' 结尾
  display_name VARCHAR(70) NOT NULL,
  is_dir       TINYINT NOT NULL DEFAULT 0,     -- 目录=条目，object_id NULL
  object_id    BIGINT NULL,
  status       ENUM('active','deleted') NOT NULL DEFAULT 'active',
  created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  updated_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  UNIQUE KEY uk_loc (owner_name, parent_path, display_name),   -- 目录与文件同一命名空间（同磁盘现状）
  KEY idx_owner (owner_name, parent_path, status),
  CONSTRAINT fk_uf_obj FOREIGN KEY (object_id) REFERENCES file_object(object_id));

CREATE TABLE IF NOT EXISTS upload_session (
  session_id     BIGINT AUTO_INCREMENT PRIMARY KEY,
  owner_name     VARCHAR(32) NOT NULL,
  parent_path    VARCHAR(150) NOT NULL,
  display_name   VARCHAR(70) NOT NULL,
  expected_size  BIGINT NOT NULL,
  client_sha256  CHAR(64) NULL,                -- 仅预检元数据，不作授权/转正依据
  staging_path   VARCHAR(255) NOT NULL,        -- filesys/tmp/{session_id}
  confirmed_offset BIGINT NOT NULL DEFAULT 0,
  status         ENUM('uploading','verifying','done','failed','expired') NOT NULL DEFAULT 'uploading',
  expires_at     DATETIME NOT NULL,
  KEY idx_owner_status (owner_name, status));

CREATE TABLE IF NOT EXISTS file_migration_map (
  old_path    VARCHAR(191) NOT NULL PRIMARY KEY, -- 旧物理相对路径 filesys/<user>/...
  file_id     BIGINT NULL,
  object_id   BIGINT NULL,
  status      ENUM('mapped','skipped_name_conflict','failed') NOT NULL,
  migrated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP);
```

回滚脚本 `migrate_ext_file_001_rollback.sql`：DROP 5 表（旧数据物理原样，零破坏）。

## 3. 启动版本检查（D1）

`Server::initialize()` 在配置校验后调 `OperateDB::checkSchemaVersion()`：
`SELECT MAX(version) FROM schema_version` → 空表/低于目标版本 → `qCCritical(lcDb)` 输出"请先执行 migrate_ext_file_001.sql"并 **返回 false 拒绝监听**（不静默建表）。表不存在=未迁移，同样拒绝。

## 4. 物理布局与转正（T2）

- staging：`filesys/tmp/{session_id}`（分块只追加写；未完整前对象不可见）。
- 完成：服务端对 staging 全文 `QCryptographicHash::Sha256` 复算 + 大小核对 → `find-or-create file_object`（`uk_sha256` 冲突即命中已有对象，DELETE 多余 staging 文件——同一内容只留一份）。
- 转正：命中新对象时 staging→`objects/{h2}/{object_id}` **同卷 rename**（原子）；命中已有对象时直接丢弃 staging（内容相同，无需搬移）。
- 复算不一致（伪造摘要/大小/中途篡改）：`upload_session.status=failed`、不建条目、不转正，`qCWarning(lcFs)` 带 conn+session_id（验收4）。

## 5. 事务边界（§4 一致性要求）

每处"对象命中+条目创建+引用计数"在一个事务内：
`pool.acquire()` → `BEGIN` → `SELECT object_id FROM file_object WHERE sha256=? FOR UPDATE`（无则 INSERT）→ `INSERT user_file ... ON DUPLICATE KEY UPDATE`（幂等命中）→ `UPDATE file_object SET refcount=refcount+1, status='ready'` → `COMMIT`。删除镜像：`UPDATE user_file SET status='deleted'` → `refcount=refcount-1`，归零 → `status='pending_reclaim'`（D3 不物理删）。任何一步失败回滚，条目/计数不同步的中间态不落盘。

## 6. 逻辑路径与兼容转换（T3/T6，客户端零改动）

- 客户端 PDU 携带的路径串语义不变（`filesys/<user>/...`）；服务端解析为 `(owner, parent_path, display_name)`——**路径前缀不等于 actor 根 → 拒（复用 pathInActorRoot 语法段），物理存在性不再是操作前提**。
- 新增 `logicPathInActorRoot()`：仅做规范化+越权判断（不要求 canonical 存在）；`isPathInSandbox` 原样保留给仍在磁盘执行的读写（staging、对象文件、下载打开）。
- `flushfile`：DB 优先渲染；**某 parent_path 无任何 active 条目且对应物理目录存在** → 回退物理列表（双读过渡，防未迁移目录显示空，风险1）；迁移完成后自然走 DB。
- 幂等与命名：`(owner,parent,name,is_dir)` 唯一键；同对象重复创建=命中返回成功；同名不同摘要 → 展示名追加 `(1)`…` 文件名 ≤ 现有 32 字节 FileInfo.caName 约束截断规则一致。
- move/rename：只改 `parent_path/display_name`（引用不变）；deldir 文件：条目软删+减引用；目录：软删子树条目并逐条减引用。
- sharefile/move 跨用户：源=分享人 `user_file` 行；同意时给接收者**新建条目指向同一 object_id 并 +1 引用**（物理零复制——去重的直接红利），握手登记改按 file_id 断言归属。

## 7. 存量迁移算法（T6，幂等、失败不删旧）

`OperateDB::migrateLegacyFiles()`（启动检查通过后、可选自动执行一次；或独立线程手动触发）：
遍历 `filesys/<user>/` 下未映射文件（`file_migration_map` 无记录）→ SHA-256 → `find-or-create object`（staging 复用同卷 rename 或引用原文件搬移后**保留原路径**至 pending_reclaim 才允许清理——本分支不删，故搬移=复制，磁盘双份是过渡代价，D2/D3 记录在案）→ 建条目 → 写映射。
- 重跑：映射存在 → 跳过（幂等）。
- 同名不同内容冲突：不覆盖，`status=skipped_name_conflict`，条目名追加 `(1)` 规则安置（验收4 后半）。
- 任一文件失败：记 `failed`，继续其余，不中断、不删任何旧文件（验收5）。

## 8. frozen_paths 与停止条件

- 本分支两 `protocol.h` 预期零 diff（开发中若出现必须改协议的情形，立即停止回 goal 按 LARGE 协议变更处理）。
- 停止条件（§6 文档）：迁移映射出现**无法审计的双向关系**（同一 old_path 多映射/条目无映射）→ 停止后续（FILE-002/GROUP-003）并在证据中定格。

## 9. 验收映射（测试计划骨架，T7）

| 验收 | 断言方式 |
|---|---|
| 1 一物多名 | 同内容传两次不同名 → `file_object` 行数不变、`user_file` +2、`refcount=2` |
| 2 幂等/`(1)` | 重复同请求 → 条目数不变；同名不同内容 → 展示名 `(1)` |
| 3 引用删除 | 删一引用另一引用可下载；refcount 归零 → `pending_reclaim`；对象文件仍在盘 |
| 4 伪造拒绝 | 伪造摘要/大小/他人路径 → `failed`+拒绝日志+无条目；客户端报文不变 |
| 5 回归 | SEC-001 48/48 用例全绿 + 上传/下载/分享/列表 e2e；迁移中断注入 → 旧文件零删除 |
| 证据 | 全部绑定候选哈希（build/test.ps1 自动落 hash），审计只认服务端日志+DB 断言 |
