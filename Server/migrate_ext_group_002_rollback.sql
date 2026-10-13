-- REQ-EXT-GROUP-002 回滚：只删除本需求新增表/列与版本行，不动 GROUP-001 数据。
DROP TABLE IF EXISTS group_message;

SET @col_exists := (SELECT COUNT(*) FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'group_info' AND COLUMN_NAME = 'last_seq');
SET @ddl := IF(@col_exists = 1,
  'ALTER TABLE group_info DROP COLUMN last_seq',
  'SELECT 1');
PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

DELETE FROM schema_version WHERE version = 2026100101;
