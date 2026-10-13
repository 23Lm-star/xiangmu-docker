-- migrate_add_password_salt.sql
-- REQ-AUTH-002：密码哈希加盐迁移
-- salt = '' 表示迁移前的老数据：按旧逻辑（客户端裸 MD5）比对，老用户登录成功后自动补盐

ALTER TABLE user_info ADD COLUMN salt VARCHAR(32) NOT NULL DEFAULT '';

-- 校验：老数据 salt 应为空串
-- SELECT name, pwd, salt FROM user_info;
