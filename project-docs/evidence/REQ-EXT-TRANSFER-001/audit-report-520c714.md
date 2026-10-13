# REQ-EXT-TRANSFER-001 独立审计报告（independent audit）

- 节点：independent audit（Development OS v1.7 / LARGE，新会话、只读、detached 工作树）。
- 审计对象（候选）：`520c714`（`feat(transfer-001): 多文件有限并发（transfer_id 多路复用 + 107/108 + 下载 ring + 任务列表）`），完整哈希 `520c71444f993f7f74b1da84adb0fdb83bd1d92e`。基线 `master`。
- 审计快照：`E:/qt/xiangmu/build/wt-audit-transfer001`（`git worktree add --detach` @ `520c714`，干净：`git status --porcelain` 空）。本节点**零提交、零生产源码/文档改动**；唯一写盘为本报告。
- 只读边界：未 `git commit/push/merge/reset/checkout`；未启动服务端进程、未占用 5000 端口、未写数据库、未重跑 e2e harness。仅读 `git diff/show/log`、源文件、证据日志，并用 `codegraph_explore` 读依赖闭包。
- 独立性披露（不掩盖）：
  - 本审计为**新会话**，运行于**与 development/test 节点不同的模型**（deepseek-flash 系），在 detached 只读工作树内进行，未复用任何会话上下文。
  - 但任务记录 `owner_agent: claude-code`，development 与 independent test 亦为 Claude Code 系（test 报告 §独立性已自述"跨代理未满足"）。本机 **Codex CLI 不可用**（npm shim 指向缺失模块），**跨 CLI 审计无法执行**。故实际独立性 = "新会话 + 不同模型 + 独立工作树 + 独立重读源码/证据"，**不等于**跨工具链/跨厂商独立。此为残余限制，不计为满足完全独立。
- **结论：FAIL —— 1 条可归因于候选 `520c714` 的阻塞缺陷（AUD-1）**。test 节点"PASS，0 条候选归因缺陷"的结论**被推翻**：其七套件断言均为**裸 socket 直发 29/101**，绕过了客户端的 **97→29 秒传握手**，因此对 AUD-1 天然不可见。非阻塞观察 4 条、F-1 归类 1 条、证据缺口 1 条见下。

## 输入证据（read-only 复核）

- 设计：`project-docs/tasks/REQ-EXT-TRANSFER-001/design.md`（§9 逐字批准稿、§6.2 测试矩阵、§6.3 人工清单、D1–D8、§10 缺口）。
- 任务记录：`project-docs/tasks/REQ-EXT-TRANSFER-001/task.md`。
- development：`project-docs/evidence/REQ-EXT-TRANSFER-001/development/report.md`。
- independent test：`project-docs/evidence/REQ-EXT-TRANSFER-001/independent-test/test-report.md` 及同目录 `entry_ops.log`(43) `dedup.log`(13) `cross_tree.log`(41) `sec001.log`(48) `fastupload.log`(46) `groupfile.log`(45) `parallel_e2e.log`(43) `parallel_e2e-rerun.log`(43) `static-independent.log` `frozen-content.log` `readonly-confirm.log` `pipeline-audit.log` `environment.txt` `build-520c714-20261001-165632.log` `test-520c714-20261001-165735.log` `INDEX.md`。
- harness：`project-docs/evidence/REQ-EXT-TRANSFER-001/parallel_e2e.py`。
- 冻结路径：`project-docs/project-manifest.yaml`（`frozen_paths: Client/protocol.h, Server/protocol.h`）。
- 源码（只读）：`Server/msghandler.{h,cpp}`、`Server/mytcpsocket.{h,cpp}`、`Server/uploadwritetask.cpp`、`Client/transfertask.{h,cpp}`、`Client/file.cpp`、`Client/uploader.cpp`、`Client/client.cpp`。

---

## A. 冻结路径契约 — FACT（独立复证）

| 核验项 | 方法与结果 |
|---|---|
| 规模 | `git diff --numstat master...520c714 -- Client/protocol.h Server/protocol.h` = `11 0` + `11 0` = **22 insertions / 0 deletions** ✓ |
| 只追加 | `git diff ... \| grep -E '^-[^-]'` = **空**（append-only）✓ |
| 落位 | `107/108` 紧接 `106` 之后、`ENUM_MSG_TYPE_MAX=99999` 之前；常量块在末尾 `#endif` 之前 ✓ |
| 旧编号不变 | 枚举解析：29/31/40/42/43/101/105/106 值不变，无重编号 ✓ |
| 逐字节一致（design §9） | 脚本独立提取 `design.md` §9(a)/(b) 两个 cpp 代码块（去掉空行 = 9 行），与两端 diff 的 `+` 行（去空行 = 9 行）逐行比较：**Client=Server=设计稿，`EQUAL=True`** ✓（与 `frozen-content.log` 一致） |
| 0=旧路径 | `TRANSFER_ID_LEGACY = 0` 与 `mkPDU` 全量 `memset` 保证旧帧该区间恒 0（I1/I3）✓ |

**判定：冻结面变更 = 批准稿，无批准外改动。audit.ps1 RED 属预期冻结面命中（非缺陷）。**

## B. 路径边界 — FACT

`git diff --stat master...520c714` = **33 文件 / +3128 −778**（与 test 报告 §3 一致）。生产源码改动全部落在本需求分解 T1–T8（design §12）范围内：`Client/protocol.h`(frozen,批准)、`Server/protocol.h`(frozen,批准)、`Client/{client,file,groupfile,msghandler,uploader,transfertask}.{h,cpp}`、`Client/Client.pro`、`Server/{msghandler,mytcpsocket,mytcpserver,uploadwritetask}.{h,cpp}`。文档侧 `project-docs/**`（architecture/code-map/interfaces/modules/requirements/tasks/evidence）改动属预期。**未发现越界生产文件、未触碰 DB schema/迁移（I4）、未改既有 PDU 编号或 caMsg 字节。** 注：任务简报所述"design §8 = 允许路径"与文件实际不符——`design.md` §8 为"风险与缓解"；允许面以 §12 T1–T8 + manifest frozen_paths 为准。此仅为简报与文档的部位差异，不构成候选缺陷。

## C. 开发节点 3 条实现期裁定 — claim-check

1. **个人上传握手 = 97 → 29 → 31/32（97 建暂存会话，29 以 `m_pendingFastcheck` 采纳，不重复建会话、保留摘要对照）→ 成立，FACT。**
   代码：`Server/msghandler.cpp:1174-1206`（`adopt` 判定同 `parent/display/size` → 复用 `sessionId`/`expectedSha`；`m_pendingFastcheck.active=false` 一次性失效）；`Client/transfertask.h:46`（`needFastcheck=true`）、`transfertask.cpp:318-327`（Hashing→`hashDue`）、`file.cpp:130`（发 97，无 transfer_id）、`transfertask.cpp:284-290`（未命中→`sendPersonalUploadInit` 发 29）。fastupload 46/46、T-01/T-03 同绿佐证。**但该握手的副作用正是 AUD-1 的根因（97 在 `m_uploads[0]` 留下的上下文被计数）。**
2. **T-06 流数 = 1 大 + 2 小（3 条），非 design 写的 4 条 → 属实，FACT。**
   `parallel_e2e.py:8-9,430-437` 实测 3 条；`design.md:303` 写"8MB×1 + 64KB×3"（4 条）。与批准 D6（下载上限 3，`design.md:36`）冲突 → 见 F-1。
3. **97 命中（秒传）零正文完成；未命中才建槽 → 成立，FACT。**
   `Server/msghandler.cpp:1490-1500`（`ret && !hit` 时才 `m_pendingFastcheck.active=true`）；T-03/T-15 与 fastupload 46/46 同绿。**注意**：命中与未命中**都**在 `m_uploads[0]` 留下上下文（`msghandler.cpp:1304-1305` 先 `remove(0)` 再 `currentUpload()` 重建），命中路径同样占用上传名额——这是 AUD-1 的一部分。

## D. test 发现 F-1..F-5 逐条裁定

- **F-1（design 文档内部矛盾，需 design owner 处置）— 成立，正确归类，FACT。**
  design §6.2 T-06 要求"8MB×1 + 64KB×3 同时下载"= **4 条流**，与主人 2026-10-01 批准的 **D6（下载上限 3，`MAX_PARALLEL_DOWNLOADS=3`）** 直接冲突：第 4 条 40/105 必被拒（`Server/mytcpsocket.cpp:215-216,303-304`），该用例按原文**不可能通过**。这是**设计文档自相矛盾**，**不是产品缺陷**，也**不是测试放宽**。
  **必须的处置**：由 **design owner（`owner_agent`/architect）** 出具 disposition，二选一：(a) 修订 `design.md` §6.2 的 T-06 文本为 3 条流（与本实现一致）；或 (b) 提请主人**修改 D6** 上限。**审计不替其放宽 D6，也不自行改验收条件。** 审计意见：D6 是主人已批准的决策且 3 条流实现正确，最小代价是选 (a) 修文本；但改 D6 属主人权限，本审计不决定。**在 disposition 落定前，F-1 视为未闭环。**
- **F-2（T-14 断言弱于 design 文本）— 成立，FACT。** design §6.2 T-14 写"服务端无残留上下文"，`parallel_e2e.py:493-505` 实测仅断言"重连后新传输正常"。属**代理断言**（上下文随连接销毁不可同连接观测），方向正确，但报告强度不应读作"已直接证明无残留"。
- **F-3（T-02 自动化缺口）— 成立，FACT。** `parallel_e2e.py` 中**不存在 T-02**（裸 socket 无法断言客户端本地队列）；服务端等价面由 T-03 覆盖。客户端队列的自动化覆盖 = 0。
- **F-4（T-08 阈值来源）— 成立，FACT。** 阈值 2.0s 为开发标定；两次实测 max 往返 **0.007s / 0.001s**（`n=6`），余量约 3 个数量级，当前环境不构成虚设门。
- **F-5（人工清单缺项建议）— 成立，FACT（属建议，非缺陷）。** 需求验收中若干**纯客户端**项（单文件失败隔离+单独重试、并发下载同名 `(N)` 递推、超限"排队中"、断线后任务可重试）既无自动化、也未在 design §6.3 逐一列出。

## E. 实缺陷猎取（依赖闭包，源码直读）— 核心产出

### AUD-1（**BLOCKING**）— 97 秒传预检查的 id=0 上下文被计入上传并发名额，导致真实客户端只能并发 2 个"新"个人上传，第 3 个被拒并**标记失败**（违反 D6 / §5.2 / 验收 5）— FACT（源码级确定性推演；未执行）

**证据链（逐行）**：
- `Server/msghandler.cpp:1247-1260` `acquireUploadSlot(id)`：非 0 且有 `contains(id)` → 拒；否则 `activeUploadCount() >= MAX_PARALLEL_UPLOADS` → 拒。`Server/msghandler.h:54` `activeUploadCount() = int(m_uploads.size())`。
- `Server/msghandler.cpp:1301-1305` `uploadfilefastcheck()`：**每次 97 都** `m_uploads.remove(0)` 后 `currentUpload()` 重新插入 `m_uploads[0]`（97 按 P2 不带 id，恒落 id=0 槽）。该函数结束处**不删除** `m_uploads[0]`。
- `Server/msghandler.cpp:1160-1209` `uploadfileinit()`：对 `tid != 0` 只在 `m_uploads[tid]` 建上下文，**从不**清除 `m_uploads[0]`。
- 客户端 `Client/transfertask.h:46` `needFastcheck=true`（新上传默认走 97）；`transfertask.cpp:227-291`：97 **连接级单飞**，每收到 98 未命中即 `sendPersonalUploadInit`（发 29），**随后**才 `pumpFastcheck()` 发下一个 97；`file.cpp:114` 每文件都提交 97。

**确定性推演**（3 个新个人上传，客户端真实路径）：服务端按序收到 `97#1, 29#1, 97#2, 29#2, 97#3, 29#3`：
1. `97#1` → `m_uploads={0}`（count=1）
2. `29#1` → `acquireUploadSlot(id1)` count=1<3 → `{0,1}`（count=2）
3. `97#2` → remove(0)+重建 → `{0,1}`（count=2）
4. `29#2` → count=2<3 → `{0,1,2}`（count=3）
5. `97#3` → remove(0)+重建 → `{0,1,2}`（count=3）
6. `29#3` → `contains(3)` 假，`activeUploadCount()=3 >= 3` → **`ret=false`，不建状态**。
   客户端 `Client/transfertask.cpp:451-453` 收到 `ret=false` 即 `finishTask(Failed,"服务端拒绝上传初始化（并发上限或已存在）")`——**标记失败、不进队列**，与 design §3.3/§8"把 ret=false 当继续排队"相反。
   混杂群上传（101 走 `acquireUploadSlot`，`msghandler.cpp:2230`）与个人上传同样受该 id=0 占位影响。

**为何 test 未发现**：`parallel_e2e.py` 的 T-01 直接裸发 3 个 29（无前置 97），故 `m_uploads` 不含 id=0 陈旧槽，三路均 ret=true。真实客户端**每个新文件必发 97**，故必然触发。design §6.3 人工清单（未被执行）本应暴露此点。
**settling check（本审计不能执行，应交 repair/GUI 节点）**：单连接驱动 3 个新文件走真实 `97→29→31/32`（或 GUI 多选 3 个新文件上传），预期第 3 个 INIT `ret=false`、任务行显示"失败"。
**建议修复（auditor 只建议，不改码）**：让 97 的暂存上下文**不占用上传名额**——例如把 97 暂存从 `m_uploads` 移入专用成员（仅 `m_pendingFastcheck` 保存 sessionId/摘要），或采纳后立即 `m_uploads.remove(0)`，或在 `activeUploadCount()` 中排除"纯 fastcheck 占位"的 id=0 槽；同时客户端 `onUploadInitRespond` 对"上限拒绝"应改为**回队重试**（沿用 §3.3），而非标记 Failed。

### AUD-2（**NON-BLOCKING**）— 客户端把"上传 INIT 被拒"当失败而非排队（design §3.3/§8 明令"继续排队"）— FACT
`Client/transfertask.cpp:451-453`。与 AUD-1 叠加放大后果（否则 AUD-1 仅表现为"晚启动"）。**建议修复**：区分"上限拒绝"与"重复 id/非法"——前者 `resume/requeue`（等待同方向槽位释放事件驱动），后者失败。此项可与 AUD-1 同批修复。

### AUD-3（**NON-BLOCKING**）— 陈旧 98 误归属给新任务（单飞槽按"序号"而非请求标识归属）— CLAIM
`Client/transfertask.cpp:260-268` `onFastcheckRespond` 用 `m_fastcheckInFlight`（任务行键）认领 98，而非按 98 内的请求标识。若任务 A 的 97 在途时被 `cancel/pause`（`transfertask.cpp:669-679,728-732` 置 `m_fastcheckInFlight=0` 并 `pumpFastcheck()` 立刻发 B 的 97），其后**迟到的 A 的 98** 会被误判为 B 的结果（可能让 B 走错分支：误判命中/误发 29）。**settling check**：单连接"97 在途时取消该任务，再立刻启动另一上传"，观察 98 归属。**建议修复**：97 请求携带可回带的请求标识（如复用 `TRANSFER_ID_OFFSET` 之外的 request_id），或对迟到 98 做任务态/请求号校验后丢弃。

### AUD-4（**NON-BLOCKING**）— 上传在 `writeInFlight` 期间被 107 中止：服务端静默、暂存可能残留已写块 — CLAIM
`Server/mytcpsocket.cpp:384-398`（107 不检查 `writeInFlight` 即 `abortUpload` 移除上下文）；`UploadWriteTask` 用 `seek(offset)` 定位写（`uploadwritetask.cpp:21-26`，不越尾），`handleUploadWriteFinished` 在 `cp==nullptr` 时**直接返回不发 32**（`:363-366`）。若 107 与在途写并发，`session.receivedSize()` 不前进但物理暂存可能已含该块；续传走 `beginStaged` 的 `(owner|parent|display|size)` 确定性路径，起点以服务端回带为准，实际一致性未被证明。窗口窄、可自愈（60s 回收）。**settling check**：并发打 107 与 31 的竞态探针，比对 `upload_session.next_offset` 与暂存文件大小。**建议修复**：107 处理等待/忽略 `writeInFlight`，或在 aborted ctx 上保留写完成回调以推进/截断暂存。

### 对"0 条候选归因缺陷"的裁定
**不成立**：AUD-1 是候选 `520c714` 引入的确定性功能缺陷（97 握手与 per-transfer 名额共用 `m_uploads` 计数），test 套件因绕过 97 而未覆盖。其余方向（上传 DATA 逐 transfer 校验、下载 ring 公平性与摘环游标修正、下载/上传两方向预算独立、未知/重复 id 拒绝、60s 回收、断线清理、id=0 旧路径）经源码直读**未发现**新的阻塞缺陷，T-03/T-12/T-17/T-18/T-15 的裸 socket 证据与其一致。

## F. 证据完整性 — FACT（含 1 处缺口）

| 核验 | 结果 |
|---|---|
| 七套件总数 | `entry_ops 43/43`、`dedup 13/13`、`cross_tree 41/41`、`sec001 48/48`、`fastupload 46/46`、`groupfile 45/45`、`parallel_e2e 43/43`、`parallel_e2e-rerun 43/43` —— **全部与报告一致**（尾行 `=== 汇总 === N/N passed` 实测） |
| 确定性复跑 | 两次 `T-06a order=[212,213,211]`、`T-08 n=6 max=0.007s / 0.001s` —— **与报告 §4.1 一致** |
| audit RED 原因 | `pipeline-audit.log`：`FROZEN-PATH HIT: Client/protocol.h` + `Server/protocol.h`；`audit RED: frozen:...`；`impact-check: NOT_APPLICABLE` —— **属预期冻结面命中** |
| 环境 | `environment.txt`：`Python 3.13.15`、`worktree:520c714...`、`frozen diff 11 0 / 11 0`、`Server.exe 456704 bytes` —— 与报告一致 |
| 只读证明 | `readonly-confirm.log` 两条 `(end)` —— 工作树 tracked/untracked 均空 |
| 冻结面内容 | `frozen-content.log` 列出两端 `+` 行，两端同文，与 §9 一致（本审计另以脚本独立比对 `EQUAL=True`）|
| **缺口** | test 报告 §8 证据清单列出 `manifest-build-*` / `manifest-test-*`，但**该目录内不存在任何 `manifest-*` 文件**（`ls manifest-*` = No such file）。`INDEX.md` 亦未列出它们。**判定：evidence missing（报告过度声明），非 claim false**——三门日志 `build-520c714-*.log` / `test-520c714-*.log` / `pipeline-audit.log` 均真实存在且内容自洽，缺失项不影响已引用的关键结论，但报告 §8 应更正清单。 |

---

## 发现汇总（Findings）

| # | 严重度 | 标题 | 依据 |
|---|---|---|---|
| AUD-1 | **BLOCKING** | 97 秒传占位计入上传名额 → 第 3 个并发新个人上传被拒且标记失败（违反 D6/§5.2/验收 5） | `Server/msghandler.cpp:1247-1260,1301-1305,1160-1209`；`Client/transfertask.h:46`、`transfertask.cpp:227-291,451-453` |
| AUD-2 | NON-BLOCKING | 客户端将 INIT 上限拒绝当"失败"而非"排队"（违反 design §3.3/§8） | `Client/transfertask.cpp:451-453` |
| AUD-3 | NON-BLOCKING | 迟到 98 按序号误归属新任务（单飞槽无请求标识） | `Client/transfertask.cpp:260-268,669-679,728-732` |
| AUD-4 | NON-BLOCKING | 107 与在途写并发时服务端静默、暂存一致性未证明 | `Server/mytcpsocket.cpp:384-398,363-366`；`Server/uploadwritetask.cpp:21-26` |
| F-1-doc | 需 design owner 处置 | design §6.2 T-06（4 流）与批准 D6（3 流）自相矛盾 | `design.md:303` vs `design.md:36` |
| EV-1 | NON-BLOCKING（证据缺口） | test 报告 §8 列出的 `manifest-build-*`/`manifest-test-*` 不存在（evidence missing，非 claim false） | 目录实测 |

**每条建议修复见上（auditor 只建议，不修码）：**
1. AUD-1 建议修复：97 暂存上下文移出 `m_uploads`（或采纳后立即移除 id=0 槽，或 `activeUploadCount()` 排除 fastcheck 占位）；先在 GUI/客户端级复现确认。
2. AUD-2 建议修复：`onUploadInitRespond` 区分"上限拒绝"→回队重试、"重复 id/非法"→失败。
3. AUD-3 建议修复：97/98 引入可回带请求标识并据此归属，丢弃无匹配的迟到 98。
4. AUD-4 建议修复：107 与在途写有序化，或保留 aborted 回调以推进/截断暂存。
5. F-1-doc：由 design owner 出具 disposition（建议改 §6.2 文本为 3 流；改 D6 属主人权限）。
6. EV-1 建议修复：更正 test 报告 §8 证据清单（删除不存在的 `manifest-*` 项，或补交该文件）。

## 本审计未覆盖 / 边界

- **未执行任何运行时验证**：未启服务端、未跑 e2e、未写 DB、未做 GUI。AUD-1/AUD-3/AUD-4 为**源码级推演**（AUD-1 为确定性，AUD-3/4 标 CLAIM），其运行级确认需 repair/GUI 节点执行。
- **design §6.3 五条纯客户端人工清单项仍为 NEEDS-HUMAN，不计 pass**：多选 ≥3 文件、任务列表并发刷新不卡、上传任务暂停/继续/取消按钮（下载任务无）、3 个大文件 SHA 计算期间 UI 不卡（M5）、暂停后继续进度不回退。test 报告亦未执行；**F-5 建议追加的 4 项同为 NEEDS-HUMAN**。**AUD-1 恰恰落在这批未执行的人工项内**——这也是它逃过 test 的原因。
- 未复核下载 ring 在大规模/极端时钟下的抖动（仅两次复跑）；未做跨代理（Codex CLI）审计（本机不可用）。
- F-1 在 design owner 给出 disposition 前**未闭环**；本审计不修改验收条件、不放宽 D6。

## 结论

**FAIL：1 条阻塞缺陷（AUD-1）** —— 候选 `520c714` 的冻结面、路径边界、七套件与三门证据均自洽且未越界（A/B/C/F 均通过或仅非阻塞观察），但**真实客户端路径（97→29 秒传握手）下并发新个人上传退化为 2 个、第 3 个被拒并报失败**，违反主人批准的 D6（上传 3）与 design §3.3/§5.2/验收 5，且 test 因裸 socket 绕过 97 而未覆盖。**不可直接 integration**：需 repair 节点修复 AUD-1（建议连带 AUD-2），并由 design owner 处置 F-1 文档矛盾、test 节点更正 §8 证据清单。修复后需重跑（含**客户端级** 3 文件并发用例）方可复审。

## cleanup_status

- 审计工作树 `build/wt-audit-transfer001`（detached @ `520c714`）：**产品源码零改动**（未执行 `git add/commit`；未编辑任何仓库文件）。报告落盘于 `build/evidence/...`（gitignored）。
- 未启动服务端、未占用 5000 端口、未写数据库；未重跑 harness。
- 本报告唯一写盘：`E:/qt/xiangmu/build/evidence/claude-req-ext-transfer-001-audit/520c71444f993f7f74b1da84adb0fdb83bd1d92e/audit-report.md`。
