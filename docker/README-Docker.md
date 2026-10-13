# xiangmu Docker 版使用说明

原项目（`E:\qt\xiangmu`）封存不动；本目录（`E:\qt\xiangmu-docker`）是独立拷贝，只做 Linux/Docker 适配。

## 适配内容清单（相对原项目的全部差异）

| 文件 | 改动 |
|---|---|
| `Server/Common.h` | ① `Page_t` 补 Linux `#else typedef size_t` 分支；② Linux 分支补 `#include <sys/mman.h>`、`<unistd.h>`；③ `SystemAlloc` 补 `MAP_FAILED` 判定（mmap 失败返回 -1 不是 nullptr）；④ `SystemFree` 加 `page` 参数并实现 `munmap` |
| `Server/PageCache.h` | `_PageToSpan` 补 Linux 64 位 `#else TCMalloc_PageMap3<64-PageShift>` 分支（原来没有 else，Linux 编不过） |
| `Server/PageCache.cpp` | `SystemFree` 调用点传入 `Span->_n`（页数） |
| `Server/Server.pro` | ① MinGW 专用 `-lmingw32 -lqtmain` 包进 `win32{}`；② `unix` 块摘掉 `utf8_source`（g++ 会加 `-finput-charset=UTF-8` 硬校验，GBK 混合编码源码直接编译失败） |
| `docker/` | 全新目录：Dockerfile、init SQL、适配补丁脚本 |
| `docker-compose.yml`、`.env.example`、`.dockerignore` | 全新 |

Windows/MinGW 下本拷贝的构建行为与原项目完全一致（所有改动都在 `unix`/`#else` 分支里）。

## 依赖设计要点

- **MySQL 5.7 固定**：schema 按 5.7 设计（Antelope/COMPACT、索引键 767B 上限），勿升 8.0
- **服务端启动校验** `schema_version >= 2026100701`，不达标拒绝监听 → init SQL 按 `docker/init/` 编号顺序执行
- **init 脚本剔除说明**（实测踩雷，勿加回）：
  - `migrate_friend_to_json.sql` 未纳入——它是死迁移：假设的老表结构（user_id/friend_id）与基线（id/name/friendName）不符，且 operatedb.cpp 实际按 name/friendName 读写（ADR-010 基线恢复后代码回退、脚本残留）
  - `migrate_ext_session_001.sql` 未纳入——裸 `ALTER ADD COLUMN login_version` 不幂等，且基线 user_info 已含该列、login_record 已含全部列，整脚本冗余
- **connect.config 编译进二进制**（qrc），MySQL 主机等配置构建期生成：监听 `0.0.0.0`、DB 主机 `mysql`（compose 服务名）、CRLF 行尾
- **QApplication 无头运行**：镜像内 `QT_QPA_PLATFORM=offscreen`
- 密码在构建时烧进镜像 → 改密码要 `docker compose build` 重建

## 使用

```bash
cd E:/qt/xiangmu-docker
cp .env.example .env        # 需要改密码时编辑 .env
docker compose up -d --build
docker compose logs -f server   # 看到 checkSchemaVersion OK 即启动成功
```

初始化种子用户（密码均为 123456）：
- `tester`（好友：friend1）、`friend1`、`friend2`

客户端连接：Windows 版 Client 连 `宿主机IP:5000`（本机连 `127.0.0.1:5000`）。

## 常用运维

```bash
docker compose down                # 停止（数据保留在 mysql-data volume 与 ./data/filesys）
docker compose down -v             # 连数据库一起清（会触发首次初始化 SQL 重跑）
docker compose build server        # 改了源码/密码后重建
```

## 待实测项（首次编译时验证）

1. GBK 注释在 g++ 下的告警量级（已摘 utf8_source，预期只警告不报错）
2. Qt 5.14 → 5.15 的弃用警告（DEFINES += QT_DEPRECATED_WARNINGS，预期不阻塞）
3. e2e 套件（pipeline/）引用 `dist/Server.exe`，Linux 下产物是 `dist/Server`，跑 e2e 需先适配脚本
