-- REQ-EXT-SESSION-001 唯一登录：user_info 加登录版本列 + 登录记录表
-- 人工执行（FILE-001 未合并态，独立迁移文件；其合并后按 C1 规则注册进 schema 版本闸门）
ALTER TABLE user_info ADD COLUMN login_version BIGINT NOT NULL DEFAULT 0;

CREATE TABLE IF NOT EXISTS login_record (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(32) NOT NULL,
    client_id VARCHAR(64) NULL DEFAULT NULL,
    login_version BIGINT NOT NULL,
    login_time DATETIME NOT NULL,
    KEY idx_user_ver (username, login_version)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
