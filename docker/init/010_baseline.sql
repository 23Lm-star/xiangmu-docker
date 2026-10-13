-- 数据库初始化脚本 (回归原始双名字段结构)
SET FOREIGN_KEY_CHECKS = 0;
DROP TABLE IF EXISTS friend;
DROP TABLE IF EXISTS login_record;
DROP TABLE IF EXISTS user_info;
SET FOREIGN_KEY_CHECKS = 1;

CREATE TABLE user_info (
    id INT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(32) UNIQUE NOT NULL,
    pwd VARCHAR(32) NOT NULL,
    online INT DEFAULT 0,
    salt VARCHAR(32) NOT NULL DEFAULT '',
    login_version BIGINT NOT NULL DEFAULT 0
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- REQ-EXT-SESSION-001 登录档案（每次成功登录一行；种子不插登录行）
CREATE TABLE login_record (
    id INT AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(32) NOT NULL,
    client_id VARCHAR(64) NULL DEFAULT NULL,
    login_version BIGINT NOT NULL,
    login_time DATETIME NOT NULL,
    KEY idx_user_ver (username, login_version)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE friend (
    id INT AUTO_INCREMENT PRIMARY KEY,
    name VARCHAR(32) NOT NULL,
    friendName VARCHAR(32) NOT NULL,
    UNIQUE KEY (name, friendName)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 插入测试用户（注意：pwd 字段存储的是服务端加盐后的 32 字符 hex 串，不是明文！）
-- salt='' 表示迁移前老数据，按裸 MD5 比对；下方种子数据 salt 为空，登录成功后会自动补盐升级
-- 123456 的 MD5 哈希 = e10adc3949ba59abbe56e057f20f883e
INSERT INTO user_info (name, pwd, online) VALUES ('tester', 'e10adc3949ba59abbe56e057f20f883e', 1);
INSERT INTO user_info (name, pwd, online) VALUES ('friend1', 'e10adc3949ba59abbe56e057f20f883e', 1);
INSERT INTO user_info (name, pwd, online) VALUES ('friend2', 'e10adc3949ba59abbe56e057f20f883e', 0);

-- 建立好友关系 (name-friendName)
INSERT INTO friend (name, friendName) VALUES ('tester', 'friend1');
