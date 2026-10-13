-- REQ-EXT-GROUP-001：群生命周期与成员关系迁移（目标版本 2026093001）。
-- 只创建本需求新增表；服务端不会运行时静默建表，执行失败必须人工处理。

CREATE TABLE IF NOT EXISTS group_info (
  group_id      BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  group_name    VARCHAR(128) NOT NULL,
  creator_name  VARCHAR(32) NOT NULL,
  status        ENUM('active','dissolved') NOT NULL DEFAULT 'active',
  version       BIGINT UNSIGNED NOT NULL DEFAULT 1,
  created_at    TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  updated_at    TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  KEY idx_group_status (status),
  KEY idx_group_creator (creator_name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS group_member (
  group_id       BIGINT UNSIGNED NOT NULL,
  user_name      VARCHAR(32) NOT NULL,
  role           ENUM('owner','admin','member') NOT NULL DEFAULT 'member',
  status         ENUM('active','left','removed') NOT NULL DEFAULT 'active',
  last_read_seq  BIGINT UNSIGNED NOT NULL DEFAULT 0,
  joined_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  updated_at     TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (group_id, user_name),
  KEY idx_group_member_user_group (user_name, group_id),
  KEY idx_group_member_status (group_id, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS group_invitation (
  invitation_id  BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  group_id       BIGINT UNSIGNED NOT NULL,
  inviter_name   VARCHAR(32) NOT NULL,
  target_name    VARCHAR(32) NOT NULL,
  status         ENUM('pending','accepted','rejected','expired') NOT NULL DEFAULT 'pending',
  expires_at     DATETIME NOT NULL,
  request_key    VARCHAR(64) NOT NULL,
  created_at     TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
  handled_at     DATETIME NULL,
  UNIQUE KEY uk_group_invitation_request (group_id, request_key),
  KEY idx_group_invitation_target (target_name, status),
  KEY idx_group_invitation_group_target (group_id, target_name, status)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS schema_version (
  version INT NOT NULL PRIMARY KEY,
  applied_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

INSERT IGNORE INTO schema_version (version) VALUES (2026093001);
