# REQ-EXT-TRANSFER-003 独立测试报告（independent test 节点）

- 节点：independent test（Development OS v1.7 / LARGE；新会话）。
- 测试对象（候选）：`b861351`（`feat(transfer): REQ-EXT-TRANSFER-003 上传分片 1 MiB / 会话连续性与吞吐`），完整哈希 `b8613514a561143be0d0af5b1ae2ad0a53726ac7`，基线 `master@d85e39f`（merge-base 同）。
- 快照：`git worktree add build/wt-indtest --detach b861351`（干净、HEAD 锁定候选）；`pipeline/` 四脚本与 `build/run_declared_suites.py` 复制自 dev 工作树（三者本就不入库，逐文件比对同版本）。
- 只读约束：本节点**零提交、零生产源码/验收标准改动**。`git status --porcelain --untracked-files=no` = 空（`readonly-confirm.log`）。写盘仅限 gitignored 的 `build/`（含 dist 运行库与 filesys 预热）。一次**负对照实验**在**独立的一次性工作树** `build/wt-negctl`（同 hash）内改一行并单独构建，未触碰候选工作树的源码；实验后候选 `dist/Server.exe` 按 sha256 复位校验（§6）。
- 独立性披露（不掩盖）：任务记录、development、本节点均为 `claude-code`。**跨代理未满足**——成立的是"新会话 + 独立干净工作树 + 独立重建 + 独立复跑 + 独立复核"。同代理补偿：本报告所有判定均来自本工作树自跑原始输出（含自写探针与负对照），不采信 dev 记录；若主人要求跨 CLI 独立测试，请在 independent audit 前裁决。
- **结论：PASS（待 master 的 GUI 5 项人工确认）**——13 套声明回归中 11 套全绿（428 条断言）、2 红均非本候选缺陷（1 既有 harness 缺陷、1 既有门禁口径错位；§5 含逐字节同型复现与源码级定位）；新套件 `transfer003` 53/53（2 轮）且断言↔design §6 矩阵逐行对齐；冻结面与 §9 批准稿**逐字节相等**；dev 声称的 build GREEN 独立复现；dev 标注的"设计外新增"经**负对照证明为必要**（§6）。非阻塞发现 6 条见 §7。needs-human（GUI）5 项未执行、不计入 PASS（§8）。

## 1. 环境与前置（`environment.txt`）

| 项 | 实测 |
|---|---|
| 解释器 | `py -3.13` = Python 3.13.15（`python` 为过时 Anaconda，未使用） |
| DB | MySQL `5.7.3-m13-log`，库 `mydb`（共享——去重按 sha 命中既有 `file_object`） |
| 构建 | 本工作树现构：`pipeline/build.ps1` → **GREEN (b861351, 20261008-195606)**，双端 OK；`{Client,Server}/connect.config` 由 build.ps1 自 main worktree sha256 同步（CRLF 原文） |
| 重建同一性 | 与 dev 工作树 dist 对比：`Server.exe` 同尺寸 509440、逐字节差异 **4**（PE 链接时间戳区）；`Client.exe` 同尺寸 3924480、差异 104 → **同一份代码的独立重建** |
| 运行形态 | 统一 E2E-RUNFORM：exe=`<wt>/dist/Server.exe`、cwd=`dist`、`filesPath=./filesys`；`dist/Qt5Core.dll` = **5.14.2.0**（无 5.9.5 污染面） |
| filesys 预热 | dev `dist/filesys`（257 MB）→ `wt-indtest/dist/filesys`（共享 DB 去重需实体对象在场，否则下载类假红） |
| 串行 | 13 套逐个独占 127.0.0.1:5000；驱动 `run_declared_suites.py`；`SEC001_SBX=<wt>/dist/filesys` |

## 2. Pipeline 三门（本工作树自跑，不采信 dev 记录）

| 门 | 结果 | 原始输出 |
|---|---|---|
| `build.ps1` | **GREEN** hash=b861351 | `build-b861351-20261008-195606.log` + `manifest-build-b861351-*` |
| `test.ps1` | **UNTESTED（exit 3）**——门册 `test_gate.status: waived`（owner 决定在册） | 按既定实践以驱动逐条手动执行套件取真证据，结论如实标注"手动执行" |
| `audit.ps1` | **RED（两条均为本任务预期内的信号）** | `pipeline-audit.log`：① `FROZEN-PATH HIT: Server/protocol.h` = 本需求**经主人批准的冻结面变更**（tier=LARGE + design §10 批准记录在册；脚本文案"escalate to LARGE + user approval"的两个前提均已满足）② `NO test evidence for b861351` = gate waived ⇒ test.ps1 不跑套件，本节点的手动扫尾（§5）即该证据 |

本工作树的 `pipeline/audit.ps1` 是**已适配版**（读 manifest `frozen_paths`、`codeRoots` 含 `Client/Server`），故冻结检查**确实在跑**；其 RED 是"侦测到已授权变更"，而非失效（与 TRANSFER-002 节点的骨架版失真不同）。

## 3. 独立静态核验（FACT；`indtest-static-review.txt`，脚本 `indtest_frozen_check.py`）

| 核验项 | 方法 | 结果 |
|---|---|---|
| 冻结面内容 == §9 批准稿 | design.md §9 代码块（`sha256=3e90e350…`）vs `Server/protocol.h:197-201` | **逐字节相等**（531 字符 / 5 行） |
| 套件常量未"同源抄错" | verify.py 内嵌 `APPROVED` 字面量 vs design §9 | **逐字节相等** |
| 分支 diff 事实 | `git diff master...HEAD -- Server/protocol.h` | **+5/−2、单 hunk**；新增行 == 批准稿 5 行；删除行 == 原两行常量 |
| 双端冻结面 | `-- Client/protocol.h` | **零改动** |
| 越界读风险（§2.2 声明） | `recvmsg` 的 `while(m_dpulist.size() >= sizeof(PDU))` | `uiType` 读点在 76 B 头已确认到达之后 → **不越界**（design §2.2 成立） |
| 唯一读点 | `grep MAX_INBOUND_PDU_SIZE\|MAX_PENDING_INBOUND_BYTES` | 仅 `mytcpsocket.cpp:651`（按类型取上限）与 `:636`（累计上限），无第二处遗漏的旧上限校验 |
| D5 位置 | `mytcpsocket.cpp:352`（超长块拒绝）先于 `:367`（`writeInFlight=true`） | 拒绝路径不动会话状态、不留"在途"闩 → 无卡死面 |
| 别名句柄扫描域 | `MsgHandler` 每连接一份（`mytcpsocket.cpp:26`） | `releaseOtherStagingHandles` 只扫描**本连接**的会话表 → 跨连接的并发同名上传不受它影响（G8 面维持主人裁决的现状） |
| 会话行写入点 | 三处 `createUploadSession`（1250/1545/2422）+ 秒传 `insertCompletedUploadSession`（1614） | 三处**均有"先按暂存键复用"前置**；秒传登记 done 行属既有设计（I5 只约束"未完成行唯一"） |
| 句柄释放先于动文件（I7） | `abortUpload`（含 `pendingAbort` 延迟收尾都走同一函数）与 `finalizeUploadOnComplete` 均在 truncate/rename 前释放 | **成立**；在途写因共享所有权持自己的引用，不会被提前关闭 |
| 断线清理 | `clientOffLine` → `m_uploads.clear()` | 会话销毁即释放共享句柄（无泄漏句柄锁住 .part） |

## 4. 新套件断言↔design §6 映射与重点复核（逐项核对套件源码，非采信汇总数）

53 条 = 静态 14（S-01a/b、S-02a/b、S-03a/b/c、S-04a–d、S-05a/b/c）+ 动态 37（T3-01…T3-12，每行 ≥1 条）+ 环境 2（E0/E0b）；`manifest asserts: 53` 与脚本汇总一致。**与 §6.1/§6.2 矩阵逐行对应、无缺行**。断言强度抽查：

- **T3-03（D7 复用键）**：97(新 reqId) 再入 → 行数仍 1 且 `client_request_id` == 最后 reqId（实测 `2b2b…`）；完成仍 1 行。**D7 收敛成立**。
- **T3-10（AUD-4 锚点）**：6 轮"31 发出即 107"→ 108 全 true、`confirmed_offset == 暂存物理大小`、续传起点 == 暂存大小。**I6 锚点保持**。
- **T3-02/04（单行贯穿）**：个人 29 路径与群 101 路径均"暂停→续传→完成"全程 1 行（`done`）。
- **T3-09**：107 后 30 回带 `resumeOffset == 暂存物理大小`，且后半段用**不同内容**写入、最终全等 == 前缀来自暂存（I3 锚点保持）。
- **T3-05c**：正文 1 MiB+1 走 D5 → `32{ok=false}` 且**后续合法块被接受**（连接存活、账目不错乱）。
- **T3-11/T3-12**：17 KB 非 31 报文仍按非法 PDU 处理；id=0 + 4 KB 旧路径全流程逐字节不变（I1/I2）。

独立补充探针（自写、不复用 dev 套件；`indtest-probe.log`，21/21）：

| 探针 | 覆盖（dev 套件未覆盖面） | 结果 |
|---|---|---|
| X-1 | 1 MiB 块切成 **256×4 KiB 慢发**（逼出跨 recv 的累计缓冲路径，验证 D3 的必要性） | 4 块全接受，暂存物理大小 == 4 MiB |
| X-3 | 31 整包 **恰好 == `MAX_UPLOAD_DATA_PDU_SIZE`**（正文 1 MiB + 4020） | 解析通过 → D5 显式拒绝 `32{ok=false}`、**连接存活** |
| X-4 | 拒绝之后：合法块仍被接受（next == 1 MiB）、无脏写、补末块后 done 且下载逐字节一致 | 全绿 |
| X-5 | **跨连接**并发（两个用户、两条连接各 3 MiB 大分片） | 两路各自逐字节正确、各自单行会话 |
| X-2 | 31 整包 = 上限 + 1 的越界面 | 见 §7 IND-5（连接被丢弃、进程存活、会话态不变） |

## 5. 13 套独立复跑（驱动串行；`indtest-sweep.log` + 逐套 `suite-*.log`）

| 套件 | 实测 | 判定 |
|---|---|---|
| sec001 | **48/48 rc=0** | 绿 |
| sec001-regression | **48/48 rc=0** | 绿 |
| entry-ops | **43/43 rc=0** | 绿 |
| dedup | **13/13 rc=0** | 绿 |
| cross-tree | **41/41 rc=0** | 绿 |
| fastupload | **46/46 rc=0** | 绿 |
| group-002 | **rc=1，1 s 内、任何断言之前** | 既有（§5.1） |
| groupfile | **45/45 rc=0** | 绿 |
| group004 | **23/23 rc=0**（`commit=b861351` 自戳） | 绿 |
| sec002 | **14/14 rc=0** | 绿 |
| unique-login | **15/16 rc=1**，单红 E7b | 既有口径错位（§5.2） |
| transfer003（新） | **53/53 rc=0**；独立复跑第 2 轮 53/53；T3-06a **33.07 / 35.71 MiB/s**（阈值 8，基线 0.28 @4 KB） | 绿 |
| parallel-transfer | **54/54 rc=0** | 绿 |

**合计 428 条断言全绿 / 2 红均非本候选缺陷。** 6 套既有 e2e（entry_ops/dedup/cross_tree/fastupload/groupfile/parallel）以旧帧形态通过 → 不变式 I1 的独立复证。

### 5.1 group-002 红——既有 harness ROOT off-by-one，FACT 级归因（`group002-attribution.log`）

`verify.py:32` `ROOT = parents[2]` 解析到 `project-docs/`，而 `:243`/`:579` 取 `ROOT/'Server'/'connect.config'` → 实际配置在仓库根 `Server/connect.config`（存在）。本候选**未触碰**该 harness（`git diff --name-only master...HEAD -- …/REQ-EXT-GROUP-002/` 为空）；本工作树与本节点之外两次独立运行（dev@e49e8aa / 本次@b861351）输出**除 commit 戳外逐字节相同**。**非本候选缺陷。**

### 5.2 unique-login 单红——E7b 对本分支结构性红，FACT 级归因

单红 = **E7b「分支 diff 对两 protocol.h 零命中」**（`unique_login_e2e.py:202-205`，`git diff master...HEAD --name-only -- Server/protocol.h …` 含 protocol.h 即红）。本分支**经主人批准的冻结面变更**（§3）必然命中 → 该断言对任何改协议头的分支结构性红；E7a（内嵌 SEC-001 回归）本轮 48/48 绿。与 TRANSFER-002 独立测试报告 F-1 同型，**非本候选缺陷；测试节点不放宽任何判定**，处置属 design owner（改口径为"仅限 §9 批准块"或移出回归面）。

## 6. dev 标注的"设计外新增"独立判定：`releaseOtherStagingHandles` 必要（FACT，负对照 A/B）

dev 在 handoff blockers② 自报：§3.1 S5 之外新增 `releaseOtherStagingHandles(keepId, stagingPath)`（finalize/abort 前释放**同一 .part** 的别名上下文句柄）。

- 静态面：定义 `Server/msghandler.cpp:1313-1326`；调用点 `:1364`（abort）与 `:1746`（finalize）；扫描域 = 单连接（§3）；不触碰冻结面、不改验收口径 → 不触发新批准（批准对象只有契约）。
- **负对照实验**（独立工作树 `build/wt-negctl` @b861351，仅去掉 finalize 处的那一次调用，其余全同；`negctl-patch.diff`）：

| 二进制 | 场景（97 预检查槽 + 传输槽共用同一 .part → 完成 → rename） | 会话行 | 对象 | 下载 |
|---|---|---|---|---|
| 候选（正对照，`negctl-positive.log`） | 32 complete=1 | `done` / confirmed=2097152 / 1 行 | 2097152 | 逐字节一致 |
| 去掉该调用（负对照，`negctl-negative.log`） | 32 complete=1 | **`failed` / confirmed=0** | **无** | ok=false |
| 复位后复跑（`negctl-restored.log`） | 同上正对照 | `done` / 1 行 | 2097152 | 逐字节一致 |

**判定：该调用是 load-bearing（不加则 rename 被 Windows 拒绝 → 会话判 failed）**——dev 的说法由 claim 升级为 fact；同时确认"5 个释放时机"在实现上确实需要"含别名上下文"这一层，属 §3.1 S5/I7 的**必要补全**，非范围外功能。

## 7. 非阻塞发现（claim vs fact 标注）

- **IND-1（LOW；FACT）** `mytcpsocket.cpp:635` 注释与 `:637-638` 日志文本仍写 **"256 KB"**，而常量已改为 1 MiB + 8 KB → 诊断文本误导（越界断连时给出错误阈值）。**非行为缺陷**；建议随下一次改动顺带把日志改为打印常量值（或并入其他 SMALL）。
- **IND-2（LOW；FACT，证据溯源）** dev 节点的 13 套扫尾证据**戳在 e49e8aa（提交前 HEAD）**：`suite-group-002.log` / `suite-group004.log` 自戳 `commit=e49e8aa`，日志 mtime 19:24–19:29，而 `b861351` 提交于 19:48:57（`dev-sweep-provenance.log`）。内容等价性由本轮独立重建证明（Server.exe 差 4 字节 PE 时间戳）。后果**仅**限于基于 git diff 的断言：unique-login E7b 在 dev 的提交前扫尾里真空通过，在候选提交上结构性红（§5.2）。**处置建议**：以本轮 `b861351` 扫尾为准（本报告）；owner 收尾时把本轮扫尾归档进 `project-docs/evidence/`。
- **IND-3（既有；FACT）** unique-login `E7b` 口径错位（§5.2）。处置归 design owner；测试节点不放宽。
- **IND-4（既有；FACT）** group-002 harness `ROOT` off-by-one（§5.1）。归 harness 修复（另立 SMALL 或并入既有 harness 修复面）。
- **IND-5（契约边界说明；FACT + 一处 claim）** 31 整包 **> 新上限**（本例上限+1）走**既有非法 PDU 路径**：`32` 无回执、**该连接被丢弃**（1 MiB 级越界包被内核切成多次 recv，残段按垃圾头继续计入非法计数，达阈值 5）、**服务端进程存活**（监听 PID 不变、新连接可建）、**会话账目不变**（未被部分受理）。**claim（标注为未独立复测）**：改前同一发送会先撞 256 KB 累计上限而断连（行为同类、路径不同）；未在 master 上重建 A/B。与 design §2.1"越界走既有非法 PDU 路径（计数后断连）"一致；D5 只覆盖正文 ≤ 1 MiB 的显式拒绝（X-3/T3-05c 已证）。**非缺陷**，但"越界只断连、不给明确错误"值得主人知情。
- **IND-6（LOW；FACT，文档内不一致）** design §9 末尾自检写"预期 `Server/protocol.h` 为 **+4/−1**（或等价的批准稿行数）"，而批准稿代码块实为 **5 行**、实际 diff **+5/−2**（逐字节等于批准稿）。按 §9 自身"或等价的批准稿行数"口径与 S-01/S-03 的逐字比对，**判定以逐字节相等为准、非以 +4/−1 为准**；建议 owner 顺手更正 design 该行（docs-only）。
- **未独立复测项（诚实边界）**：dev 声称的基线吞吐 0.28 MiB/s（@4 KB）沿用其 goal 节点探针结论，本轮未在 master 上重建对比；验收判据是**绝对阈值 8 MiB/s**，本轮 33–36 MiB/s（≈4.3× 余量）通过。G8（同一 owner 同名同大小的 `.part` 共用）为**主人 2026-10-08 明确裁决不处理**，本节点不复测其跨连接形态（§3 已记录其影响域）。

## 8. NEEDS-HUMAN（未执行，不计为 PASS）

design §6.3 的 5 项纯 GUI 行为本节点**未执行**（需人眼/手工操作；本仓无客户端测试宿主）：①≥100 MB 上传肉眼提速；②DB `confirmed_offset` 随进度前进、完成 == 文件大小；③暂停→继续→完成仅一行且进度不回退；④0 字节文件显示"已完成"且服务端有条目；⑤**G7 断线收敛**（杀服务端 → 秒级失败+原因；重启+重连后「重试」从断点续传；排队任务在断线期间不发新 INIT）。**结论 PASS 不包含它们**；主人一轮完成后按 `human-verified by <owner>, <date>` 记入。

## 9. 证据清单（本目录）

`test-report.md`（本文件）、`environment.txt`、`build-b861351-20261008-195606.log` + `manifest-build-b861351-*`、`pipeline-audit.log`、`readonly-confirm.log`、`indtest-static-review.txt`（+脚本 `indtest_frozen_check.py`）、`indtest-sweep.log` + 13 套 `suite-*.log`、`indtest-probe.log`（+脚本 `indtest_probe.py`）、`x2-isolate.log`（+脚本 `x2_isolate.py`）、`negctl-positive.log` / `negctl-negative.log` / `negctl-restored.log` / `negctl-patch.diff`（+脚本 `negctl_probe.py`）、`group002-attribution.log`、`dev-sweep-provenance.log`。

## 10. 交下一节点

- **主人**：GUI 5 项一轮（§8）。
- **repair（按需，owner 新会话）**：IND-1/IND-6 是文档与日志文案面（产品源码可零改动、或一行日志），IND-2 的扫尾归档，IND-3/IND-4 属设计口径与既有 harness（另立 SMALL）。**均不阻塞 integration**。
- **independent audit（新会话、只读 detached worktree @`b861351`）**：输入 = 本报告 + §5 矩阵日志 + 影响面（`git diff d85e39f...b861351`，15 个源码文件）。建议深查 §4 已覆盖之外的**实现层**：`releaseOtherStagingHandles` 与幂等重放槽的交互、`restartUploadSession` 改键的唯一键竞争、节流落库与完成/失败置值的交错、G7 客户端状态机（`m_connectionDown` 与 `m_fastcheckInFlight*`）。
