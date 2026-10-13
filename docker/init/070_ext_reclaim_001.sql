-- REQ-EXT-RECLAIM-001 周期清理调度器 迁移脚本（目标版本 2026100701）
-- 执行：mysql -u<user> -p <db> < migrate_ext_reclaim_001.sql
-- 附加式（additive）：给 file_object 加一列、扩一个枚举值、加一条索引；把
-- group_file.object_id 放宽为可空。不新增表、不删数据、不改既有列语义。
-- 幂等可重放：MySQL 5.7 无 ADD COLUMN IF NOT EXISTS，用 information_schema 判断
-- （与 migrate_ext_file_002.sql 同一套口径）。
-- 回滚见 migrate_ext_reclaim_001_rollback.sql。
--
-- 本迁移是"延迟删除的收割者"能落地的前提。三处改动各自不可省：
--   1) reclaim_at  —— 宽限期（grace_hours）必须有一个"最后一次引用消失"的锚点（见下）
--   2) reclaiming  —— 调度器抢删除权用的中间状态，把"删的人 vs 复活的人"的竞争收成
--                     同一行上的两个条件 UPDATE（见 reclaimscheduler.h 的三条红线）
--   3) object_id 可空 —— 不这么做，file_object 的行根本删不掉（见下，这是硬阻塞）

-- ---------------------------------------------------------------------------
-- 1) file_object.reclaim_at：最后一次引用消失的时刻（宽限期锚点）
--    由四个标记点在同一句 UPDATE 里顺带写入：reclaim_at=IF(refcount<=0,NOW(),NULL)
--    （依赖 MySQL 单表 UPDATE 赋值从左到右求值，本文件里既有代码早已依赖这一点）。
--    写完之后，抢权(reclaiming)与回退(pending_reclaim)**都不再改它**——否则每次重试都会
--    把宽限期重新计时，一个 unlink 一直失败的对象就会变成"每过一个宽限期才重试一次"，
--    永远等不到下一轮。
--    （这也是不用 updated_at ON UPDATE CURRENT_TIMESTAMP 的原因：那条列会被抢权和
--      回退自动刷新，正是上面这个毛病。）
SET @col_exists := (SELECT COUNT(*) FROM information_schema.COLUMNS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'file_object'
    AND COLUMN_NAME = 'reclaim_at');
SET @ddl := IF(@col_exists = 0,
  'ALTER TABLE file_object ADD COLUMN reclaim_at DATETIME NULL COMMENT ''最后一次引用消失时刻（宽限期锚点）''',
  'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- ---------------------------------------------------------------------------
-- 2) file_object.status 扩一个值 reclaiming：调度器抢到的删除权。
--    MODIFY 天然幂等（重复执行结果相同）。ready / pending_reclaim 语义不变，
--    现有读写路径不受影响（复活侧新增的谓词同时放行原有两种状态）。
ALTER TABLE file_object
  MODIFY status ENUM('ready','pending_reclaim','reclaiming') NOT NULL DEFAULT 'ready';

-- ---------------------------------------------------------------------------
-- 3) 候选查询索引。候选 SQL 形状：
--      WHERE status='pending_reclaim' AND reclaim_at < NOW()-INTERVAL ? DAY
--      ORDER BY reclaim_at ASC LIMIT ?
--    没有这条索引，每轮候选查询就是 file_object 全表扫描——直接违背设计里
--    "走索引扫候选、分页限量、绝不扫全表" 这一条。全文只有 PRIMARY(object_id)
--    与 UNIQUE uk_sha256，status 上原本无索引。
SET @idx_exists := (SELECT COUNT(*) FROM information_schema.STATISTICS
  WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'file_object'
    AND INDEX_NAME = 'idx_status_reclaim');
SET @ddl := IF(@idx_exists = 0,
  'ALTER TABLE file_object ADD KEY idx_status_reclaim (status, reclaim_at)',
  'SELECT 1');
PREPARE stmt FROM @ddl; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- ---------------------------------------------------------------------------
-- 4) group_file.object_id 放宽为可空。这是**硬阻塞**，不是优化：
--    file_object 上有两条外键——fk_uf_obj(user_file.object_id)、
--    fk_gf_obj(group_file.object_id)。软删条目与已移除群文件都**保留** object_id，
--    于是"引用归零"之后外键仍然钉着那一行，DELETE FROM file_object 必然报 1451。
--    user_file.object_id 本来就可空，摘掉悬挂引用只需一条 UPDATE；
--    group_file.object_id 是 NOT NULL，摘不掉 → 群文件用过的对象永远回收不了。
--    放宽后可空，与 user_file 的既有形态一致，且 releaseGroupFile 早就写了
--    `if (!obj.isNull())` 的判空分支，说明这个方向本来就是预期内的。
--    安全性：群文件下载读的是 `status='active'`（operatedb.cpp GroupFileDownload），
--    removed 行的 NULL object_id 永不被读取；revive 路径（fileobjectstore.cpp:1055）
--    会连同 status='active' 一起把 object_id 写回。
ALTER TABLE group_file MODIFY object_id BIGINT NULL;

-- ---------------------------------------------------------------------------
-- 5) 存量待回收行回填：宽限期从**本次迁移时刻**起算，不从 created_at 起算。
--    created_at 是对象创建时刻，早于真实归零时刻；拿它当锚点会让"刚归零不久"的对象
--    立刻满足宽限期条件而被回收——正好是宽限期要防的那件事。宁可多留一个宽限期。
UPDATE file_object SET reclaim_at = NOW()
WHERE status = 'pending_reclaim' AND reclaim_at IS NULL;

-- ---------------------------------------------------------------------------
-- 6) 独立版本号。必须与 2026092901 / 2026093001 / 2026100101 / 2026100102 /
--    2026100103 都不同名，否则"未执行本迁移"的库会通过启动检查，fail-fast 失效
--    （启动检查读 MAX(version) 要求 >= 各需求常量）。
INSERT IGNORE INTO schema_version (version) VALUES (2026100701);
