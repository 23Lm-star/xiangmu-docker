# REQ-EXT-TRANSFER-001 多文件并行传输（有限并发）— 任务记录

## 档位
- **LARGE**（goal 定档 2026-10-01）。理由（风险面，非行数）：
  1. **冻结面命中**：需向 `Client/protocol.h` 与 `Server/protocol.h` 追加 transfer_id 字段（按 PDU 落位）与拟新增 ABORT PDU（manifest `frozen_paths`）→ 按 v1.7 规则自动升 LARGE，改动须主人批准。
  2. **公共契约变更**：上传/下载全族 PDU（29/30/31/32、40/41/42/43、101/102、105/106）扩展 + 拟追加 107/108；`0` 值保留 = 旧单流路径（旧编号语义零改动）。
  3. **跨会话**：客户端（单例 → per-transfer 多实例、UI 任务列表、暂停/取消）+ 服务端（单会话 → QHash 状态表、per-transfer 写盘在途位、下载 round-robin 调度、空闲回收），单会话放不下。
  4. **热路径**：上传/下载状态机核心改造，回归覆盖全部六套 e2e 套件（cross_tree/entry_ops/dedup/sec001/fastupload/groupfile）。
- 需求文档 `project-docs/requirements/REQ-EXT-TRANSFER-001-multi-file-parallel-transfer.md` 自声明 LARGE；本记录与之一致，不再改档。
- 流程版本：Development OS v1.7。分支：`feat/req-ext-transfer-001-parallel-transfer`，基线 `master@53b048b`。
- 节点计划：goal（本记录）→ design（逐字节协议设计 + 状态机 + 调度算法 + 测试矩阵 → `design.md`）→ **主人批准契约变更** → development → independent test（新会话）→ repair（按需）→ independent audit（新会话、只读）→ integration（三事实行）→ merge no-ff。
- `owner_agent: claude-code`。

## 依赖核查（2026-10-01 实测，非文档转述）
| 依赖 | 状态 |
|---|---|
| REQ-EXT-SEC-001 | ✅ merged `7fc47c1`；连接级 actor 门（`m_actorName`、`pathInActorRoot`） |
| REQ-EXT-GROUP-001 | ✅ merged `ee75441`；群成员资格（101 判定复用面） |
| REQ-EXT-GROUP-002 | ✅ merged `ffaaacf`；群消息 PDU 88–96 |
| REQ-EXT-FILE-001 | ✅ merged `9faae6c`；对象仓 + 五表 + `upload_session` CRUD + 逻辑键 |
| REQ-EXT-FILE-002 | ✅ merged `abe2e5d`；秒传 97/98（本需求保持单飞不改造） |
| REQ-EXT-GROUP-003 | ✅ merged `495e552`；群文件 PDU 99–106 + `group_file` 表（群上传/下载 + 续传 `nextOffset` 现成） |
| 并行冲突面 | 无活动开发区；master@53b048b 为唯一基线 |
| 回归运行形态 | 已统一（`0d92607` + `53b048b`）；六套件按 E2E-RUNFORM 规格跑 |

**复用盘点（代码事实）** — 并行化的关键既有设施：
- 上传续传：个人 29/30 `resumeOffset`、群 101/102 `nextOffset`（`resumeEnabled=true`）**均已具备** → 暂停/恢复走既有续传路径，零新协议（goal 取证）。
- 背压：入站 `MAX_INBOUND_PDU_SIZE=16KB` + `MAX_PENDING_INBOUND_BYTES=256KB`；出站 256KB 高水位节流（Server/protocol.h:193-194）。
- 清扫定时器模式：`MyTcpServer` 僵尸连接清扫（空闲回收可参照）。
- 上传 DATA 已有 offset 连续性/长度边界/magic 校验（并行后逐 transfer 化，语义不变）。

## 用户决策（2026-10-01，AskUserQuestion 三项确认）
1. 传输路径 = **全路径并行**：个人上传、个人下载、群上传、群下载（下载数据通道 42/43 本就共用，统一加编号优于分路径特判）。
2. 单连接并发上限 = **3**（常量，可调；超限排队不报错）。
3. UI = 多选选择器 + 文件级任务列表 + **每个上传任务可手动暂停/恢复/取消**（用户追加，扩展原范围）。
- 明确非目标：单文件乱序分片、动态分片、任务持久化、跨连接并行、限速/配额、下载任务暂停/取消（下载仅多选并行）。

## 契约影响（须主人批准；批准卡随 design.md 提交）
| 面 | 变更 | 性质 |
|---|---|---|
| `Client/protocol.h` + `Server/protocol.h`（frozen） | 29/30/31/32、40/41/42/43、101/102、105/106 追加 transfer_id（u64，落位草案见需求文档 §8.2）；拟新增 `ENUM_MSG_TYPE_UPLOADFILE_ABORT_REQUEST/RESPOND=107/108`（形态 design 裁定） | 只追加不重编号；`0` 值保留 = 旧单流路径 |
| 既有 PDU 编号 / 分块协议语义 | **零改动**（id=0 旧路径隔离；97/98 不加 id） | 旧客户端与全部既有 e2e 脚本零改动通过 |
| DB schema | **零变更**、无迁移 | 附加值 |

## 门次与事件（append-only）
- 2026-10-01 design：`design.md` 落盘（逐字节落位表 / 107-108 形态 / 两端状态机 / ring 调度 / 60s 回收 / 测试矩阵 18 例 / 兼容不变式 I1-I6 / 冻结面逐字批准稿 §9）。8 项设计裁决 D1-D8 记录在案。契约变更批准卡以业务语言提交，**主人批准：同意**；并发名额 Q1 裁决为**上传 3 + 下载 3（相互独立）**，已回写 `design_status: CONFIRMED`。
- 2026-10-01 goal：定档 LARGE。方案 B（协议级多路复用）裁决 = **采用，带 6 处修正**（M1 transfer_id 覆盖初始化握手 = 硬缺陷修正；M2 0 值保留 = 免费回归护栏；M3 服务端 per-transfer 状态 + 上限；M4 下载 round-robin 公平调度；M5 SHA-256 移出 UI 线程；M6 文件级任务列表 UI）。排除：多连接并行（与单登录版本模型冲突）、纯客户端串行队列（不产生并行）。需求文档落盘，分支自 `master@53b048b` 切出。契约变更批准卡待 design 节点提交。
- 2026-10-01 development：T1–T8 落盘，候选 `520c714`。两端 `protocol.h` 按 §9 逐字追加（各 +11 行、零删除）；`MsgHandler::UploadCtx` 表 + `MyTcpSocket` 下载环；客户端 `TransferManager`/`TransferTaskPanel`（新增 `transfertask.{h,cpp}`）；六套既有 e2e 全绿（43/13/41/48/46/45）+ 新套件 `parallel_e2e.py` **43/43**；`build GREEN`、`test GREEN-WARN(0 注册单测)`、`audit RED = 预期冻结面命中`。实现期裁定 3 条见 `evidence/REQ-EXT-TRANSFER-001/development/report.md` §3（97→29 握手调和 P4/P5；T-06 流数 3 vs design 写 4 与 D6 冲突；97 命中仍零正文）。

- 2026-10-01 independent test：新会话、detached 干净工作树 `build/wt-test-transfer001`（候选 `520c714`）、只读复跑。**结论 PASS，0 条候选归因缺陷**。三门独立复现：build GREEN、test GREEN-WARN(0 注册)、audit RED = 预期冻结面命中（22 insertions / 0 deletions，与 §9 批准稿逐字一致）；七套 e2e 独立复跑全绿 43/13/41/48/46/45/43；追加确定性复跑 `parallel_e2e` 两次均 43/43。非阻塞发现 F-1..F-5（F-1：design §6.2 T-06 写 4 条流 vs 批准 D6 上限 3，属设计文档自相矛盾，需 design owner 处置，测试节点不放宽 D6）；NEEDS-HUMAN：design §6.3 五条纯客户端行为未执行，不计 pass。跨代理独立性未满足（test 与 dev 同为 claude-code），测试报告已显式披露。证据 `project-docs/evidence/REQ-EXT-TRANSFER-001/independent-test/`。

- 2026-10-01 independent audit：新会话 + **不同模型**（Codex CLI 本机不可用，跨 CLI 未执行，已披露残余限制）、detached 只读工作树 `build/wt-audit-transfer001` @ `520c714`。**结论 FAIL —— 1 条阻塞缺陷 AUD-1**，推翻 test 节点 PASS。AUD-1（FACT，源码级推演并经节点复核）：真实客户端每个新个人上传先发 97，`uploadfilefastcheck` 在 `m_uploads[0]` 留下**不回收**的上下文并被 `activeUploadCount()` 计入，致并发新个人上传**只能 2 个**，第 3 个 INIT 被拒且客户端标记**失败**（违背批准 D6 上传=3 与验收 5）；test 裸 socket 绕过 97 故不可见。非阻塞 AUD-2（上限拒绝应回队而非失败）/AUD-3（迟到 98 误归属）/AUD-4（107 与在途写并发）；F-1 归类为 design 文档自相矛盾（需 design owner 处置，audit 不放宽 D6）；EV-1 证据缺口（test 报告 §8 所列 `manifest-*` 不存在）。冻结面 22 ins/0 del、逐字节合 §9，路径边界与七套件证据复核自洽。报告 `build/evidence/claude-req-ext-transfer-001-audit/520c714.../audit-report.md`。

- 2026-10-01 仲裁（AskUserQuestion 两项）：audit FAIL 与 test PASS 冲突 → 主人裁决**接受审计结论、进入修复**；**F-1 处置 = 修订 `design.md` §6.2 T-06 文本为 3 条流**（不改已批准的 D6）。两项裁决已记录；修复须在 owner **新会话**执行（audit 会话只读，不得改生产源码/验收条件）。

- 2026-10-01 repair（owner，授权范围 = 上述 6 项）：候选 `119e6d3`。**AUD-1 首版修复（`uploadfilefastcheck` 返回前删 id=0 槽，RAII 守卫）被"六套件零脚本改动"否决**——`fastupload` **24/46**：旧客户端（FILE-002 形态）未命中后**不发 29、直接发 31 正文**，正是靠该槽接收（`fastupload_e2e.py:133-145`）。定稿修复改为"**占位参与兼容、不参与名额**"：`UploadCtx.fastcheckPlaceholder` + `activeUploadCount()` 排除占位 + 29 采纳后释放 id=0 槽（`adoptedFastcheck`，remove 置于所有 `ctx` 引用之后）+ 收到该槽正文时占位转正（`handleUploadWriteFinished`）。AUD-2：`onUploadInitRespond` 的 `ret=false` 改为回队（事件驱动重试，`needFastcheck=false` 直走 29），仅在"无在途上传可释放名额"时收口失败（design §3.3 的实现级补充）。F-1：design §6.2 T-06 修订为 3 条流（**D6 未改**）+ §3.1 补 AUD-1 适用条件。新增 **T-19**（`parallel_e2e.py`，逐字节复现 97→98→29→30→31/32；43→46）。EV-1：test 报告 §8 删除不存在的 `manifest-*` 并加注。**反向验证**：关闭占位排除后 T-19b 精确复现 `[True,True,False]`（探针已零残留回滚）。三门：build GREEN / test GREEN-WARN(0 注册) / audit RED=预期冻结面命中（修复未触碰 `protocol.h`）。七套件 **43/13/41/48/46/45/46 全绿**。报告 `project-docs/evidence/REQ-EXT-TRANSFER-001/repair/report.md`。**AUD-3/AUD-4（非阻塞）、test F-2..F-5、design §6.3 人工清单 5 条仍未闭环，不计 pass。**

- 2026-10-01 repair follow-up（owner，授权 = 5 项开放项）：候选 `35dfce7`（基线 `119e6d3`）。**DOC-1**：`design.md` §3.1 repair 补充重写——删去不存在的 `LegacySlotGuard`/返回前清槽（被 `fastupload 24/46` 否决的首版），改为 `fastcheckPlaceholder` + `activeUploadCount()` 排除，明写 id=0 槽不删；§3.1 口径行同步。**AUD-3**：服务端 98 **回带 97 请求标识**（`caMsg` 尾追加 `reqId`，nameLen 不含，拒绝路径尽力回带）；客户端 `pumpFastcheck()` 派发唯一标识、`onFastcheckRespond()` 无在途即丢弃 / 标识不匹配即丢弃（取消·暂停清在途槽）。**AUD-4**：`UploadCtx.pendingAbort` ——107 遇 `writeInFlight` **延迟释放**（108 即时 ret=true），写完成回调收尾；`abortUpload()` 暂存**对齐** `receivedSize()` 并落 `confirmed_offset`。**F-2**：design §6.2 T-14 断言降级为代理措辞。**F-3**：新增 **T-22 静态契约**；动态面（第 4 个 29 未发出）无自动化宿主（test.ps1 注册 0 单测、Client 为 GUI 进程）→ 并入 §6.3 人工项，**未闭环不计 pass**。**F-4**：T-08 阈值标定来源留痕。**F-5**：4 条并入 `design.md` §6.3（第 6–9 项），另加 AUD-3 人工佐证第 10 项（共 10 条 NEEDS-HUMAN，步骤见 followup-report §4，一次不重试）。新增锚点 **T-20a/b/c**（107×在途写竞态：confirmed_offset == 暂存大小，6/6 轮）、**T-21a/b**（98 回带标识逐字节 + nameLen 语义不变）、**T-22a/b**（静态）。**未用反向探针**（正向锚点已足以区分修与未修，故无 worktree/回滚事项）。三门：build GREEN(35dfce7) / test GREEN-WARN(0 注册) / audit RED=预期冻结面命中；七套件 **43/13/41/48/46/45/53 全绿**（fastupload 46/46 = AUD-1 兼容未破）。冻结面自 `119e6d3` **零改动**（累计 22 ins/0 del 合 §9），D6/上限未改。报告 `project-docs/evidence/REQ-EXT-TRANSFER-001/repair/followup-report.md`。**owner_agent: claude-code；本机 Codex CLI 不可用 → 跨 CLI 独立性不成立，本节点为同一代理修复会话，不构成独立验证。**

- 2026-10-01 independent audit（follow-up 复审，候选 `84bfb26` / 代码面 `35dfce7`）：新会话、detached 只读工作树 `build/wt-audit-35dfce7`；`35dfce7..84bfb26` 两次提交为 docs-only（`git diff --stat` 全落 `project-docs/`），审计在 tip 上做、源码等价 35dfce7。**结论 PASS —— 5 项开放项逐条复核通过，无阻塞项。** AUD-3（98 回带标识两路径 + 客户端按标识归属）与 AUD-4（延迟释放 + 暂存对齐）经**源码直读 + 独立重建运行时复现 + 反向探针**确认为真实修复：基线 `parallel_e2e` 53/53、`fastupload` 46/46；一次性回退三处服务端文件到 `119e6d3` 重建后 T-20b 6/6 FAIL、T-21a/b FAIL、总 50/53，探针**已完全回滚**（`git status` 空、HEAD 未变、零提交）。DOC-1 已重写且与代码逐条一致；冻结面自 `119e6d3` 零改动、D6 未改。非阻塞发现 **N-1**（迟到 98 丢弃后服务端 id=0 占位残留不被空闲回收；建议 97 建槽置 `lastProgressMs`）与 **N-2**（T-20 判别力全由 T-20b 承担；T-22 仅静态文本断言）。**F-3 动态面与 design §6.3 十条 GUI 人工项仍未闭环**，integration 三事实行暂不成立。独立性披露：`owner_agent: claude-code`，Codex CLI 本机不可用 → 跨 CLI 不成立；实际独立性 = 新会话 + 独立工作树 + 独立重读 + 独立重建复现（含反向探针），**不等于**跨工具链独立。报告 `build/evidence/claude-req-ext-transfer-001-audit/84bfb26.../audit-report.md`（已归档 `project-docs/evidence/REQ-EXT-TRANSFER-001/audit-report-84bfb26.md`）。

- 2026-10-01 repair（N-1 处置；owner，授权 = 主人指示"读取最新审计结果并对仍存在的缺陷修正与测试"）：候选 `be31161`（基线 `84bfb26`）。**N-1 已修**：`uploadfilefastcheck` 建槽即写 `ctx.lastProgressMs`，使 97 纯占位纳入 D5 空闲回收（此前恒 0 → 永不回收）；连带在 `abortUpload` 释放时按 `sessionId` 精确匹配清 `m_pendingFastcheck.active`，避免占位被回收后迟到的 29 采纳已 `expired` 的会话行。新增回归锚点 **T-23**（97 占位静默 >60s：`status` 由 `uploading` → `expired`；判别力真实、复用 T-13 静默窗口零额外耗时），`parallel_e2e` 53→**54**。**N-2 不改**（T-20a/c 在未修复代码下同真属设计使然——T-20c 正是 AUD-4 缺陷的"症状"；不改验收条件，如实保留审计标注）。三门：build **GREEN(be31161)** / test **GREEN-WARN(0 注册)** / audit **RED=预期冻结面命中**；套件 **parallel_e2e 5 次中 3 次 54/54、2 次服务端静默终止；fastupload 46/46**；冻结面自 `84bfb26` **零改动**（累计 22 ins/0 del 合 §9），D6 未改。**⚠ 阻塞级未决观察（OPEN，不得读作 pass）**：`parallel_e2e` 2/5 次出现服务端**静默进程终止**（`ConnectionResetError` + `Server.exe` 消失），崩溃点不同（首次在 T-18 `up_init`；第二次在 **T-13 的 82s 静默窗口之后**，即 `abortUpload(TRANSFER_ID_LEGACY)` **首次可被空闲回收触发的窗口**——该路径在修复前因占位 `lastProgressMs` 恒 0 而事实上从不执行）。**主要假设（未证实）**：本候选使该路径可达并触发崩溃；但首次 T-18 崩溃与该假设不符，**原因未定**。服务端为 `-subsystem:windows` 无控制台，重定向 stderr 得 0 字节，**崩溃现场不可采**。复审须先"回退 `msghandler.cpp` 到 `84bfb26` 重建 + 重复运行"判定归因；若确认本候选引入，须回退 N-1 并改更保守形态重做。报告 `project-docs/evidence/REQ-EXT-TRANSFER-001/repair/n1-fix-report.md`（§3 详载）。**owner_agent: claude-code；同一代理修复会话，不构成独立验证。**

- 2026-10-01 arbitration（N-1 崩溃观察归因；owner 会话，梦梦/WorkBuddy 代理，主人授权收口并首先验证主人的"端口占用"假设）：**裁定 = 候选 `be31161` 未被牵连，N-1 维持；静默终止 = 服务端被"干净退出"而非崩溃——机制为 QApplication `quitOnLastWindowClosed` × 唯一可见窗口（Server 主窗体）在主人并行人工操作中被关闭（点 X / 软 taskkill 的 WM_CLOSE）→ 事件循环正常退出。** 与并行 repair-attribution 会话（turn-cap 关闭，交接 `handoffs/req-ext-transfer-001-repair-attribution-2026-10-01.txt`）的 gdb 实证（`break exit` 命中 `QEventLoop::exit(int)`）及本会话"零 WER 记录"互证。6 条独立证据（全链见 `repair/n1-arbitration-report.md`）：①事件日志 0 条 Server.exe WER 崩溃记录（覆盖崩溃窗口）；②run 1 崩于 T-18，其前不存在 60s 空闲态，N-1 回收路径不可能触发；③静态：两 hunk 平凡写，新路径值语义且有 `cp==nullptr`/`!staging.isEmpty()`/`writeInFlight` 三重防护；④受控 A/B（一次性 worktree 独立构建 + PID 轮询）：base 53/53、cand 54/54 全绿；⑤时间压缩定向探针（诊断 worktree 内 `UPLOAD_IDLE_REAP_SEC` 60→3，非冻结面）：精确复现嫌疑条件 10/10 轮存活；⑥并行会话 gdb 事实 + 主人自述并行操作。**诊断 worktree 已整体删除，分支代码零改动、零探针残留。** 遗留建议（合并后另开需求）：`Server/main.cpp` 加 `a.setQuitOnLastWindowClosed(false)`。限制（照实）：owner 会话仲裁非跨 CLI 独立；全量 A/B 各仅 1 轮（主人 safe-delete 批量删除钩子 >50 文件与套件 `.staging` 清理结构性冲突，后续轮次被拦）。交接 `handoffs/req-ext-transfer-001-n1-arbitration-2026-10-01.txt`。

## 下一节点
**NEEDS-HUMAN（主人 GUI 内一轮完成，不重试）**：①F-3 动态面（客户端队列不发起第 4 个 29）；②`design.md` §6.3 十条人工清单（步骤 `repair/followup-report.md` §4）——**须安排在无 e2e 运行、无并行服务端启停的独立时段执行**，逐条回填 `human-verified by <owner>, <date>`。两项闭环后本需求即满足 integration 三事实行。可选加固（待主人批准，套件为证据工件）：`start_server()` 记 PID + 存活轮询；safe-delete 钩子对 `dist/filesys/.staging` 设白名单。
