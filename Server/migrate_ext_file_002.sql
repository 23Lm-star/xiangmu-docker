-- REQ-EXT-FILE-002 顺序上传秒传与去重 迁移脚本（目标版本 2026100102）
-- 执行：mysql -u<user> -p <db> < migrate_ext_file_002.sql
-- 附加式（additive）：只新增 upload_session 的两列与一个唯一键，不新增表、不删数据、
-- 不改既有列语义；幂等可重放（MySQL 5.7 无 ADD COLUMN IF NOT EXISTS，用 information_schema 判断）。
-- 回滚见 migrate_ext_file_002_rollback.sql（DROP 键与两列；表内容零改动）。
-- 键长：uk_req = (32 + 64) * 3 = 288B < 767B（本机 Antelope + COMPACT 上限）✓
-- 历史行 client_request_id 为 NULL，MySQL 唯一键允许多个 NULL，存量行不冲突。

-- upload_session.client_request_id：请求级幂等键（秒传预检查短路依据）
SET @col_exists := (SELECT COUNT(*) FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'upload_session'
    AND COLUMN_NAME = 'client_request_id');
SET @ddl := IF(@col_exists = 0,
  'ALTER TABLE upload_session ADD COLUMN client_request_id VARCHAR(64) NULL',
  'SELECT 1');
PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- upload_session.file_id：秒传命中后指向的逻辑条目（user_file.file_id）
SET @col_exists := (SELECT COUNT(*) FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'upload_session'
    AND COLUMN_NAME = 'file_id');
SET @ddl := IF(@col_exists = 0,
  'ALTER TABLE upload_session ADD COLUMN file_id BIGINT NULL',
  'SELECT 1');
PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

-- (owner_name, client_request_id) 唯一：同一用户同一请求键只允许一条会话行
SET @idx_exists := (SELECT COUNT(*) FROM information_schema.STATISTICS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'upload_session'
    AND INDEX_NAME = 'uk_req');
SET @ddl := IF(@idx_exists = 0,
  'ALTER TABLE upload_session ADD UNIQUE KEY uk_req (owner_name, client_request_id)',
  'SELECT 1');
PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

INSERT IGNORE INTO schema_version (version) VALUES (2026100102);
