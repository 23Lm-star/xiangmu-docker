-- REQ-EXT-GROUP-002：可靠群文本消息迁移（目标版本 2026100101）。
-- 幂等、可重复执行；服务端不会运行时静默建表，执行失败必须人工处理。
-- 只新增本需求的列/表，不动 GROUP-001 既有数据。

-- group_info.last_seq：群内序号水位（事务内原子递增）。列已存在时跳过（幂等）。
SET @col_exists := (SELECT COUNT(*) FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'group_info' AND COLUMN_NAME = 'last_seq');
SET @ddl := IF(@col_exists = 0,
  'ALTER TABLE group_info ADD COLUMN last_seq BIGINT UNSIGNED NOT NULL DEFAULT 0',
  'SELECT 1');
PREPARE stmt FROM @ddl;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;

CREATE TABLE IF NOT EXISTS group_message (
  msg_id        BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,  -- 服务端全局唯一消息标识
  group_id      BIGINT UNSIGNED NOT NULL,
  seq           BIGINT UNSIGNED NOT NULL,                    -- 群内序号，(group_id,seq) 唯一
  client_msg_id VARCHAR(64) NULL,                            -- 幂等键；系统消息为 NULL
  sender_name   VARCHAR(32) NOT NULL,                        -- 服务端 actor 推导，非客户端自报
  msg_type      ENUM('text','system') NOT NULL DEFAULT 'text',
  content       TEXT NOT NULL,
  created_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  UNIQUE KEY uk_group_seq (group_id, seq),
  UNIQUE KEY uk_group_client_msg (group_id, client_msg_id),
  KEY idx_group_created (group_id, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT IGNORE INTO schema_version (version) VALUES (2026100101);
