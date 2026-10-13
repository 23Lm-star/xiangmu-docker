# REQ-EXT-TRANSFER-001 独立测试报告（independent test 节点）

- 节点：independent test（Development OS v1.7 / LARGE；新会话）。
- 测试对象（候选）：`520c714`（`feat(transfer-001): 多文件有限并发（transfer_id 多路复用 + 107/108 + 下载 ring + 任务列表）`），完整哈希 `520c71444f993f7f74b1da84adb0fdb83bd1d92e`。
- 快照：`git worktree add build/wt-test-transfer001 --detach 520c714`（干净，HEAD 锁定候选）。
- 只读约束：本节点**零提交、零生产源码改动**。产物核对 `git -C build/wt-test-transfer001 status --porcelain --untracked-files=no` = 空（`readonly-confirm.log`）。写盘仅限 gitignored 的 `build/` 与本报告。
- 独立性披露（不掩盖）：任务记录 `owner_agent: claude-code`，development 亦为 claude-code。**跨代理未满足**——只有"新会话 + 独立干净工作树 + 独立复跑"成立。若主人要求跨 CLI 独立测试，请在 integration 前裁决。
- **结论：PASS** —— 0 条可归因于本候选的缺陷；开发节点声称的七套件与三门结果全部独立复现；追加确定性复跑（同套件连跑两次）结果一致。非阻塞发现 5 条见 §6。

## 1. 环境与前置

| 项 | 实测 |
|---|---|
| 解释器 | `py -3.13` = Python 3.13.15（`python` 为过时 Anaconda，未使用） |
| DB | MySQL `5.7.3-m13-log`，库 `mydb`，`schema_version` 最大 = `2026100103`（无本需求相关迁移，符合 I4） |
| 构建 | 本工作树 `pipeline/build.ps1` 现构，`dist/Server.exe` 456704 B（2026-10-01 16:57）；`Server/connect.config` 由 build.ps1 自 main worktree 同步并 sha256 校验 |
| 运行形态 | 统一 e2e 运行形态（`E2E-RUNFORM.md`）：cwd=`<repo>/dist`、exe=`dist/Server.exe`、`filesPath=./filesys` 相对 |
| 未入库运行依赖 | 自 main worktree 的 `dist/` 复制 Qt5*.dll、libmysql.dll、platforms/、sqldrivers/、bearer/ 等（仅复制，未改动） |
| 串行 | 七套件逐个独占 127.0.0.1:5000；本轮无并发执行 |

## 2. Pipeline（本工作树自跑，不采信开发记录）

| 门 | 结果 | 原始输出 |
|---|---|---|
| `build.ps1` | **GREEN** hash=`520c714` | `build-520c714-20261001-165632.log`，Client/Server 均 OK |
| `test.ps1` | GREEN-WARN：`0 automated tests registered` | `test-520c714-20261001-165735.log`（仓库口径如实：回归由 e2e harness 承担，非本门） |
| `audit.ps1` | **RED**：`FROZEN-PATH HIT: Client/protocol.h` + `Server/protocol.h`；`impact-check: NOT_APPLICABLE` | `pipeline-audit.log`（exit 1） |

> audit RED = **预期冻结面命中**（本任务即冻结面变更，已获主人批准），非缺陷。冻结面解析命中**两个**文件（`Server/protocol.h` 未被 ACP936 解析缺陷吞掉）。

## 3. 独立静态核验（FACT）

| 核验项 | 方法 | 结果 |
|---|---|---|
| 冻结面规模 | `git diff --numstat master...520c714 -- Client/protocol.h Server/protocol.h` | `11 0` + `11 0` = **22 insertions / 0 deletions** |
| 冻结面删除行 | `git diff master...520c714 -- ... \| grep -E '^-[^-]'` | **空**（append-only） |
| 内容一致性 | 提取两端 `+` 行与 `design.md` §9(a)/(b) 批准稿逐行比对 | **逐字一致**，两端新增块互相同文；见 `frozen-content.log` |
| 落位正确性 | `git diff -U3` 上下文 | 107/108 紧接 106 之后、`ENUM_MSG_TYPE_MAX` 之前；常量块在末尾 `#endif` 之前 |
| 冻结面外改动 | 全量 `git diff --stat master...520c714` | 33 文件 / +3128 −778，生产侧仅本需求允许面（Client/transfertask.*、Server msghandler/mytcpsocket 等）；冻结面文件**只有**上述 22 行 |

**判定：符合 §9 批准稿（只追加、两端逐字一致、无批准外改动）。integration 事实线 #2 的前置成立。**

## 4. 七套 e2e 独立复跑（全绿，与开发声称一致）

| 套件 | 基线 | 本轮实测 | 日志 |
|---|---|---|---|
| entry_ops | 43 | **43/43** | `entry_ops.log` |
| dedup | 13 | **13/13** | `dedup.log` |
| cross_tree | 41 | **41/41** | `cross_tree.log` |
| sec001 | 48 | **48/48 PASS** | `sec001.log` |
| fastupload | 46 | **46/46** | `fastupload.log` |
| groupfile | 45 | **45/45** | `groupfile.log` |
| parallel_e2e（新） | 43 | **43/43** | `parallel_e2e.log` |

六套既有套件以 id=0 帧驱动服务端全流程 → 即不变式 **I1/I2/I3（旧客户端零改动、旧语义零改动）** 的独立证明（非采信开发结论）。

### 4.1 追加：确定性复跑（本节点新增，非开发要求）

`parallel_e2e.py` 在**同一候选、同一工作树**连跑**两次**（`parallel_e2e.log` 17:00:24→17:01:59；`parallel_e2e-rerun.log` 17:03:23→17:04:58）：

- 两次均 **43/43 passed，rc=0，0 条 FAIL**。
- 含随机/时序性质的用例两次结论一致：`T-06a` 两次 `order=[212, 213, 211]`（小文件 43 均早于大文件，公平性稳定）；`T-08` 两次 `n=6`，PONG 最大往返 **0.007s / 0.001s**（阈值 2.0s，余量三个数量级）。

**判定：本套件在本环境无观测到的抖动（FACT，两次观测）。**

## 5. 对开发节点 3 条实现期裁定的独立复核

开发报告 `evidence/REQ-EXT-TRANSFER-001/development/report.md` §3 列出 3 条裁定，逐条复核：

| # | 裁定 | 独立复核 |
|---|---|---|
| 1 | 个人上传握手 = 97 → 29 → 31/32（97 建暂存会话，29 以 `m_pendingFastcheck` 采纳，不重复建会话、保留摘要对照） | **成立（FACT）**。代码：`Server/msghandler.cpp:1174-1206`（`adopt` 判定同 parent/display/size → 复用 `sessionId`/`expectedSha`，`m_pendingFastcheck.active=false` 一次性失效）；客户端 `Client/file.cpp:104-127` 保留 1MiB 流式 SHA-256 且只发 FASTCHECK，`transfertask.cpp` 走 29。`fastupload` 套件 46/46 与 `parallel_e2e` T-01/T-03 同时绿，两端行为一致。 |
| 2 | T-06 流数 = 1 大 + 2 小（3 条），非 design 写的"8MB×1 + 64KB×3"（4 条） | **属实且可复现（FACT）**；但与已批准 design 文本冲突 → 见 §6 F-1（非产品缺陷，属设计文档/测试矩阵自相矛盾，需 design owner 处置）。 |
| 3 | 97 命中路径（秒传）在并行下仍零正文完成；未命中才建槽 | **成立（FACT）**：`parallel_e2e` T-03（硬上限）/T-15（id=0 锚点）与 `fastupload` 46/46 同绿；`T-12` 未知 id 拒绝且连接存活。 |

## 6. 非阻塞发现（claim vs fact；不阻断 integration）

**F-1（design 文档内部矛盾，需 design owner 处置）** — design §6.2 `T-06` 写"8MB×1 + 64KB×3 同时下载"共 **4 条**流，与主人 2026-10-01 裁定的 `D6`（下载上限 **3**）直接冲突：第 4 条必被 41 拒绝，该用例按原文**不可能通过**。开发把套件改为 1 大（512KB）+ 2 小（1KB×2）**3 条**，断言同一性质（小文件 43 早于大文件 43）。**测试节点不修改验收条件**：本节点只是复现并把矛盾记录在案，请 design owner 选择"改 design §6.2 文本"或"改 D6 上限"。**注意**：本条目**不是**测试节点放宽验收，也不改变 D6 与 `MAX_PARALLEL_DOWNLOADS=3` 的批准事实。

**F-2（claim 弱于 design 文本）** — design §6.2 `T-14` 验收写"断线 → **服务端无残留上下文**"；套件实测断言为"断线后**重连**新传输正常（`okn and recs is not None`）"，是**代理断言**而非对残留上下文的直接观测（上下文随连接销毁，同连接断后即不可观测）。属性方向正确、结论可信（FACT：复跑稳定绿），但报告的强度不应被读成"已直接证明无残留"。

**F-3（自动化覆盖缺口）** — design §6.2 `T-02`（客户端队列：超限第 4 个**不发** 29；槽位释放后自动开始并成功）在 `parallel_e2e.py` 中**不存在**（该脚本为裸 socket 驱动，无法断言客户端本地队列）。服务端侧等价面由 `T-03` 覆盖（FACT：`T-03a..d` 全绿）。客户端队列因此**只有人工清单可覆盖** → 见 F-5。

**F-4（阈值来源）** — `T-08` 的 2.0s PONG 往返阈值由开发标定（脚本常量）。独立复跑的**实测**最大往返为 0.007s / 0.001s（`n=6`），余量三个数量级，故该阈值在本环境不构成"形同虚设"的宽松门（FACT，两次观测）。记录以便 audit 判断阈值是否应随环境收紧。

**F-5（人工清单缺口，建议补入，由 design owner 决定）** — 需求 §6 验收 10 条中，以下**客户端专属**项既无自动化覆盖、也未在 design §6.3 逐一列出：验收 1 的"单文件失败不影响其他 + 单独重试"、验收 3 的"并发下载同名 `(N)` 递推互不干扰"、验收 5 的"超限显示排队中、槽位释放后自动开始"、验收 9 的"断线后任务标记失败可重试"。建议并入 §6.3 一次性人工清单。**测试节点不修改验收条件，仅提出。**

### F-2..F-5 处置（repair follow-up，2026-10-01，owner 会话）

| # | 处置 | 证据 |
|---|---|---|
| F-2 | **降级措辞（design §6.2 T-14 已改）**：断言明确写为"重连后新传输正常"这一代理形式，不再声称"已直接证明无残留上下文"。直证在本形态不可得（上下文随连接销毁）。 | `design.md` §6.2 T-14 行；`parallel_e2e.py` T-14 |
| F-3 | **部分闭环（诚实标注）**：新增 **T-22（静态契约）** 断言 `pump()` 以 `MAX_PARALLEL_UPLOADS` 为闸、INIT 被拒走回队（`needFastcheck=false`，事件驱动，非 Failed）。**动态**断言（第 4 个 29 确实未发出）仍无自动化宿主——本仓 `pipeline/test.ps1` 注册 0 单测，Client 为 GUI 进程、无 headless 入口；该动态面已并入 design §6.3 第 6 项人工清单，**不计 pass**。 | `parallel_e2e.py` T-22；`design.md` §6.3-6 |
| F-4 | **留痕**：T-08 阈值 2.0s 为开发标定（脚本常量），本轮实测 `max=0.001s`（n=6），余量约 3 个数量级；阈值不随环境收紧（避免引入环境相关抖动），标定来源记于此。 | `parallel_e2e.py` T-08；`repair/report.md` §3 |
| F-5 | **已并入 design §6.3**（新增第 6–9 项）：超限排队→释放后自动开始 / 单文件失败隔离+重试 / 并发下载同名 `(N)` 递推 / 断线后可重试。 | `design.md` §6.3 |

## 7. NEEDS-HUMAN（未执行，不计为 pass）

design §6.3 的 5 条纯客户端行为本节点**未执行**（需 GUI 人工目视，AI 不可自动化）：多选 ≥3 文件 / 任务列表逐行进度·状态·失败原因且三任务并发刷新不卡 / 上传任务的暂停·继续·取消按钮（下载任务无此按钮）/ 3 个大文件 SHA 计算期间窗口可拖动切换页面（M5）/ 暂停后继续进度不回退到 0。

**这 5 条（并建议加 F-5 的 4 条）必须在 GUI 中由人确认后才能计入验收**；本轮结论 PASS **不包含**它们。执行者请按 design §6.3 原文操作，结论按 `human-verified by <owner>, <date>` 记录。

> **repair 跟进（2026-10-01）**：F-5 的 4 条已并入 `design.md` §6.3（第 6–9 项），另加 AUD-3 人工佐证第 10 项 —— 人工清单共 **10 条**，全部仍为 **NEEDS-HUMAN，不计 pass**。

## 8. 证据清单（本目录）

`test-report.md`（本文件）、`environment.txt`、`static-independent.log`、`frozen-content.log`、`readonly-confirm.log`、`pipeline-audit.log`、`build-520c714-*.log`、`test-520c714-*.log`、七套件日志 `entry_ops/dedup/cross_tree/sec001/fastupload/groupfile/parallel_e2e(.log)`、复跑 `parallel_e2e-rerun.log`。

> **repair 更正（EV-1）**：本清单原列有 `manifest-build-*` / `manifest-test-*`，本目录内**不存在**该文件。
> 证据 manifest 由 `pipeline/build.ps1` / `test.ps1` 的 `Stamp-Evidence` 写入 **`build/evidence/current/`**
> （gitignored 的传输区），不经本目录归档；此处删除该两项。原文为清单过度声明（evidence missing），
> 三门日志本身真实自洽，结论不受影响。

## 9. 交下一节点

independent audit（**新会话、不同代理、只读** detached worktree at `520c714`）：输入 = 本报告 + 七门/七套件日志 + `impact-check` 依赖闭包 + 冻结面 diff；对 §5 三条裁定与 §6 五条发现做 claim-check。**F-1 需 design owner 先给出处置方向**，audit 请勿替其放宽 D6。
