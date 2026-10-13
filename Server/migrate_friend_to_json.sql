-- migrate_friend_to_json.sql
-- 迁移现有的 friend 表结构，改为单用户单行的 JSON 数组结构

-- 1. 备份原表
RENAME TABLE friend TO friend_backup;

-- 2. 创建新表
CREATE TABLE friend (
    user_id INT NOT NULL PRIMARY KEY,
    friend_list JSON NOT NULL
);

-- 3. 迁移数据
-- 使用 JSON_ARRAYAGG 将每个 user_id 对应的 friend_id 聚合为数组
INSERT INTO friend (user_id, friend_list)
SELECT user_id, IFNULL(JSON_ARRAYAGG(friend_id), JSON_ARRAY())
FROM friend_backup
GROUP BY user_id;

-- 校验迁移数据量
-- SELECT COUNT(DISTINCT user_id) FROM friend_backup;
-- SELECT COUNT(*) FROM friend;
