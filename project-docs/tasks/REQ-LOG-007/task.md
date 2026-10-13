# REQ-LOG-007 日志分级治理 — 任务记录

## 档位
- **MEDIUM**（新能力，有验收标准 A1–A9；只动生产源码日志语句级别，不触碰 public contract——`protocol.h` 为 frozen_path 且在本需求禁止清单内）。
- 流程版本：Development OS v1.7。分支：`refactor/logging-levels`。

## 依赖核查（2026-09-29 goal 节点）
| 依赖 | 状态 |
|---|---|
| REQ-AUTH-002（必须先合并，改同一批行） | ✅ 已合并 master @`4b3a376`（no-ff），BRANCH-ORDER 标记完成 |
| REQ-FS-003 | ✅ 已合并 @`f937c90`（复审 PASS），其新增校验日志由本需求一并分级 |
| REQ-REPO-006 历史重写 | ✅ 完成；**文档基线 `b5d7bb3` 已不存在**（commit-map 见 REQ-REPO-006 证据），A7 基线改为重测 |
| REQ-DOC-005（并行分支） | ✅ 本会话开场后 master 前进 @`08265d5`（README/LICENSE 合并，与 LOG-007 文件零重叠）——分支从该 HEAD 切出 |

- 未合并且可能冲突的分支：**无**（`chore/history-purge`、`fix/path-traversal-and-ownership`、`docs/repo-chrome`、`add_数据库连接池功能`、`worktree-agent-*` 均 0 ahead 或已合入）。工作区干净。

## A7 重基线（master@08265d5 实测，行计数 `grep -c "qDebug\|qWarning\|qCritical"`）
| 文件 | 文档基线(b5d7bb3) | 当前 | 差异来源 |
|---|---|---|---|
| Server/msghandler.cpp | 77 | **84** | AUTH-002 −3 凭据行；FS-003 新增校验日志 +10 |
| Server/operatedb.cpp | 38 | **36** | AUTH-002 −4 凭据行 |
| 其余 12 文件 | 同文档 | 同文档 | 无变化 |
| **合计** | 213 | **213**（恰好等值） | — |
头文件（*.h）无日志调用。全部 213 处落在允许清单内。

## 决策日志（开发前定型，待主人批准）
- **D1｜分类 C++ 标识符改名**：文档 §3.1 用 `net/db/fs/auth/app` 作标识符，与 `operatedb.cpp`/`databaseconnectionpool.cpp` 中约 20 处局部变量 `QSqlDatabase db` **冲突**（`qCDebug(db,...)` 会被变量遮蔽，编译失败）。改为标识符 `lcNet/lcDb/lcFs/lcAuth/lcApp`（客户端 `lcCli`），**显示名不变**（`xmu.net` 等），`QT_LOGGING_RULES` 规则串完全照文档。
- **D2｜setFilterRules**（开发期由守卫改为无条件，见"D2 修订"）：初判 Qt 优先级不可靠拟加守卫；实测 env 天然优先，守卫破坏单类语义，最终**无条件** `setFilterRules("xmu.*.debug=false")`。
- **D3｜客户端建 `Client/logging.h`/`Client/logging.cpp`**（单分类 `xmu.cli`，登记入 `Client.pro`），与 Server 对称；弃用文档 §9 的"匿名分类塞 main.cpp"方案。
- **D4｜A6 去路径清单扩大**：文档只点名 file.cpp:411/205/320；实测 `Client/file.cpp` 的 :68、:69、:228、:394 同样打印绝对路径。A6 范围 = 全部路径输出点（去绝对路径、留文件名，级别归 debug）。
- **D5｜Server/mytcpsocket.cpp PDU 转储（~:390-396）含登录包 caData（密码哈希）**：AUTH-002 A6 移交本需求。默认方案：降级 `qCDebug(lcNet)`（默认不输出）+ 保留语句（守 A7）。是否额外涂掉哈希内容属禁止事项 4 的例外 → **需主人裁决**。
- **D6｜过滤规则串**：文档含 `xmu.*.info=true`，info 本就默认开启；建议只保留 `xmu.*.debug=false`（行为等价，字符串更短）。

## allowed paths
文档 §四全部 19 项 + 新增：`Client/logging.h`、`Client/logging.cpp`、`project-docs/tasks/REQ-LOG-007/`、`project-docs/evidence/REQ-LOG-007/`、handoff。禁止事项 1–10 全数继承。

## 验收与证据计划
- A1–A9 照文档 §二；A7 对比基线 = 本文件重基线表（非文档旧表）。
- 人工验证 §7 六项：quiet 启动、`QT_LOGGING_RULES="xmu.*.debug=true"`、单类 `xmu.db.debug=true`、制造配置错误看 qCritical、语句计数、无路径打印。
- 证据目录：`build/evidence/refactor-logging-levels/<hash>/`（pipeline-run / quiet-run / verbose-run / manual-verify / manifest），归档至 `project-docs/evidence/REQ-LOG-007/`。
- 脚本路径修正：skill 脚本实际位于 `C:/Users/王晨扬/.codex/skills/development-os-v1-7/scripts/`（与文档一致）。

## 流程
goal（本节点）→ 主人批准 → dev（先 Server 后 Client，逐文件改级别；先建分类文件+登记 .pro）→ build/test/audit → §7 人工验证 → 提交 → 等主人批准合并 → no-ff 合 master → handoff。**全程不 push。**

## 批准记录
- 2026-09-29：主人批准 D1–D6 全案开始开发；**D5 裁决 = 仅降级 debug、默认不输出、语句与文本保留**（不涂哈希）。

## 开发执行记录
- 分类标识符（D1）：`lcNet/lcDb/lcFs/lcAuth/lcApp` + 客户端 `lcCli`；显示名 `xmu.net` 等不变。
- 过滤规则（D2 修订/D6）：两个 main.cpp 均在 `QApplication` 构造后无条件 `setFilterRules("xmu.*.debug=false")`。
- msghandler.cpp 函数级归属（38–236 auth / 237–369 net / 370–894 fs / 895–941 net / 942–1080 fs）：`sharefile*` 与 publish-link 三函数归 **fs**（文件分享），`getuserinformation`/`chatmsg` 归 net。
- Server 端路径（deldir 等打服务器存储路径）：**保留文本**——服务端日志面向运维自身数据，默认已静默（A6 仅约束客户端）。
- 客户端去路径 8 处（D4）：file.cpp :69/:70/:206/:229/:240/:321/:395/:412（绝对路径 → 文件名或 `<path suppressed>`；语句全部保留）。:240 为实测补漏（文档未列）。
- **A4 错误升级（开发中补充判定）**：语义为"操作真失败"的原 qDebug 升为 `qCWarning(同类)`，共 34 处——operatedb 16（SQL 失败 + 登录/logoff 凭据不符；:646/:654 两条自带 "might be empty" 保持 debug）、连接池 2（MySQL 打开失败）、msghandler fs 13（move/rename/copy/listdir/分享链接 拒绝与失败）、auth 1（:88 验证码连错锁定，:63/:76/:91 单次输入错误保持 debug 防刷屏）、mytcpsocket 2（下载打开/读块失败）、Client msghandler 1（下载写块失败）。升级后 `qCDebug 157 / qCWarning 47 / qCCritical 9`＝**213，A7 持平**；旧形式残留 0。按类：fs 56、cli 60、db 40、net 37、auth 12、app 8。
- 踩坑记录：`Server/msghandler.cpp` 带 UTF-8 BOM，行首插 include 会把 BOM 挤到第 2 行导致 `stray '\357'` 编译错，已字节级修复（BOM 回文件首）。
- 文档旧表偏差已在上方重基线表记录（warn 实为 13、crit 实为 9，源于 FS-003 新增拒绝日志）。

## D2 修订（实测裁决）
- harness 实证 Qt 5.14 mingw：**QT_LOGGING_RULES 优先于程序化 setFilterRules**（env=true+code=false 同设时 debug 放行），文档 §3.2 断言成立。
- 原"环境变量为空才设规则"的守卫有缺陷：设 `xmu.db.debug=true` 时守卫整体跳过 setFilterRules，导致**其他类 debug 也全开**，不满足"单类打开"。
- 改为**无条件** `setFilterRules("xmu.*.debug=false")`：安静/全开/单类三种形态全部实证通过（probe-quiet / probe-verbose / probe-singlecat）。修复 @`b039569`。

## §7 验证结果（六项 PASS，详见证据）
- 机制取证用同构 harness（logging_probe.cpp/.exe）：**GUI 子系统 exe 在本沙箱无控制台且调试 API 被拦截，真实进程日志无法直接捕获（已声明限制，非跳过）**；harness 与生产逐行同构（同规则串、同分类显示名、同守卫逻辑），quiet/verbose/singlecat/precedence 四份输出在证据目录。
- 计数 213→213、旧形式残留 0、客户端路径残留 0（grep 实证）。

## 状态
**已完成：三门 GREEN/GREEN/PASS，§7 六项 PASS，no-ff 合入 master @01ccf89，合并结果三门再次全绿 @01ccf89。证据归档 project-docs/evidence/REQ-LOG-007/。未 push（待主人指令）。**
