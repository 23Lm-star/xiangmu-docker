-- REQ-EXT-FILE-001 文件资产与内容寻址 迁移脚本（D1：独立 SQL，幂等可重放）
-- 执行：mysql -u<user> -p <db> < migrate_ext_file_001.sql
-- 目标 schema_version = 2026092901；服务端启动检查不达标则拒绝监听。
-- 回滚见 migrate_ext_file_001_rollback.sql（只删新表，旧数据零改动）。
-- 字符集/列宽适配本机 MySQL 5.7（Antelope+COMPACT，索引键上限 767 字节）：
--   uk_loc = (32+150+70)*3 = 756B；old_path PK = 191*3 = 573B。与现有表（utf8）一致。

CREATE TABLE IF NOT EXISTS schema_version (
  version INT NOT NULL PRIMARY KEY,
  applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

INSERT IGNORE INTO schema_version (version) VALUES (2026092901);

CREATE TABLE IF NOT EXISTS file_object (
  object_id    BIGINT AUTO_INCREMENT PRIMARY KEY,
  sha256       CHAR(64) NOT NULL,
  size_bytes   BIGINT NOT NULL,
  storage_path VARCHAR(255) NOT NULL COMMENT '服务端生成: objects/{sha256前2位}/{object_id}',
  refcount     INT NOT NULL DEFAULT 0,
  status       ENUM('ready','pending_reclaim') NOT NULL DEFAULT 'ready',
  created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uk_sha256 (sha256)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS user_file (
  file_id      BIGINT AUTO_INCREMENT PRIMARY KEY,
  owner_name   VARCHAR(32) NOT NULL COMMENT '服务端 actor（PDU 定长 32），客户端字段不参与',
  parent_path  VARCHAR(150) NOT NULL COMMENT '规范化逻辑父路径',
  display_name VARCHAR(70) NOT NULL,
  is_dir       TINYINT NOT NULL DEFAULT 0,
  object_id    BIGINT NULL,
  status       ENUM('active','deleted') NOT NULL DEFAULT 'active',
  created_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  updated_at   TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  UNIQUE KEY uk_loc (owner_name, parent_path, display_name),
  KEY idx_owner (owner_name, parent_path, status),
  CONSTRAINT fk_uf_obj FOREIGN KEY (object_id) REFERENCES file_object(object_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS upload_session (
  session_id       BIGINT AUTO_INCREMENT PRIMARY KEY,
  owner_name       VARCHAR(32) NOT NULL,
  parent_path      VARCHAR(150) NOT NULL,
  display_name     VARCHAR(70) NOT NULL,
  expected_size    BIGINT NOT NULL,
  client_sha256    CHAR(64) NULL COMMENT '仅预检元数据，不作转正依据',
  staging_path     VARCHAR(255) NOT NULL,
  confirmed_offset BIGINT NOT NULL DEFAULT 0,
  status           ENUM('uploading','verifying','done','failed','expired') NOT NULL DEFAULT 'uploading',
  expires_at       DATETIME NOT NULL,
  KEY idx_owner_status (owner_name, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS file_migration_map (
  old_path    VARCHAR(191) NOT NULL PRIMARY KEY,
  file_id     BIGINT NULL,
  object_id   BIGINT NULL,
  status      ENUM('mapped','skipped_name_conflict','failed') NOT NULL,
  migrated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8;
