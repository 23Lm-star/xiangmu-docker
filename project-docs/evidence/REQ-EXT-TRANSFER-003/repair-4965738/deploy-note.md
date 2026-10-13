# 部署与人工复验请求（repair 节点，2026-10-09）

## 1. 修复内容（FINDING-IND-7：断线重登后在途任务消失，重试续传不可达）

提交 `4965738`（单提交），只动客户端：

- **R-1（根因）**：`MsgHandler::handllogin()` 改为 `QString` 返回，登录名同步交给
  `Client::handlMsg` 的 LOGIN_RESPOND 分支，先落 `m_LoginName` 再 `setCurrentUser(...)`；
  1.5 s 定时器只保留 `resetGroupStates()`/`Index::show()`/`Client::hide()` 与既有延迟刷新。
- **D-a（防御）**：`saveSnapshot()` 不再"无用户静默 return"——以最近一次有效登录名
  `m_lastKnownUser` 兜底，每条 return 前有 `qCWarning` 告警。
- **D-b（防御）**：`setCurrentUser()` 同名/空名早退，清表只发生在"确实是另一个账号";
  `restoreSavedTasks()` 按任务身份去重合并（内存为准、快照补缺）。
- 冻结面（两 `protocol.h`）零改动；`transfers_<user>.json` 位置与命名不变。

## 2. 双端已重新部署到主 dist（供人工复验）

| 文件 | sha256（前 16） | 大小 | 003 标记 | 修复面标记 | 备份 |
|---|---|---|---|---|---|
| `E:/qt/xiangmu/dist/Server.exe` | `e0ec1c973ea5d0cd` | 509,440 B | D5 串命中 1 | — | `build/backup-b861351/Server.exe`（`c4df72bea9a05b33`） |
| `E:/qt/xiangmu/dist/Client.exe` | `034707ea6f55828d` | 3,924,992 B | G7 串命中 2（UTF-16） | 1+1（两条新 qCWarning，UTF-8） | `build/backup-b861351/Client.exe`（`aa724f121daf6a64`） |

标记法自证（同法三份原始输出见 `deploy-markers-*.log`）：修复前候选对（备份）G7 串命中 2、
修复面标记 0；修复后命中 2 + 1 + 1。Server 源码本次未改（`Server/` 零 diff），与 b861351 等价，
差异只是构建出处（dev 工作树重建）；本轮 13 套回归正是用这一对跑的。

**未**用 `artifact-trace.py record` 落 current：主工作树 HEAD 是 002 分支，与部署 intent 不一致，
落记录会伪造匹配（口径同 `findings` 目录 `deploy-and-cleanup-note.md §1`）。`state.ps1` 报 STALE 属预期。
部署前占用 5000 端口的遗留 `E:/qt/xiangmu/dist/Server.exe`（PID 30232，09:19:00 启动）经主人确认后结束。

## 3. 请主人执行的人工复验（finding §3 五步；文件与端口均已就绪）

1. cwd = `E:/qt/xiangmu/dist`，启动 `Server.exe`，再启动 `Client.exe`，登录 `wcy`；
2. 记下快照文件 `C:/Users/王晨扬/AppData/Local/Client/transfers_wcy.json` 的时间戳（基线）；
3. 传一个大文件（如 200 MB+），等面板出现"传输中"；查 json：**应有该任务**；
4. 杀掉服务端进程：任务行应在秒级内变"失败"并给出原因（G7 行为不变）；
5. 重启服务端 → 客户端重连并**重新登录**：任务行**应仍在列表**（"失败"），选中点"重试"：**从断点续传完成**。

第 5 步是本缺陷的判定步：修复前任务消失（F2/F3），修复后内存保留 + 快照兜底。

## 4. 证据文件对应关系（复核用）

- `build-b861351-20261009-092750.log` = 修复代码的实际编译日志（构建时未提交，故戳 b861351，0 warning）；
  `build-4965738-20261009-093939.log` = 提交后的空转重建（同源同二进制，戳 4965738），两轮 exe sha256 一致。
- `static-contract.log` = verify.py 静态段（20/20）；`negctl-static.py` 用 `git show b861351:` 读旧码重跑，
  `negctl-static.log` 显示新增 S-06a..S-06f 6/6 FAIL（无永真）。
- `verify-transfer003.log` = 全套 59/59；`suites-summary.log` + `suite-*.log` = 13 套（11 绿；
  group-002 = harness ROOT off-by-one 既有红，unique-login = E7b protocol.h diff 结构性既有红，两者口径未动）。
- 三个本地工具（`static-contract-check.py` / `negctl-static.py` / `marker-check.py`）已随证据归档，可重跑。
