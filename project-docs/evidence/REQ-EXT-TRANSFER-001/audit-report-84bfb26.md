# REQ-EXT-TRANSFER-001 独立审计报告（independent audit — repair follow-up 候选 84bfb26 / 代码面 35dfce7）

- 节点：independent audit（Development OS v1.7 / LARGE；新会话、只读、detached 工作树）。
- 对象：`84bfb26c9625fce5a880f597ec2a90beee557fe4`（分支 tip）。其**代码提交**为 `35dfce7e6447d7d2a70cf3c46f7b2c3f4d135f4f`（`fix(transfer-001): 修复 AUD-3 + AUD-4`）；`35dfce7..84bfb26` 两次提交为 **docs-only**（`git diff --stat` 全部落在 `project-docs/`，源码/协议/测试零改动），故审计在 tip 上做、源码等价于 35dfce7。
- 审计快照：`E:/qt/xiangmu/build/wt-audit-35dfce7`（`git worktree add --detach 84bfb26`；起始 `git status --porcelain` 为空）。
- 输入：followup 报告 `project-docs/evidence/REQ-EXT-TRANSFER-001/repair/followup-report.md` + 七套件日志 + 上一候选 `119e6d3` 的 audit 报告（pending）+ `119e6d3..84bfb26` 源码 diff + 冻结面 diff。未做全仓扫描。
- **结论：PASS —— 5 项开放项逐条复核通过；AUD-3/AUD-4 的修复经本会话独立重建 + 反向探针确认为真实修复；无新的候选归因阻塞缺陷。** 非阻塞发现 2 条（N-1 迟到 98 丢弃后的服务端暂存残留；N-2 T-20 的判别力仅由 T-20b 承担）。**F-3 动态面与 design §6.3 十条 GUI 人工项仍未闭环，不得读作 pass；integration 三事实行暂不成立。**

## 0. 只读边界与独立性披露（照实）

- 只读边界：未 `commit/push/merge/reset`；未改生产源码/文档/验收条件。**唯一例外且已完全回滚**：为独立重做反向探针，在**一次性 detached 审计工作树**内 `git checkout 119e6d3 -- Server/msghandler.{h,cpp} Server/mytcpsocket.cpp`、重建并跑套件，随后 `git checkout HEAD -- <同三文件>` 还原并重建（见 §4）。当前 `git status --porcelain` = 空，HEAD 仍 `84bfb26`、零提交。主工作树 `E:/qt/xiangmu` 的源码/文档**全程未触碰**。
- 运行时边界：构建/运行**只在审计工作树的 `dist/`（gitignored）**内；`dist/filesys` 为**主工作树运行数据根的只读副本**（3.0 MB 拷贝，未写回），`dist/filesys/.staging` 与 SQL 库 `mydb` 为全套件共用的既有运行态（与 repair 会话同库；非产品源码）。
- 独立性披露：任务 `owner_agent: claude-code`，development/test/repair 亦为 Claude Code 系；本机 **Codex CLI 不可用**，跨 CLI 独立性不成立。实际独立性 = 新会话 + 独立工作树 + 独立重读源码/证据 + **本会话独立重建并运行时复现（含反向探针）**，**不等于**跨工具链独立。此为残余限制，不计为满足完全独立。

## 1. 复核面 (a)：98 回带标识的 wire 契约与客户端归属 — FACT

**服务端（`Server/msghandler.cpp:1320-1401,1545-1564`）**

| 核验 | 证据 | 结果 |
|---|---|---|
| 接受路径逐字节回带 | `:1547` `mkPDU(finalName.size()+reqIdBytes.size())`；`:1550,1557` `nameLenOut = finalName.size()`；`:1558-1563` `caMsg = finalName ‖ reqIdBytes` | **成立**：`msg[:nameLen]` 仍是落定名（旧读法不受影响），reqId 追加其后 |
| 拒绝路径尽力回带 | `:1341-1359` 按 "caMsg 末尾 `rl` 字节" 取；`rl∈[8,96] 且 rl<=uiMsgLen` 才回带，否则空 | **成立**；`mkPDU` 全零（`Server/protocol.cpp:19`）→ 拒绝响应 `nameLen` 字段 = 0，客户端按"从 caMsg[0] 读整段"解析，与注释一致 |
| 97 摘要语义未变 | `:1372-1380` shape 校验含 `reqIdLen∈[8,96]`、`total==uiMsgLen`；`:1382` 重新切分 body | **成立** |
| 客户端按标识归属 | `Client/transfertask.cpp:269-295`：`m_fastcheckInFlight==0` → 丢弃；`echoed` 非空且 ≠ 在途标识 → 丢弃并**保留在途槽**；匹配 → 归属 | **成立** |
| 标识生成与留存 | `:238-240` pumpFastcheck 生成唯一 token 并同时写 `m_fastcheckInFlightToken` 与 `t->fastcheckToken`；`Client/file.cpp:141` 97 正文用该 token | **成立**（单飞：`:230` 有在途即 return，同一时刻仅一个 97） |

**"无在途/不匹配即丢弃" 会不会吞掉合法响应 —— 不成立（不会吞）**（推演 + T-21 实例）：客户端恒发 32 字节 reqId（`FASTCHECK_REQID_HEX_LEN`），服务端两条路径都回带；单飞槽只在 ① 收到匹配 98 ② `pause/cancel/failTask` 主动清槽 三种情形释放，故任何**匹配**的 98 必被接收；被丢弃的只能是主动放弃后到达的迟到响应（本就是设计意图）。

- **残余（非阻塞，记 N-1）**：客户端主动放弃后，服务端 97 已建的 `id=0` 暂存槽与 `upload_session` 行仍在（`:1323-1331` 建槽、`:1438-1440` 建会话）。`reapIdleUploads`（`:1305-1310`）要求 `lastProgressMs > 0`，而 97 纯占位从不设置该字段 → **该残留不被空闲回收**，只能等会话 3600s TTL 或下一次 97 覆盖 id=0 槽。有界（单槽 + TTL），且**非本候选引入**（119e6d3 的 AUD-1 设计即如此）；AUD-3 相比旧行为只是把"误归属"换成"多一条有界残留"。**建议修复（不阻塞）**：97 建槽后置 `ctx.lastProgressMs = now`，使其纳入既有空闲回收口径。
- **残余（claim，未证实）**：若服务端回带的 `echoed` 非空但与在途不符，且该 98 实为**当前**任务的响应（需服务端回带错误字节，本仓客户端不产生该请求形态），任务将停在 Hashing。未构造出可达路径，**记 claim 不记 fact**。

## 2. 复核面 (b)：AUD-4 延迟释放的新竞态面 — FACT（未发现新缺陷）

- **线程模型**：写盘在 `QThreadPool` 线程（`Server/uploadwritetask.cpp:19-26`），完成经 `Qt::QueuedConnection` 回到 `MyTcpServer`（`:28-34`）→ `handleUploadWriteFinished` 与 `handleUploadAbort`/`handleUploadData` **同在主线程**，`writeInFlight`/`pendingAbort` **无数据竞争**。
- **107 ↔ 32 ack 顺序**：107 到达时若在途 → `pendingAbort=true` 且**立即**回 108 `ret=true`（`mytcpsocket.cpp:397-409`）；写落定后 `handleUploadWriteFinished` 先 `acceptWrittenChunk` 再 `abortUpload` 并 **return（不再回 32）**（`:361-376`）——客户端已发 107、不再等该 32，语义自洽；且 `abortUpload` 内 `m_uploads.remove` 后代码**立即 return、不再触碰 `cp`**，无 use-after-free。
- **暂存对齐正确性**：`abortUpload`（`msghandler.cpp:1276-1290`）将暂存 `resize` 到 `session.receivedSize()` 并 `updateUploadOffset(confirmed)`。写成功 → `confirmed == offset+len == 物理大小`（幂等）；写**失败**（`QFile::write` 部分写/`flush` 失败）→ `acceptWrittenChunk` 不执行 → `confirmed == offset` < 物理大小 → **尾部未验证字节被剪掉**。这正是 AUD-4 要修的语义（`beginStaged` 按物理大小派生续传起点，`resumableuploadsession.cpp:98-109`；未剪会让续传跳过未验证字节 → 内容损坏）。
- **`pendingAbort` ↔ 空闲回收**：`reapIdleUploads` 已要求 `!writeInFlight`，等待写落定的槽不会被误回收 → **无竞态**。
- **`pendingAbort` ↔ 新 INIT 复用同 id**：客户端 `m_nextId++` 单调递增（`transfertask.cpp:41,119`），连接内不复用 id；服务端 `acquireUploadSlot` 对已存在 id 一律拒绝（`:1260-1262`）→ **无"旧槽未释放、新上传撞 id"面**。
- **`pendingAbort` ↔ 断线清理（残余，非本候选引入）**：断线走 `MyTcpSocket` 析构 → `MsgHandler` 析构，**不经过 `handleUploadAbort`** 的延迟口径；此时池线程写完成后 `findUpload` 返回 `nullptr` → 早退（`:364-366`），**不做对齐**。该路径与改动前同构（改动前亦无对齐），属**历史遗留**；下次续传由 `beginStaged` 按物理大小重派生并回写 DB，故实际风险受限于"部分写 + 恰在断线窗口"。
- **残余（claim）**：池线程写永不返回时 `pendingAbort` 槽滞留且不可回收（`writeInFlight` 为真）。此为既有 `writeInFlight` 停摆类，**非本候选引入**，无自动化可判据。

## 3. 复核面 (c)：DOC-1 文本 ↔ 代码逐条一致 — FACT（已修复，一致）

`design.md:136-144` 与代码逐条对齐：

| 文档表述 | 代码 | 一致 |
|---|---|---|
| 排除 `fastcheckPlaceholder` 后的条目数 `<= MAX_PARALLEL_UPLOADS` | `msghandler.h:61-70` `activeUploadCount()` 逐条 `if(!fastcheckPlaceholder) ++n` | ✓ |
| **id=0 槽不删**（旧客户端 97→31 直传依赖） | `msghandler.cpp:1323` 建槽后不 remove；`:1422+` 继续用 `ctx` | ✓ |
| `LegacySlotGuard` "返回前清槽"**不存在于代码、不得按此实现** | 全仓 `grep LegacySlotGuard` 仅 `design.md:140` 一处，且为**否决说明** | ✓（旧 DOC-1 的误导性文本已消除） |
| 占位失效路径 ① 收到正文 → `fastcheckPlaceholder=false` 并重新计名额 | `mytcpsocket.cpp:384-386` | ✓ |
| 占位失效路径 ② 随后的 29 采纳 → `uploadfileinit` 移除 id=0 槽 | `msghandler.cpp:1211-1216`（`adoptedFastcheck`） | ✓ |

§3.1 口径行（`:134`）已与 §3.1 补充、§6.2、§9 一致；未见旧式 `m_uploads.size() <= MAX_PARALLEL_UPLOADS` 残留。**F-2**（T-14 代理措辞，`design.md:321`）、**F-5/§6.3 十条**（`:§6.3` 逐条 1..10 齐备）文本均按报告落位。

## 4. 复核面 (d)：T-20/T-21/T-22 的自证力（独立反向重做） — FACT

**基线（本会话独立重建 84bfb26 / Server.exe 于审计工作树）**：`parallel_e2e.py` = **53/53 PASS**（含 T-20a/b/c、T-21a/b、T-22a/b、T-19a/b/c）；另独立复跑 `fastupload_e2e.py` = **46/46 PASS**（覆盖被改动的 97/98 处理与 legacy 97→31 直传契约未破）。与 followup 报告 §2 计数一致。

**反向探针（本会话独立重做，一次性、已完全回滚）**：仅回退本次修复的三处服务端文件到 `119e6d3` → 重建 → 复跑同一套件：

- **T-20b** `:: [False, False, False, False, False, False]`（**6/6 确定性失败**）→ T-20 是**真实回归锚点**（关修复即失败），与 followup "改动前必失败（confirmed_offset 停留 INIT 值）" 逐字相符。
- **T-21a** `:: [False, False]`、**T-21b** FAIL → T-21 是**真实契约锚点**。
- 总 `50/53`（仅上述 3 条失败；T-19/T-20a/T-20c 及全部下载面不变）→ 回退**未产生**其他行为差异，反向探针干净。
- **回滚自查**：`git checkout HEAD -- <三文件>` → `git status --porcelain` 空；重建 GREEN；HEAD 仍 `84bfb26`、零提交。**探针零残留**。

**精度提示（记 N-2，非阻塞）**：T-20 的**判别力全部由 T-20b 承担**（T-20a 只断言 108 `ret=true`，T-20c 只断言 30 回带 == 物理大小——二者在**未修复**代码下同样 PASS，见反向日志）。T-20c 不是 AUD-4 的判别项，勿据此宣称覆盖。
**T-22 定位照实**：T-22a/b 是对 `Client/transfertask.cpp` 的**正则/子串静态断言**（文本存在性），**不构成运行时行为证据**；其真值随源码文本变化，反向"重做"只需删改源码文本即失败，无独立判别价值。报告已如实标注为"静态"，**动态面（第 4 个 29 确实未发出）仍归 §6.3 第 6 项人工**——本审计认同该标注，不提高其证据等级。

**复现环境前置（供后续复现者，重要）**：审计工作树的 `dist/filesys` **必须预置**（本会话第一次运行套件得 **46/53**，7 条失败全在下载面：T-01c/T-06a/T-06b/T-07/T-15/T-18c/T-18d）。原因**非代码缺陷**：`file_object` 在**共享 SQL 库**中已有历史行（如 `objects/f9/377`，refcount=17），上传按 sha 去重在**新工作树的空 filesys** 上直接命中既有行、**不落物理文件** → 下载取不到字节。将主工作树 `dist/filesys` 拷入后即 **53/53**（两次日志均已在证据目录留存：`...-attempt1-freshfilesys.log` / `...-53of53.log`）。上一轮 audit 报告 §4 的"需预置 `dist/filesys/<user>`"应扩写为"**需预置整个 filesys 对象树**"。

## 5. 复核面 (e)：冻结面零改动 / D6 未改 — FACT

- `git diff 119e6d3..84bfb26 -- Client/protocol.h Server/protocol.h` = **空**（本候选对冻结面零改动）✓
- `git diff master..84bfb26 -- Client/protocol.h Server/protocol.h` = **22 insertions(+), 0 deletions(-)**（11+11），与 §9 批准稿逐字一致；仅追加 107/108 枚举与 transfer_id 常量块，旧编号 1–106 未重编号 ✓
- D6：`Server/protocol.h:289-290` 与 `Client/protocol.h:279-280` 均为 `MAX_PARALLEL_UPLOADS = 3` / `MAX_PARALLEL_DOWNLOADS = 3`，两端一致、未改 ✓
- `pipeline/audit.ps1`（本会话在工作树内实跑）：`FROZEN-PATH HIT: Client/protocol.h`、`Server/protocol.h` → **RED = 预期**（本需求即冻结面变更且已批准）；`impact-check: NOT_APPLICABLE`（0 注册单测，回归由 e2e 承载）✓

## 6. 本会话机械门（工作树内实跑，非采信）

| 门 | 结果 | 备注 |
|---|---|---|
| `pipeline/build.ps1` | **GREEN** `hash=84bfb26`（Server 构建 OK；config 从主工作树 SHA256 校验同步） | 审计工作树内独立重建 |
| `pipeline/test.ps1` | **GREEN-WARN**：`0 automated tests registered` | 仓库口径如实；不构成任何 GREEN 主张 |
| `pipeline/audit.ps1` | **RED = 预期冻结面命中**；另报 `NO test evidence for 35dfce7` | 后者为**空工作树**的机械副作用（test.ps1 证据写在 84bfb26 名下、audit 按 docs-only 解析到 35dfce7），**非候选缺陷**；owner 工作树内已有 35dfce7 的 build/test 日志 |

## 7. 不得读作 pass 的残余（明写）

- **F-3 动态面**（客户端第 4 个 29 确实未发出）：本会话**未闭环**，无宿主可自动断言 → 归 design §6.3 第 6 项人工。
- **design §6.3 十条 GUI 人工项**（含 AUD-3 人工佐证第 10 项）：本会话**未执行**（需 GUI 人眼），**不计 pass**。
- **AUD-2-N1**（客户端回队未区分"上限拒绝"与"重复 id/非法"）：本轮**未改**，仍为历史观察。
- **AUD-1 时代残余**：N-1（97 纯占位不被空闲回收）与断线路径不对齐暂存——均**非本候选引入**，见 §1/§2。
- 本会话**未运行** entry_ops/dedup/cross_tree/sec001/groupfile 五套件（取 repair 日志为输入）；未做 GUI、未跑跨 CLI。

## 结论

**PASS**：复核面 (a)(b)(c)(d)(e) 逐条通过。AUD-3 的 98 回带标识契约（接受/拒绝两路径）与客户端按标识归属逻辑经**源码直读 + 独立重建运行时复现（基线 53/53、反向探针 T-21a/b 必失败）**确认为真实修复；AUD-4 的延迟释放 + 暂存对齐经**反向探针 T-20b 6/6 必失败**确认为真实修复，且新引入的竞态面（线程归属、107↔32、空闲回收、id 复用、断线）**逐面排查未发现新缺陷**；DOC-1 已重写并与代码逐条一致；冻结面自 119e6d3 零改动、D6 未改。**无阻塞项**。非阻塞：N-1（迟到 98 丢弃后的有界服务端残留，建议置 `lastProgressMs`）、N-2（T-20 判别力仅由 T-20b 承担；T-22 为静态文本断言，不构成行为证据）。**F-3 动态面与 §6.3 十条人工项仍开放，integration 三事实行暂不成立。**

## cleanup_status

- 审计工作树 `build/wt-audit-35dfce7`（detached @ `84bfb26`）：**产品源码零提交**；反向探针改动已 `git checkout HEAD --` 完全回滚，`git status --porcelain` 空、HEAD 未变（见 §4）。工作树按协议在报告落盘后删除。
- 未触碰主工作树 `E:/qt/xiangmu` 的源码/文档；运行时仅在审计工作树 `dist/`（gitignored）内构建/跑服务端，`dist/filesys` 为主工作树的**只读拷贝**，占用过 5000 端口（套件自 `taskkill` 释放）；SQL 库 `mydb` 中仅新增本套件测试行（与 repair 会话同库同口径）。
- 本报告唯一写盘：`E:/qt/xiangmu/build/evidence/claude-req-ext-transfer-001-audit/84bfb26c9625fce5a880f597ec2a90beee557fe4/audit-report.md` + 同目录 4 份套件日志。
