# REQ-EXT-GROUP-003 独立测试报告（independent test 节点）

- 节点：independent test（新会话；本会话只读产品源码，未改 `src/ include/ tests/ tools/ pipeline/`）
- 候选哈希：**0b83aab**（`feat(group-003): 群文件目录与访问控制`）
- 工作树：`E:/qt/xiangmu/build/wt-test-group003`（`git worktree add --detach 0b83aab`，干净）
- 分支：`codex/req-ext-group-003-group-files`（owner: claude-code）
- 日期：2026-10-01
- 结论：**PASS** —— 0 条可归因于本候选的缺陷；作者 harness 与既有回归面全部复现，独立追加性核验通过。

## 1. 环境与前置

| 项 | 实测 |
|---|---|
| DB `mydb` `schema_version` | 已含 `2026100103`（2026-10-01 13:47:20 写入），前四版 `2026092901/2026093001/2026100101/2026100102` 在位 |
| 表 `group_file` | 存在（`SHOW TABLES LIKE 'group_file'` 命中） |
| 解释器 | `py -3.13`（`python` 为过时 Anaconda，按要求改用 3.13） |
| 构建产物 | 本工作树 `pipeline/build.ps1` 现构，`dist/Server.exe`(444416B)+`dist/Client.exe` 于 14:01 生成 |
| 运行形态 | 统一 e2e 运行形态（见 `project-docs/evidence/E2E-RUNFORM.md`）：cwd=`<repo>/dist`、exe=`dist/Server.exe`、`filesPath=./filesys` 相对、CRLF 配置 |

## 2. Pipeline（在本工作树自跑，非采信开发记录）

| 门 | 结果 | 说明 |
|---|---|---|
| build.ps1 | **GREEN** hash=`0b83aab` | Client/Server 均 OK；connect.config 自 main worktree 同步并 sha256 校验 |
| test.ps1 | GREEN-WARN | `0 automated tests registered`，如实（回归由 e2e socket harness 承担，非本门） |
| audit.ps1 | **RED** | `FROZEN-PATH HIT: Client/protocol.h`、`Server/protocol.h` → 属**预期冻结面命中**（契约变更已获主人批准）；`impact-check: NOT_APPLICABLE`（0 注册用例） |

> audit RED 是冻结面边界的正常告警，不是缺陷；本任务本就是冻结面变更（PDU 99–106 追加）。

## 3. 作者 harness 复跑

| 脚本 | 结果 | 与开发节点声称 |
|---|---|---|
| `groupfile_e2e.py`（验收 1–6） | **45/45 PASS** | 一致 |

覆盖：协议静态一致性（两端常量块逐字节）、列表/非成员拒绝、上传落逻辑记录/同内容复用物理对象、下载/移除即时失权（老连接+新连接）、删除授权/引用隔离（软删、refcount 递减、物理对象留存）、路径与越权负向面、既有群面回归。

## 4. 既有回归面复跑（本候选声称零破坏）

| 套件 | 结果 | 开发节点声称 |
|---|---|---|
| dedup_e2e.py | **13/13** | 13/13 ✅ |
| entry_ops_e2e.py | **43/43** | 43/43 ✅ |
| cross_tree_e2e.py | **37/37** | 37/37 ✅ |
| sec001_regression.py | **48/48** | 48/48 ✅ |
| fastupload_e2e.py | **46/46** | 46/46 ✅ |
| `group_e2e_regress.py`（群生命周期） | **15/15** | 15/15 ✅ |

> 群生命周期 harness 不自管 Server 进程；本次由外部以 `PATH` 预置 Qt 运行库方式启动 `dist/Server.exe`（cwd=dist）后运行，属既有已知运行形态，非产品缺陷。

## 5. 独立核验（本轮新增，非作者 harness 结论）

- **冻结面追加性**：`git diff master...0b83aab -- Client/protocol.h Server/protocol.h` → **无任何删除行**（append-only）。
- **枚举水位**：新增仅 `99..106`（GROUP_FILE_LIST/UPLOAD_INIT/DELETE/DOWNLOAD 四对），既有 `97/98` 原样保留，未重编号。
- 结论：与设计 §协议一致。

## 6. 发现（Findings）

- 可归因于候选 `0b83aab` 的缺陷：**0 条**。
- 既有基线缺陷 `FINDING-PRE1`（`getchatMsg` 离线消息重复下发，已在 GROUP-002 阶段另开 SMALL 分支处理）：不在本需求回归面内，本候选未触及该路径，不阻断本节点。

## 7. 复现步骤

1. `git worktree add build/wt-test-group003 --detach 0b83aab`
2. 该树内 `powershell -File pipeline/build.ps1` → `pipeline/test.ps1` → `pipeline/audit.ps1`
3. `py -3.13 project-docs/evidence/REQ-EXT-GROUP-003/groupfile_e2e.py`（脚本自管 Server 生命周期）
4. 五个既有套件依次 `py -3.13 project-docs/evidence/REQ-EXT-FILE-00{1,2}/*.py`（串行，占用 127.0.0.1:5000）
5. 群生命周期：外部启动 `dist/Server.exe`（cwd=dist）后 `py -3.13 project-docs/evidence/REQ-EXT-GROUP-002/independent-test/group_e2e_regress.py`
