-- REQ-EXT-GROUP-003：群文件目录与访问控制迁移（目标版本 2026100103）。
-- 幂等、可重复执行；服务端不会运行时静默建表，执行失败必须人工处理。
-- 附加式：只新增 group_file，不动 GROUP-001/002 与 FILE-001/002 的任何既有表/数据/语义。

CREATE TABLE IF NOT EXISTS group_file (
  group_file_id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  group_id      BIGINT UNSIGNED NOT NULL,             -- 群标识（身份源同 GROUP-001）
  file_id       BIGINT          NULL,                 -- → user_file.file_id（逻辑文件标识；删个人条目不级联群）
  object_id     BIGINT          NOT NULL,             -- → file_object.object_id（物理对象；下载/去重/引用计数）
  uploader_name VARCHAR(32)     NOT NULL,             -- 服务端 actor 推导，非客户端自报
  display_name  VARCHAR(70)     NOT NULL,             -- 群目录展示名快照（与上传者个人根解耦）
  size_bytes    BIGINT          NOT NULL,             -- 快照
  status        ENUM('active','removed') NOT NULL DEFAULT 'active',
  created_at    TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at    TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  UNIQUE KEY uk_group_file_name (group_id, display_name),
  KEY idx_group_file_group (group_id, status),
  KEY idx_group_file_uploader (group_id, uploader_name),
  CONSTRAINT fk_gf_obj FOREIGN KEY (object_id) REFERENCES file_object(object_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

-- 独立版本号：与 2026100101(GROUP-002)、2026100102(FILE-002) 均不同名，
-- 否则"未执行本迁移"的库会通过启动检查，fail-fast 失效。
INSERT IGNORE INTO schema_version (version) VALUES (2026100103);
