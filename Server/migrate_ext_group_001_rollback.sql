-- REQ-EXT-GROUP-001 回滚：只删除本需求新增表和版本行。
DROP TABLE IF EXISTS group_invitation;
DROP TABLE IF EXISTS group_member;
DROP TABLE IF EXISTS group_info;
DELETE FROM schema_version WHERE version = 2026093001;
