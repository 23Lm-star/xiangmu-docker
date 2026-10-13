-- REQ-EXT-GROUP-003 回滚（仅用于开发环境）。
-- 只删本需求新增的 group_file 表；不回收 file_object.refcount 的引用计数变更
-- （回滚后计数偏大只会导致对象暂不回收，不会造成数据丢失；生产回滚须人工核对）。
DROP TABLE IF EXISTS group_file;
DELETE FROM schema_version WHERE version = 2026100103;
