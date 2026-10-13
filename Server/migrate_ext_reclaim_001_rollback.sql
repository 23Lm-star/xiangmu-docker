-- REQ-EXT-RECLAIM-001 回滚（仅用于开发环境）。
-- 顺序有硬要求：必须先把 reclaiming 行落回 pending_reclaim，再收窄枚举，
-- 否则收窄会因为表里还有 'reclaiming' 值而失败。
-- 说明：本回滚只回收本需求新增的列/枚举值/索引。
--   * 已被调度器物理删除的对象文件**不可恢复**（这是删除功能的固有代价，
--     回滚不能把它变回来）；仍在的待回收对象只是恢复"待回收"状态。
--   * group_file.object_id 放宽为可空后已被摘成 NULL 的 removed 行不会重新填回，
--     也不影响任何读路径（removed 行不参与下载）。
--   * file_object.refcount 一律不动：回滚后计数只可能偏大，效果是对象暂不回收，
--     不会造成数据丢失；生产回滚须人工核对。

UPDATE file_object SET status='pending_reclaim' WHERE status='reclaiming';

ALTER TABLE file_object
  MODIFY status ENUM('ready','pending_reclaim') NOT NULL DEFAULT 'ready';

SET @idx_exists := (SELECT COUNT(*) FROM information_schema.STATISTICS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'file_object'
    AND INDEX_NAME = 'idx_status_reclaim');
SET @ddl := IF(@idx_exists > 0,
  'ALTER TABLE file_object DROP KEY idx_status_reclaim',
  'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @col_exists := (SELECT COUNT(*) FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'file_object'
    AND COLUMN_NAME = 'reclaim_at');
SET @ddl := IF(@col_exists > 0,
  'ALTER TABLE file_object DROP COLUMN reclaim_at',
  'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- group_file.object_id 还原为 NOT NULL。若表里已存在 NULL 行（调度器落地时摘过引用），
-- 这条会以 1138 报错——**这是预期的**，不要用哨兵值(如 0)去填，那会造出一条指向不存在
-- 对象的假外键。此时要么保留可空形态，要么人工先处置那些 removed 行。
ALTER TABLE group_file MODIFY object_id BIGINT NOT NULL;

DELETE FROM schema_version WHERE version = 2026100701;
