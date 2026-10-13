# REQ-EXT-FILE-002 独立测试报告（2026-10-01）

- 分支 `codex/req-ext-file-002-fast-upload`，被测候选 `c452eb3`（detached worktree `build/wt-test-file002`，只读）。
- 档位 LARGE，节点 `test`（沿用 `task.md` L6；`validate-handoff.py` 无 `independent-test` 节点名）。
- 手段：干净 detached worktree 从零构建，独立重跑 `fastupload_e2e.py` + FILE-001 四套件 + pipeline 三门。
- 未改任何产品源码 / 测试 / 验收判据；唯一改动是**本报告与证据**，以及 worktree 内未入库的运行配置（`Server/connect.config`，跑完已还原）。

## 1. 运行环境与独立复现

- detached worktree @ `c452eb3`；`pipeline/build.ps1` 从零构建 **GREEN**（含 Client/Server）。
- MySQL 5.7 本机 `mydb`；启动前 `schema_version` 已含 `2026100102`（迁移幂等，重放无副作用）。
- Python `py -3.13`。
- **运行形态判读（独立测试必须自行准备，非产品缺陷）**：
  - build.ps1 从主工作树同步的 `Server/connect.config` 中 `filesPath` 是**绝对路径** `E:/qt/xiangmu/dist/filesys`；
    detached worktree 的 exe 内嵌此路径，故 Server.exe 必须以**该绝对树**为 cwd 启动（相对 `./filesys/<user>` PDU
    按 cwd 解析）。首跑误以 worktree 自身 `dist/` 为 cwd → 所有路径请求被 `isPathInSandbox` 拒绝、FASTCHECK 全 `ret=false`（假红）。
  - 对齐后 dist 形态复现 46/46；另按部署前相对形态（`filesPath=./filesys` + `cwd=Server` + exe 在
    `build/build-Server/release/`，用 `dist/*.dll`+`platforms/`+`sqldrivers/` 覆盖陈旧 DLL）再跑一次 46/46。

## 2. 结果总表（本节点独立重跑）

| 项目 | 结果 | 日志 |
|---|---|---|
| 构建 worktree @ `c452eb3` | GREEN | `build/evidence/.../itest-build-*.log` |
| `pipeline/test.ps1` | GREEN-WARN（仓库注册 0 个自动化测试；**不主张绿灯**） | `itest-pipeline-test.log` |
| `pipeline/audit.ps1` | RED: `frozen:Client/protocol.h` + `frozen:Server/protocol.h`（**预期签名**，判读见 §3） | `itest-pipeline-audit.log` |
| `fastupload_e2e.py` dist 形态 | **46/46 PASS**（重跑 2 次稳定） | `itest-fastupload-dist.log`、`-dist-rerun.log` |
| `fastupload_e2e.py` Server-cwd 形态 | **46/46 PASS**（重跑 2 次稳定） | `itest-fastupload-servercwd.log` |
| FILE-001 `dedup_e2e.py` | 13/13 | `itest-dedup_e2e.log` |
| FILE-001 `entry_ops_e2e.py` | 43/43 | `itest-entry_ops_e2e.log` |
| FILE-001 `cross_tree_e2e.py` | 37/37 | `itest-cross_tree_e2e.log` |
| FILE-001 `sec001_regression.py` | 48/48 | `itest-sec001_regression.log` |

结论：开发报告 §2 的 46/46 与四套件 13/43/37/48 **均可独立复现**。

## 3. 冻结面独立判读（design §5 / 报告 §4.2）

`audit.ps1` 报 `frozen` 命中是预期签名（改动正是 `protocol.h`）。按批准清单逐项独立比对：

| 项 | 判据 | 独立结果 |
|---|---|---|
| ① 只追加不重编号 | 旧编号 1–96 值不变 | PASS（hidden-index 解析 + 抽样 1/15/29/31/40/88/96；`git diff` 仅新增 97/98） |
| ② 两端常量块逐字节一致 | Client vs Server FASTCHECK 常量块 | PASS（md5 级逐字节相等，1352B） |
| ③ 无既有 PDU 语义变更 | diff 仅追加、无既有枚举/布局改动 | PASS |

## 4. §4.1 偏差复核（schema_version，需主人知悉）

- **事实成立**：`operatedb.h:22 kGroupMessageSchemaVersion = 2026100101`、`operatedb.h:26 kFastUploadSchemaVersion = 2026100102`；
  `operatedb.cpp:96-98` `required = qMax(..., qMax(kGroupMessageSchemaVersion, kFastUploadSchemaVersion))`。
  若 FILE-002 也取 `2026100101`，则"只跑 GROUP-002 迁移、未跑 FILE-002 迁移"的库会通过启动 fail-fast，而运行期缺列必炸。
- **判定**：偏差合理、属"新增版本号并入 required 计算"的批准范围，非范围扩张。`design.md:87`、`task.md` 批准行的数值应更正为 `2026100102`。迁移脚本 `migrate_ext_file_002.sql` 末行确为 `INSERT IGNORE ... VALUES (2026100102)`。

## 5. FINDING 复核（报告 §4.3）

| 编号 | 独立结论 | 依据 |
|---|---|---|
| FINDING-DEV-1 | **不可复现（claim 与 fact 不符）** | 本机 PS 5.1（`[Text.Encoding]::Default = gb2312` / ACP 936）下，`audit.ps1` 的 manifest 解析在默认、`-Encoding Default`、cmd `chcp 936`、`chcp 65001`、LF/CRLF 六种组合下**均解析出两条冻结路径**，本报告 `itest-pipeline-audit.log` 报**两个** FROZEN-PATH HIT。.NET 对 UTF-8 中文注释按替换字符解码，不吞换行，故两行不会并成一行。开发报告 `run-logs/audit.log` 只显示 1 项，与当前环境不符。→ 建议 dev 提供其确切调用环境，或撤回该 finding。**副结论**：冻结检查实际覆盖 `Server/protocol.h`，契约批准要求被正确执行。 |
| FINDING-DEV-2 | **复现并确认** | detached worktree 以自身 `dist/` 为 cwd 启动 → 全部路径请求被拒（§1 假红）；按对齐形态跑绿。运行形态差异，非产品缺陷；建议另开 SMALL 统一。 |
| FINDING-DEV-3 | **确认（代码事实 + 数据事实）** | `operatedb.cpp:229` canonical 选择谓词 `parent_path LIKE '/%' AND LIKE '%/' AND NOT LIKE '%//%'` 未含 `isLogicKeyForm` 新增的两条否证（含 `':'`、首段 = fs 根 basename）；本机 `SELECT COUNT(*) FROM user_file WHERE parent_path LIKE '/filesys/%'` = **0**，存量无该类行，缺陷潜伏未触发。 |
| FINDING-DEV-4 | **确认** | 代码库无定期清理调度器；`expires_at` 有列无消费者。e2e 以不变式验证（清理后对象/条目零影响）通过。 |
| FINDING-DEV-5 | **确认** | `Server/msghandler.cpp:510` `MKDIR` 用 `QDir::mkdir`，目录已存在返回 false（响应 `ret=0`）；用户根仅首次注册时创建。既有行为。 |

## 6. 归因候选缺陷

**无（0 条）**。本节点全部失败均归因于运行形态准备（detached worktree 的 cwd/绝对 `filesPath` 对齐），非被测代码缺陷。

## 7. 停止条件与结论（design §7）

协议兼容性、服务端复算对照、物理对象唯一性三项均被独立测试客观证明，**未触发停止条件**。

**结论：PASS** —— 候选 `c452eb3` 在被测环境下满足验收 1–6（46/46 + FILE-001 四套件全绿 + 构建 GREEN；
`audit.ps1` RED 为批准范围内的预期冻结签名）。

## 8. 流程备注

- L6 工具缺口：`validate-handoff.py` 的 NODES 无 `independent-test`，故沿用 `node: test`。
- 独立性：会话为**全新上下文**；CLI/owner 与开发节点同为 `claude-code`（技能默认建议不同 agent），本节点为**使用者直接指派**，已在此记录。
- 本节点只读产品面；唯一写入 = 本报告 + 证据 + handoff。修复属 owner 后续会话（repair）。
