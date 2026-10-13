# 统一 e2e 运行形态（六套件）

> 生效 2026-10-01（FINDING-DEV-2 收口）。此前 FILE-001 四套件依赖"旧相对形态"（`filesPath=./filesys` +
> `cwd=Server` + exe 取自 `build/build-Server/release/`），与 fastupload 的 dist 形态并存，每次复跑都要
> 临时改配置再还原。现在只有 **一种**形态。

## 形态（唯一）

| 项 | 值 |
|---|---|
| cwd（Server 进程） | `<repo>/dist` |
| exe | `<repo>/dist/Server.exe`（qmake `DESTDIR=dist`，构建期直接落 `dist/`） |
| filesPath | `./filesys`（相对 cwd）→ 实际数据根 `<repo>/dist/filesys` |
| config 来源 | `Server/connect.config`，构建期经 `Server/config.qrc` **内嵌**进 exe；该文件**不入库**（模板 `Server/connect.config.template` 亦为 `./filesys`） |
| 运行方式 | `py -3.13 <脚本绝对路径>`；**cwd 任意**——六个脚本都自管理 Server 生命周期（`taskkill` + `Popen(cwd=dist)`） |

`cross_tree_e2e.py` 的树 B 以 `<repo>/build/xtree-file001` 为 cwd，其 `./filesys` 是 NTFS junction
指向 `<repo>/dist/filesys`——**正是因为 filesPath 为相对路径**，"两棵不同 cwd 的树"才成立。

## 未入库的运行时依赖（不入 git，须本机就位）

- `dist/{Qt5Core,Qt5Gui,Qt5Network,Qt5Sql,Qt5Svg,Qt5Widgets}.dll`
- `dist/libmysql.dll`
- `dist/sqldrivers/qsqlmysql.dll`
- `dist/platforms/qwindows.dll`
- `dist/Server.exe`（由 `pipeline/build.ps1` 生成，见上）
- `Server/connect.config`（机器本地，从 `Server/connect.config.template` 起步；**必须**保持 `filesPath=./filesys`）

## 前置

- MySQL 5.7 本机运行，库 `mydb`，迁移脚本已执行（`Server/migrate_ext_file_00*.sql`，幂等可重放）。
- `py -3.13`（PATH 上的 `python` 是旧 Anaconda，勿用）。
- 串行执行：五个套件都占 127.0.0.1:5000 且都 `taskkill Server.exe`，同一时刻只跑一个。

## 套件与基线计数

| 套件 | 用例数 | 脚本 |
|---|---|---|
| entry_ops | 43 | `REQ-EXT-FILE-001/entry_ops_e2e.py` |
| dedup | 13 | `REQ-EXT-FILE-001/dedup_e2e.py` |
| cross_tree | 41 | `REQ-EXT-FILE-001/cross_tree_e2e.py` |
| sec001 | 48 | `REQ-EXT-FILE-001/sec001_regression.py` |
| fastupload | 46 | `REQ-EXT-FILE-002/fastupload_e2e.py` |
| groupfile | 45 | `REQ-EXT-GROUP-003/groupfile_e2e.py`（GROUP-003 合入时加入，同形态、自管理） |

> 注：套件自身只统计"断言用例"（`check()` 计数）；Server 启动失败以 `FATAL` 直接退出，不计入用例数。
