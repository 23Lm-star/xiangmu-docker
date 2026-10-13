-- REQ-EXT-FILE-001 回滚：只删本需求新增表，存量业务表与 filesys 旧文件零改动。
-- 注意：objects/ 下已转正对象文件不随表删除（D3 本分支不物理删）；如需彻底清理，人工确认后手动移除。
DROP TABLE IF EXISTS file_migration_map;
DROP TABLE IF EXISTS upload_session;
DROP TABLE IF EXISTS user_file;
DROP TABLE IF EXISTS file_object;
DELETE FROM schema_version WHERE version = 2026092901;
