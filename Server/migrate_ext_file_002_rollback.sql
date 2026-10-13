-- REQ-EXT-FILE-002 回滚：只删本需求新增的唯一键与两列，upload_session 表内容与既有列零改动。
-- 回滚后秒传预检查依赖的幂等键与命中登记丢失；已落定的 user_file/file_object 数据不受影响
-- （条目与对象由 REQ-EXT-FILE-001 模型持有，不因本回滚删除）。

SET @idx_exists := (SELECT COUNT(*) FROM information_schema.STATISTICS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'upload_session'
    AND INDEX_NAME = 'uk_req');
SET @ddl := IF(@idx_exists > 0,
  'ALTER TABLE upload_session DROP KEY uk_req',
  'SELECT 1');
PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @col_exists := (SELECT COUNT(*) FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'upload_session'
    AND COLUMN_NAME = 'client_request_id');
SET @ddl := IF(@col_exists > 0,
  'ALTER TABLE upload_session DROP COLUMN client_request_id',
  'SELECT 1');
PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

SET @col_exists := (SELECT COUNT(*) FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'upload_session'
    AND COLUMN_NAME = 'file_id');
SET @ddl := IF(@col_exists > 0,
  'ALTER TABLE upload_session DROP COLUMN file_id',
  'SELECT 1');
PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

DELETE FROM schema_version WHERE version = 2026100102;
