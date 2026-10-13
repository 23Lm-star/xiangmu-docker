# REQ-EXT-TRANSFER-001 repair follow-up 报告（接 119e6d3 audit 的 5 项开放项）

- 节点：repair（owner 会话；候选 `35dfce7`，基线 `119e6d3`）。分支 `feat/req-ext-transfer-001-parallel-transfer`。
- 授权：主人 2026-10-01 开 follow-up repair，逐项处置 DOC-1 / AUD-3 / AUD-4 / test F-2..F-5 / design §6.3 五条 GUI 项。
- 约束遵守：**未触碰冻结面**（`Client/protocol.h`/`Server/protocol.h` 自 `119e6d3` 零改动，累计仍 22 insertions / 0 deletions，合 §9 批准稿）；**未改 D6 与上限**（上传 3 / 下载 3，`MAX_PARALLEL_DOWNLOADS=3` 未动）。
- 独立性与限制（照实）：`owner_agent: claude-code`，本机 **Codex CLI 不可用**，跨 CLI 独立性不成立；本次为同一代理的修复会话，**不构成独立验证**，须由新会话 independent audit 复审。

## 1. 逐项处置（claim → fact + 证据）

| # | 项 | 处置（一行） | 证据 |
|---|---|---|---|
| 1 | **DOC-1**（必修文档） | `design.md` §3.1 repair 补充**重写**：删除不存在的 `LegacySlotGuard`/"返回前清槽"表述，改为 `fastcheckPlaceholder` + `activeUploadCount()` 排除；明写 **id=0 槽不删**（旧客户端 97→31 直传依赖，首版删槽已被 `fastupload 24/46` 否决）；§3.1 口径行同步为 `activeUploadCount() <= MAX_PARALLEL_UPLOADS`。 | `design.md:133-142`；`Server/msghandler.h:57-71`、`msghandler.cpp:1311-1319` |
| 2 | **AUD-3**（迟到 98 误归属） | **服务端**：98 回带 97 的请求标识（`caMsg` 尾追加 `reqId`，`nameLen` 不含它；拒绝路径尽力回带）。**客户端**：`pumpFastcheck()` 为每次 97 生成唯一标识并留在途，`onFastcheckRespond()` 无在途 → 丢弃、标识不匹配 → 丢弃；取消/暂停清在途槽（随后到达者按"无在途"丢弃）。 | `Server/msghandler.cpp` 98 构造段；`Client/transfertask.cpp` `pumpFastcheck`/`onFastcheckRespond`；`Client/file.cpp` `onFastcheckDue`；wire 锚点 `parallel_e2e.py` **T-21**（未命中/命中两路径，nameLen 语义不变） |
| 3 | **AUD-4**（107 × 在途写） | `UploadCtx.pendingAbort`：107 到达时若 `writeInFlight` → **延迟释放**（108 仍即时 ret=true），写完成回调收尾；`abortUpload()` 把暂存**对齐**到 `receivedSize()` 并落 `upload_session.confirmed_offset`。 | `Server/msghandler.h:36-40`、`msghandler.cpp:abortUpload`、`mytcpsocket.cpp:handleUploadAbort/handleUploadWriteFinished`；竞态探针 **T-20**（6 轮：confirmed_offset == 暂存物理大小；续传 30 回带 == 暂存大小） |
| 4 | **F-2** | `design.md` §6.2 T-14 断言**降级为代理措辞**（"重连后新传输正常"），不再暗示已直接证明无残留。 | `design.md` §6.2 T-14；test-report §6 处置表 |
| 5 | **F-3** | **部分闭环（诚实标注）**：新增 **T-22 静态契约**（`pump()` 闸 = `MAX_PARALLEL_UPLOADS`；INIT 被拒 → 回队 `needFastcheck=false`，非 Failed）。**动态**面（第 4 个 29 确实未发出）无本仓自动化宿主（`test.ps1` 注册 0 单测、Client 为 GUI 进程），已并入 §6.3 人工项第 6 条，**不计 pass**。 | `parallel_e2e.py` **T-22**；`design.md` §6.3-6 |
| 6 | **F-4** | T-08 阈值 2.0s 标定来源**留痕**（开发标定；实测 max 0.001s，余量约 3 个数量级；不随环境收紧以免引入抖动）。 | test-report §6 处置表；`parallel_e2e.py` T-08 |
| 7 | **F-5** | 4 条客户端专属验收并入 `design.md` **§6.3 第 6–9 项**（超限排队→释放后自动开始 / 单文件失败隔离+重试 / 并发下载同名 `(N)` 递推 / 断线后可重试），另加 AUD-3 人工佐证第 10 项。 | `design.md` §6.3（共 10 条） |
| 8 | **§6.3 五条 GUI 项** | 本轮仍**不可自动化** → 输出 **NEEDS-HUMAN 精确步骤**（见 §4），**最多一次**，不重试循环。 | 本报告 §4 |

## 2. 验证（本会话实跑，非采信）

| 门 | 结果 | 日志 |
|---|---|---|
| `pipeline/build.ps1` | **GREEN** `hash=35dfce7`（Client/Server 均 OK） | `followup/build-*.log`（`build/evidence/current/`） |
| `pipeline/test.ps1` | GREEN-WARN（`0 automated tests registered`，仓库口径如实） | 同上 |
| `pipeline/audit.ps1` | **RED = 预期冻结面命中**（`Client/protocol.h`+`Server/protocol.h`，本需求即冻结面变更且已批准）；`impact-check: NOT_APPLICABLE` | 同上 |

七套 e2e（串行独占 5000，`py -3.13`）：

| 套件 | 计数 | 套件 | 计数 |
|---|---|---|---|
| entry_ops | 43/43 | fastupload | **46/46**（AUD-1 兼容锚点，未破） |
| dedup | 13/13 | groupfile | 45/45 |
| cross_tree | 41/41 | parallel_e2e | **53/53**（原 46 + T-20×3 + T-21a/b + T-22a/b） |
| sec001 | 48/48 | | |

**新增 4 条锚点的自证力**：
- **T-20** 在改动前必失败（`confirmed_offset` 停留 INIT 值 0 ≠ 暂存大小）；修复后 6/6 轮不变量成立。属**真实回归锚点**，非形式断言。
- **T-21** 直接锁定 98 的回带契约（`msg[nameLen:]` == reqId 逐字节；命中路径 `msg[:nameLen]` 仍是落定名）。
- **T-22** 为**静态**契约（已如实标注），不冒充动态行为验证。

## 3. 未闭环 / 边界（不读作 pass）

- **AUD-3 的客户端归属路径无自动化**：T-21 只锁服务端回带契约；客户端"按标识归属/丢弃无匹配"的运行时行为无宿主可自动断言 → 已列为 §6.3 第 10 项人工佐证。
- **F-3 动态面**未闭环（同上）。
- **AUD-2-N1**（回队未区分"上限拒绝"与"重复 id/非法"）仍为历史观察，本轮未改（本轮只补 DOC/归属/竞态）。
- **本轮未做反向探针**：主人规定的"反向探针只允许在一次性 worktree 内且必须完全回滚"本轮**未使用**（T-20/T-21 的正向锚点已足够区分修与未修，无需改码取反）。故无探针残留问题。
- 未做 GUI；未跑跨 CLI。

## 4. NEEDS-HUMAN（精确步骤，一次，不重试）

前置：`Server.exe` 已按 `E2E-RUNFORM` 形态运行（cwd=`dist`，`filesPath=./filesys`），用**同一账号**登录一个已建目录 `tx1` 的客户端。10 条来自 `design.md` §6.3，逐条记录 `human-verified by <owner>, <date>`：

1. 多选选择器一次选中 **≥3** 个文件（不同大小）。
2. 任务列表逐行显示 文件名/进度/状态/失败原因；3 个任务同时刷新不卡。
3. 上传任务有 [暂停][继续][取消] 且行为/文案正确；下载任务**无**这三个按钮。
4. 选 3 个大文件（≥50MB），SHA 计算期间窗口可拖动、可切页（M5）。
5. 暂停后继续，任务行进度**不回退**（走续传，不再从 0 开始）。
6. 选 **≥4** 个文件：第 4 行显示"排队中"；完成/取消前 3 个之一后，第 4 行**自动开始并成功**。
7. 令 1 个任务失败（目标目录越权或磁盘满）：其余 2 个不受影响；失败行带原因，点[重试]后可成功。
8. 同时下载两个**同名**文件到同一目录 → 落盘为 `name` 与 `name(1)`，互不覆盖。
9. 传输中断开服务端 → 任务行标记失败并给原因；服务端恢复后[重试]可完成。
10. 任务 A 在"计算摘要…"时立即暂停，再启动任务 B 上传：B 按其自身结果推进（命中完成/未命中进入传输中），A **不**出现错误的"已完成"行。

执行者请在 GUI 内**一轮**完成；任一不可用/不可判读 → 记 NEEDS-HUMAN 并停止，不重试循环。

## 5. 交下一节点

**independent audit（新会话、只读、detached worktree @ `35dfce7`）**：复核 (a) 98 回带标识的 wire 契约与客户端归属逻辑（含"无在途/不匹配即丢弃"是否可能吞掉合法响应）；(b) AUD-4 延迟释放是否引入新竞态（107 与 32 ack 顺序、`pendingAbort` 与断线清理、空闲回收的交互）；(c) DOC-1 文本与代码是否逐条一致；(d) T-20/T-21/T-22 的自证力（可否独立重做反向）；(e) 冻结面零改动、D6 未改。**§6.3 十条人工清单与 F-3 动态面仍未闭环**，integration 三事实行暂不成立。
