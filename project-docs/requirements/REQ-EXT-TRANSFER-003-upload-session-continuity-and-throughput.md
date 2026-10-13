# REQ-EXT-TRANSFER-003 上传会话连续性、进度可见与吞吐

> 阶段：goal（需求分析）｜日期：2026-10-08｜分级：**LARGE**
> 事实源：主人 2026-10-08 GUI 实测上报三条 + 本节点探针取证与全链路复核
> （`project-docs/evidence/REQ-EXT-TRANSFER-003/goal/复核发现.md`：G1–G8 / D1–D3，含代码锚点与可重跑探针）
> 上游：REQ-EXT-TRANSFER-001（多文件并发 + 上传暂停/续传，merged `d2148bf`）、
> REQ-EXT-FILE-001 T4（`upload_session` 持久化/暂存归属）、REQ-EXT-FILE-002（97/98 秒传预检查）
> **批准前禁止编辑 `Client/protocol.h` / `Server/protocol.h`**（`project-manifest.yaml` frozen_paths）。
> 逐字节设计与契约批准稿在 design 节点冻结（`tasks/REQ-EXT-TRANSFER-003/design.md`）。

## 1. 需求

上传路径的三条实测缺陷（主人 GUI 一轮发现，按原话）：

1. **进度不可见**：上传过程中 `upload_session` 记录的偏移量始终为 0，看不出服务端收到多少。
2. **会话不连续**：暂停之后断点续传时新建了一条上传会话记录。
3. **太慢**：4 KB 分片太慢；希望按固定块数生成分片、分片更大以提速。

复核附加（同批，见证据 §6）：客户端上传暂停/续传未摘旧 id 映射（与下载侧防御不对称）。

## 2. 事实取证（2026-10-08 实测；完整版见证据页）

| # | 事实 | 证据 |
|---|---|---|
| G1 | 上传中 `confirmed_offset` 恒 0（已发 1.5 MiB 仍 0）；**正常完成后仍 0** | 探针 B 组。**定性 = 设计承诺未实现**（2026-10-08 主人质询后复核）：REQ-EXT-FILE-001 T4 明写 `uploadfileinit/UPLOADFILE_DATA/完成` **三阶段**落库（`tasks/REQ-EXT-FILE-001/task.md:33`），实现里 DATA 阶段从未落库（`updateUploadOffset` 自 `6e023d5` 起无调用者，`35dfce7` 才首次被 abort 路径调用），完成路径 `finishUploadSession` 也不写该列（FILE-002 design §2.4 的完成行写法只在秒传命中路径实现）。该列今天无任何产品逻辑读取 |
| G2 | 1 MiB 后暂停 → 续传：行数 1 → 2（`expired` 残留 + 新 `uploading`） | 探针 C 组；29/97/101 三条建会话路径均 INSERT，`restartUploadSession` 只被 97 请求键路径使用 |
| G3 | 8 MiB 上传：4 KB 块 28.6 s（0.28 MiB/s）、16 KB 块 7.3 s（1.09 MiB/s）；每块固定成本 ~13–20 ms 且与块大小近乎无关 | 探针 A 组 + 三段分离（raw Win32：**成本全在 CreateFileW 12.3–13.5 ms**，CloseHandle 0.3 ms，未写过的文件 open+close 仅 0.5 ms）+ Qt 原生复核（服务端同一 API 面：每块形态 12.8 ms/块、单句柄 0.1–0.2 ms/块）。**不是线程**（Qt 全局线程池 worker 常驻；三支探针均无线程）、**不是句柄销毁**、**卷健康**（顺序写 112–409 MiB/s） |
| G4 | 客户端 4 KB 硬编码（`Client/uploader.cpp:71`）+ 单块在途（32 ACK 才发下一块） | 代码锚点 |
| G5 | 单块正文上限 16308 B（`MAX_INBOUND_PDU_SIZE=16 KB`，冻结面） | `Server/protocol.h:197-198`、`mytcpsocket.cpp:639-659` |
| G6 | 上传暂停/续传不摘旧 id 映射（下载侧已摘） | `transfertask.cpp:955-966`、`1000-1019` vs `934`、`972` |
| G7 | **断线后任务永久卡"传输中"**（验收 9 未实现） | `client.cpp:565-573`、`169-173`；`TransferManager` 零连接感知 |
| G8 | **群上传与个人根上传同名同大小共用同一暂存文件**（键不含群作用域） | `resumableuploadsession.cpp:84-93` + `msghandler.cpp:2377-2379` + `normalizeParent` 根→`/` |

**关键归因（本需求的设计前提）**：慢的主因不是块大小，而是服务端**每块重开写句柄**
（`Server/uploadwritetask.cpp:21-26` 每块 `QFile{open→seek→write→flush→close}`）。三段分离实测：
成本几乎全部落在 `CreateFileW`（12.3–13.5 ms/块），CloseHandle 仅 0.3 ms，未被写过的文件 open+close 仅 0.5 ms
→ 即「对一个刚被写过的文件再次以写权限打开」的代价（显式 fsync 移不走它），与线程无关
（Qt 全局线程池 worker 常驻复用，探针无线程亦复现）。Qt 单句柄形态 0.1–0.2 ms/块（~64–128×）。
只把块放大 4 倍只能快 4 倍（~1 MiB/s）；**改为每会话单句柄 + 放大分片**，两者都做才有数量级变化。

## 3. 范围

**范围内**（三条上报 + 一条同批防御）：

| # | 改动 | 冻结面 |
|---|---|---|
| S-A | **补齐 FILE-001 T4 承诺的 DATA 阶段落库**（欠账，非新功能）：上传期间按节流窗口写 `confirmed_offset`；完成按 FILE-002 §2.4 置为文件大小；失败/取消按暂存实际大小 | 否 |
| S-B | 会话行复用：同一逻辑上传（同一暂存键）在暂停/续传/重试间**复用同一行**（原地 `restartUploadSession`），29/97/101 三条路径统一；不再线性增生 `expired` 行 | 否 |
| S-C | 吞吐：服务端暂存**每会话单句柄**（去每块重开写句柄，实测 12.8 ms/块 → 0.1–0.2 ms/块）；客户端分片按文件大小定档（块数固定 + 上下限夹取） | 否 |
| S-D | 冻结面：上调 `Server/protocol.h` 的 `MAX_INBOUND_PDU_SIZE` / `MAX_PENDING_INBOUND_BYTES`，使大分片合法（终值 design 定） | **是（LARGE + 主人批准）** |
| S-E | 上传暂停/续传摘旧 id 映射（与下载侧对齐） | 否 |
| S-F | **G7 断线任务态收敛**（2026-10-08 主人指令纳入）：断线/心跳丢失 → 在途上传与下载标记失败并给出原因、名额归还、队列不空转；重连登录后 Queued 自动继续、Failed 可「重试」续传；本地分片与服务端暂存一律保留 | 否 |

**范围外**（本轮复核产出，建议另立任务，本需求不做）：

| # | 项 | 建议 |
|---|---|---|
| G8 | 暂存键纳入群作用域（跨群/个人根同名同大小互撞） | **主人 2026-10-08 裁决：不处理（风险接受）**——同名同大小互撞时若内容不同则 finalize 复算失败、不转正，不产生静默损坏件；如实记录不修 |
| D3 | 客户端下载落地无累计上界校验 | 记录不修（低危） |
| — | 单文件分片乱序/多块在途（流水线）、动态限速、分片级校验 | 非目标（沿用 TRANSFER-001 §2 非目标） |

## 4. 方案方向（design 节点定稿，本节点只给方向与约束）

1. **进度落库（S-A，= T4 三阶段里的 DATA 阶段）**：写点仍是 `updateUploadOffset`
   （语义 = 暂存物理大小），落库时机 = 每块接受后**按节流窗口**（每 ≥ 阈值字节 或 ≥ 阈值时间 落一次）；
   完成路径按 FILE-002 §2.4 补"置为 expected_size"（`staging_path` 是否清空待 design 裁定——
   转正后 staging 文件已被 rename 走，reclaim 只读 `uploading/verifying` 行，故清空与否当前无功能差异）。
   续传起点**仍以暂存物理大小为准**（不改成读 DB），DB 只是记录，避免引入"三方漂移"。
2. **行复用（S-B）**：查找键用**暂存路径**（= 确定性键 `sha1(owner|parent|display|size)` 的产物，
   即同一逻辑上传的身份）；只复用未完成状态行；命中即原地 UPDATE，未命中才 INSERT。
   复用不得违反 `uk_req`（`(owner_name, client_request_id)` 唯一）——复用路径不插行。
3. **吞吐（S-C）**：服务端把暂存句柄生命周期从"每块"提到"每会话"（写线程独占使用；
   abort/finalize 先关句柄再动文件——Windows 下改名前必须关闭）。实测依据：每块重开写句柄
   12.8 ms/块（Qt 原生）→ 单句柄 0.1–0.2 ms/块，且**保留每块 `flush()` 不损失收益**（C2 = 0.1 ms/块），
   故不改 flush 语义；客户端分片大小
   `chunk = clamp(ceil(size / N), MIN, MAX)`（N/MIN/MAX 终值 design 定，建议 N=1024、MIN=64 KiB、MAX=1 MiB）。
   **S-C 机制三选一（design 裁决；2026-10-08 主人提案 A 后逐条压测）**：

   | 方案 | 形态 | 改动量 | 跨线程风险 | 事件循环阻塞 |
   |---|---|---|---|---|
   | A（主人提案） | 每上传一条写线程，线程内 open 常开句柄，主线程信号槽派发写，取消/断网时线程内关闭 | 大（线程生命周期 + 四类关闭时序 + 回收连带收线程） | 中（须三条纪律：句柄只在 worker 内 open/close；关闭由持有者线程执行且 `clientOffLine` 的 `m_uploads.clear()` 不得抢先销毁 ctx；finalize/abort/删除必须先关句柄） | 无 |
   | B | 保留全局线程池，ctx 持共享句柄（`QSharedPointer<QFile>`），写任务 seek+write | 小 | 中（同三条纪律，句柄跨池线程复用） | 无 |
   | C（推荐先标定） | **不开线程**：句柄常开、31 处理里主线程内联 seek+write | 最小（可删 `Server/uploadwritetask.{h,cpp}`） | **无**（单线程独占；`clientOffLine` 天然安全；AUD-4 的 107×在途写竞态结构性消失） | 0.1–0.7 ms/块（实测：WriteFile 0.069 / 真 fsync 0.707 / Qt 单句柄+flush 0.1 @4KB） |

   标定门槛：单块"写 + flush"实测 ≤ ~2 ms 则采纳 C（与下载侧现状同构：`DownloadStream::file` = 单线程 + 常开句柄）；
   超阈值再退 B/A。A 的唯一必胜场景 = 单块写可能长时间阻塞（慢盘/冷缓存 1 MiB 块）。

4. **契约（S-D）**：只**放宽上限**，不删不改既有字段与编号；上调后每连接入站缓存仍须有界
   （`MAX_PENDING_INBOUND_BYTES` 必须 ≥ 单 PDU 上限 + 余量，否则大 PDU 会被"缓存超限"误断开——
   这是 `mytcpsocket.cpp:639` 与 `:653` 两处校验的耦合，design 必须同时处理）。
5. **兼容（不变式）**：老客户端/老脚本字节语义不变（上限是"允许更大"，不是"必须更大"）；
   续传起点语义不变（`parallel_e2e` T-09/T-20 是锚点）；`upload_session` schema **零变更**。

## 5. 验收标准（业务语言）

1. 上传中任意时刻查 `upload_session.confirmed_offset`，能反映服务端已确认字节
   （与真值差 ≤ 一个节流窗口）；上传完成后等于文件大小；不再出现"完成后仍为 0"。
2. 同一文件的**暂停→继续、失败→重试**全过程只保留**一条**会话记录（行数不增长）；
   历史 `expired` 行不再因续传而增生。
3. 上传吞吐：同一探针复跑，8 MiB 文件吞吐 ≥ **8 MiB/s**（基线 0.28 MiB/s @4 KB，
   ≥ 基线 30 倍；终值由 design 按实现后实测标定并写入套件）。
4. 分片按"固定块数 + 上下限夹取"生成：大文件分片随文件增大而增大，小文件不被切成碎片
   （≥ MIN）。
5. 断点续传语义不变：续传起点 = 暂存物理大小，前缀不重传（既有 T-09/T-20 保持绿）。
6. 老客户端在新服务端上的行为与字节语义逐字节不变；id=0 旧路径不变。
7. 既有回归全绿：master 侧 12 套；TRANSFER-002 合并后 13 套（含 `transfer002` 25 条）。
8. 大分片下的边界正确：0 字节文件、大小为分片整数倍、末块小于分片、并发 3 路上传各自正确落定。
9. **断线收敛（G7）**：传输过程中服务端消失 → 任务在秒级内标记失败并给出原因（不再永久"传输中"），名额归还、排队任务不空转；
   服务端恢复并重新登录后，「重试」可从断点续传完成（上传按服务端暂存、下载按本地分片），期间不删本地分片、不动服务端暂存。

## 6. 节点计划（LARGE skeleton）

goal（本文件 + `tasks/REQ-EXT-TRANSFER-003/task.md`）→ design（逐字节/常量终值/节流与复用规则/测试矩阵
→ **契约变更批准卡**）→ development → independent test（新会话）→ repair（按需）→
independent audit（新会话、只读）→ integration（三事实行）→ merge no-ff。
分支 `feat/req-ext-transfer-003-upload-session-throughput` 自 `master@d85e39f` 切出。

## 7. 开放问题（design 节点裁定）

1. 节流参数（字节/时间窗口）与写点（是否并入既有 `finishUploadSession` 语义）；
   失败/取消时 `confirmed_offset` 的取值口径。
2. 行复用查找键（暂存路径 vs `owner+parent+display+size`）与状态白名单；
   97 路径复用时是否改写 `client_request_id`（旧请求键重放语义）。
3. 冻结面常量终值：`MAX_INBOUND_PDU_SIZE` / `MAX_PENDING_INBOUND_BYTES` /
   客户端 N/MIN/MAX；上调后的抗 DoS 上界与 SEC-001 面复核方式。
4. 暂存句柄常开的并发与生命周期（**S-C 的全部风险集中在此**，见 §8 风险 R1–R3）：
   写线程独占使用；finalize（SHA 复算 + rename 转正）、abort（截断）、失败删除、断线清理四条"动文件"
   路径必须先关句柄；**断线清理 `clientOffLine()` 直接 `m_uploads.clear()` 不等在途写**
   （`Server/mytcpsocket.cpp`）→ 句柄必须共享所有权（`QSharedPointer<QFile>`）且由写任务持引用，
   否则 worker 仍在写时 ctx 被销毁即悬垂；空闲回收与句柄的交互（删除被句柄挡住 → 延后，不破坏）。
5. 分片策略边界（0 字节、末块、恰好整数倍）与客户端进度显示粒度是否足够。
6. 与 TRANSFER-002 分支的合并顺序（两者都改 `protocol.h` 尾部/中部常量区，插入点不同）。

## 8. 风险

| 风险 | 缓解 |
|---|---|
| 冻结面上调削弱抗 DoS 上界 | 只放宽"单 PDU 上限"，每连接仍保有界（上限 + 余量）；design 给出安全面复核，SEC-001 复跑 |
| **R1 断线清理 vs 在途写**（S-C 唯一真 bug 风险）：`clientOffLine()` 直接 `m_uploads.clear()`，不等在途写；今天安全只因写任务持"路径副本"，ctx 持句柄后即悬垂 | 句柄共享所有权（`QSharedPointer<QFile>`，写任务持引用），最后一个引用释放才关；ctx 清空不阻断在途写完成（其回调本就容忍 ctx 已消失） |
| **R2 动文件路径 vs 打开句柄**（Windows：rename/delete 遇打开句柄失败） | 四条路径统一"先关句柄再动文件"：finalize（`hashStaging` 复算 + `commitUpload` rename）、abort（`f.resize` 截断）、失败删除、回收；均天然落在"无在途写"的时点（`pendingAbort`/完成回调在主线程保证） |
| **R3 同一暂存文件被两个上下文同时打开**（既有隐患 G8：群上传/个人根同(名,大小)同键；续传换 id 瞬时重叠） | S-C **不加剧**（两上下文本就并发写同一文件；窗口 = 会话存活期，与句柄开合无关）；建议同批修掉 G8 的键，否则"常开句柄"让该既有隐患更易被观测 |
| 句柄常开与 abort/finalize/回收竞态 | 沿用既有 `writeInFlight`/`pendingAbort` 序列化（单写者模型零改动）；关句柄先于 finalize 改名与 abort 截断（Windows 语义） |
| 持久性降级 | **不降级**：保留每块 `flush()`（Qt 单句柄形态实测 0.1 ms/块，C2），不引入新的丢数据窗口 |
| 句柄数上限 | 每连接 ≤ 3 上传 + 3 下载 = 6 个常开句柄，可忽略 |
| 行复用撞 `uk_req` 唯一键 | 复用路径只 UPDATE 不 INSERT；复用前查状态白名单 |
| 吞吐改动引入回归 | `parallel_e2e`(54) / `fastupload`(46) / `groupfile`(45) 三套为准绳；新增专项套件（块数/大分片/进度/行复用） |
| 两条并行分支同改 `protocol.h` | 两分支改动点不同 hunk（TRANSFER-002 在文件尾追加块，本需求在中部常量）；先合并者进 trunk，后者合并前 pre-merge trunk 并复跑 |
| 大分片下内存/延迟放大 | 上限 1 MiB 级 + 单块在途，最坏内存 = 客户端 1 块 + 服务端 1 块/连接；design 记录上界 |

## 9. 取证页

`project-docs/evidence/REQ-EXT-TRANSFER-003/goal/复核发现.md`（G1–G8 / D1–D3、代码锚点、探针与日志、
取证口径与局限）。探针可重跑：`probe_upload_review.py` / `probe_upload_cost.py` / `probe_file_pattern.py`。
