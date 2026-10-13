-- REQ-EXT-SESSION-001 迁移回滚（先回滚本任务，再按 FILE-001 回滚链顺序处理其余）
DROP TABLE IF EXISTS login_record;
ALTER TABLE user_info DROP COLUMN login_version;
