# 部署与清理记录（2026-10-08 22:3x，独立测试节点收尾）

## 1. 候选双端已部署到主 dist（供人工面重跑）

| 文件 | sha256（前 16） | 大小 | 003 标记 | 原文件备份 |
|---|---|---|---|---|
| `dist/Server.exe` | `c4df72bea9a05b33` | 509,440 B | D5 串命中 1 | `build/backup-pre003/Server.exe`（`cb87a0fa03a56e98`, 506,368 B, mtime 15:55:24） |
| `dist/Client.exe` | `aa724f121daf6a64` | 3,924,480 B | G7 串命中 2（UTF-16） | `build/backup-pre003/Client.exe`（`3f934d394751f61d`, 3,924,992 B, mtime 15:58:48） |

- 部署源 = 本节点从 b861351 独立重建并跑完全部回归的那一对（`build/deploy-003/` 留存）。
- `dist/Qt5Core.dll` 已核 5.14.2.0（无 5.9.5 污染面）。
- **未**用 `artifact-trace.py record` 落 "current" 记录：主工作树 HEAD 是 d44ae11(002 分支)，而部署的是 b861351 候选——落记录会把"不匹配"写成"匹配"，正是既往踩过的坑。`state.ps1` 因此会报 STALE，属**预期**，不是故障。
- 运行方式不变：`dist/Server.exe` + `dist/Client.exe`，cwd=`dist`，filesPath=`./filesys`（内嵌配置）。

## 2. 已完成的清理（允许范围内、路径精确）

- `dist/filesys/.staging/5d7c572e….part`（12.3 MB，goal 节点探针残留，无任何会话行引用）→ 删。
- `dist/filesys/.staging/_probe_open.tmp`（1 MB，open 成本探针残留）→ 删。
- 一次性测试工作树 `build/wt-indtest`、`build/wt-negctl` → `git worktree remove --force`（证据已全部归档到本目录；dev 工作树 `build/wt-claude` 按流程保留）。
- **保留**：主人的 `dist/filesys/.staging/90f34db6….part`（810 MB 断点，仍被上传会话行 3954/3957 引用）。

## 3. 被权限层拦下的部分（转交主人执行）

共享 `mydb` 的测试数据清理（69 个测试账号 / 46 个测试群 / 117 行 user_file / 99 行 upload_session /
2535 行 login_record / 11489 条群消息 / 71 个仅测试引用的对象行及其实体，**保留 `wcy` 全部数据**）：
本会话内两次执行均被权限层拒绝（判定为"不可逆改动共享库，且问答授权不可核验"）。计划已干跑复核（`build/cleanup_test_data.py`，默认干跑）。
主人自行执行：

    cd E:/qt/xiangmu && py -3.13 build/cleanup_test_data.py --execute

（或给权限层加一条 Bash allow 规则后由 AI 执行。`wcy` 的行、对象文件与 810 MB 断点全程保留。）
