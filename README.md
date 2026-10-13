# QT 云盘聊天系统

基于 Qt 5 与自研 TCP 二进制协议的桌面云盘 + 聊天应用（C/S 架构，Qt Widgets）。

> 学习项目：个人学习 / 课程实践作品，非生产环境代码，未做完整的安全加固与性能压测，请勿直接用于生产。

## 功能概览

- 用户注册 / 登录（验证码、失败冷却与锁定）、注销账号
- 好友管理（查找、添加、删除、在线列表刷新）
- 私聊消息（含历史消息拉取）
- 云盘文件：目录管理、上传 / 下载、断点续传、重命名、移动、删除
- 分享：好友协作分享 + 外链分享（创建 / 解析 / 查询）

## 技术栈

- C++11 / Qt 5.14.2（MinGW 7.3.0 64bit）
- 自研 PDU 二进制协议（TCP）
- MySQL 持久化，RAII 数据库连接池（`Server/databaseconnectionpool.*`）
- 自研内存分配组件：线程缓存 / 中央缓存 / 页缓存 + 基数树
  （`Server/ThreadCache.*`、`CentralCache.*`、`PageCache.*`、`ConCurrentAlloc.*`、`memalloc.*`、`RadixTreeHash.h`、`ObjectPool.h`）

## 仓库内容

本仓库只包含**项目源码与本体**：

| 路径 | 说明 |
|---|---|
| `Client/` | 桌面客户端（`.pro` + `.cpp/.h` + `.ui` + `.qrc` + 界面资源） |
| `Server/` | TCP 服务端（业务处理、数据库、文件存储、内存分配组件），含建表与迁移 SQL |
| `project-docs/init_test_data.sql` | 数据库基础表结构 + 测试种子数据（**运行必需**，随仓库分发） |
| `README.md`、`LICENSE` | 项目说明与 MIT 许可证 |

以下内容已由 `.gitignore` 排除，不随仓库分发：构建产物 `build*/`、`dist/`；运行期数据 `Server/filesys/`；本机凭据 `connect.config*`；开发流程记录 `project-docs/`（上述 SQL 除外）；流水线脚本 `pipeline/`。

## 构建

### 依赖

- Qt 5.14.2 mingw73_64：`C:\Qt\5.14.2\mingw73_64`
- MinGW 7.3.0 64bit：`C:\Qt\Tools\mingw730_64`
- MySQL 服务端

### 配置（构建前必须就位）

分别复制模板并填入本机信息：

- `Server/connect.config.template` → `Server/connect.config`，**8 行**，依次为：
  监听地址 / 监听端口 / 文件存储路径 / MySQL 主机 / MySQL 端口 / 库名 / MySQL 用户 / MySQL 密码
- `Client/connect.config.template` → `Client/connect.config`，**3 行**，依次为：
  服务端地址 / 服务端端口 / 本地文件根目录（每个账号的数据落在其下的同名子目录）

两个由实现决定、绕不开的点：

- **必须用 CRLF 换行**。读取处按 `split("\r\n")` 切分（`Server/server.cpp:25`、`Client/client.cpp:107`），存成 LF 会让整个文件变成单行，后续按下标取值越界，进程直接崩溃。
- 配置通过 `config.qrc` **编译进可执行文件**（运行期读 `:/connect.config`），改完必须重新构建才生效。

### 编译

用 Qt Creator 打开 `Server/Server.pro` 与 `Client/Client.pro`（套件选 Desktop Qt 5.14.2 MinGW 64-bit）即可；命令行方式：

```bash
C:/Qt/5.14.2/mingw73_64/bin/qmake.exe Server/Server.pro && mingw32-make -j4
C:/Qt/5.14.2/mingw73_64/bin/qmake.exe Client/Client.pro && mingw32-make -j4
```

Release 构建的产物统一落到仓库根的 `dist/`；Debug 构建保留在各自的构建目录，方便 Qt Creator 里直接运行。服务端在 `Server.pro` 中显式加了 `CONFIG += console`，所以 `Server.exe` 的 stdout/stderr 可重定向到文件或管道。

## 数据库

建库并写入种子数据：

```bash
mysql -u root -p < project-docs/init_test_data.sql
```

会建立 `user_info` / `login_record` / `friend` 三张表，并插入测试账号 `tester`、`friend1`、`friend2`（密码均为 `123456`）。

`Server/migrate_*.sql` 是各扩展功能的增量迁移脚本，每个都配有同名的 `*_rollback.sql` 回滚脚本；`Server/share_link.sql`、`Server/migrate_publish_links.sql` 属于外链分享功能。

## 运行

从 `dist/` 目录启动 —— 工作目录决定配置里 `./filesys` 的落点：

```bash
cd dist
./Server.exe      # 先启动服务端（监听 connect.config 里的地址与端口）
./Client.exe      # 再启动客户端
```

## 许可证

MIT，见 [LICENSE](LICENSE)。

## Docker 部署（本仓库特有）

本仓库是 [QT-](https://github.com/23Lm-star/QT-) 的 Docker 适配版：原项目零改动，本仓库新增跨平台补丁与容器封装，服务端可在 Linux/Windows（Docker Desktop）上一键部署。

```bash
cp .env.example .env      # 可选：修改数据库密码（构建期烧入镜像，改后需 rebuild）
docker compose up -d --build
```

适配内容、初始化 SQL 顺序、已知注意事项见 [docker/README-Docker.md](docker/README-Docker.md)。

- 服务端：`server` 容器监听 5000，Windows 客户端 `connect.config` 首行改为宿主机 IP 即可连接
- 数据库：`mysql:5.7` 容器，schema 由 `docker/init/` 按序自动初始化
- Windows/MinGW 本地构建行为与原项目完全一致（所有补丁都在 `unix`/`#else` 分支内）

