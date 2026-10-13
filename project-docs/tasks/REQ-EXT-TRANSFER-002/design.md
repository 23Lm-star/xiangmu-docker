# REQ-EXT-TRANSFER-002 设计（design 节点）

> 分支 `feat/req-ext-transfer-002-download-pause-resume`｜基线 `master@d85e39f`｜节点 design｜日期 2026-10-08
> 档位 LARGE（goal 定档，不可改）｜`owner_agent: claude-code`｜`design_status: CONFIRMED`（2026-10-08 主人批准，见 §11）
> 上游：`requirements/REQ-EXT-TRANSFER-002-download-pause-cancel-resume.md`（§3 最小方案）、`tasks/REQ-EXT-TRANSFER-002/task.md`
> **本节点不写任何产品代码。** 契约变更于 2026-10-08 获主人批准（§11）；`Client/protocol.h` / `Server/protocol.h` 的编辑在 development 节点按 §9 逐字批准稿进行，**超出批准稿的改动一律不允许**。

---

## 0. 本文件冻结的内容（批准卡的对象）

| # | 冻结项 | 位置 |
|---|---|---|
| F1 | 逐字节 wire format：40/105 起点字段、41/106 采纳起点字段 | §2 |
| F2 | 冻结面新增内容（**逐字**，§9 为批准稿） | §9 |
| F3 | 服务端行为：107 泛化、流起点、校验与拒绝、上限交互 | §3 |
| F4 | 客户端状态机：暂停/继续/取消/重试、id 映射生命周期、分片文件生命周期 | §4 |
| F5 | 快照字段变更与旧快照迁移 | §4.4 |
| F6 | 不变式 I1–I7 | §7 |
| F7 | 测试矩阵与验收映射 | §6 |

`design_status` 在主人批准后由本文件改为 `CONFIRMED`，批准记录写入 §11。

---

## 1. 设计裁决（goal §6 四个开放问题 + 实现级补充）

| # | 问题 | 裁决 | 理由 |
|---|---|---|---|
| D1 | 续传起点非法的错误码 | 个人：41 `ret=false`（无独立码通道）；群：106 `code = -2108`（`-2107` 已被并发上限占用） | 两端都只需"明确拒绝"，不新增错误码枚举；客户端以"未接受续传起点"统一呈现 |
| D2 | 105 起点字段落位 | **caData[32..39]**，与 40 同一常量同一偏移 | 105 的 caData 除 [48..55] 外全空；群信封与尾随字节检查只作用于 caMsg，本方案 caMsg 零改动 → 该检查不受影响（代码事实：`Server/msghandler.cpp:2208` 只解析 `caMsg`） |
| D3 | 暂停后继续是否等待 108 | **不等待** | 同一 TCP 连接，107 必然先于新 40 被服务端处理（`MyTcpSocket` 单线程顺序处理入站 PDU）；等待只会引入超时态与额外状态 |
| D4 | 106 响应契约措辞 | 由「caData 全空」改为「**[24..31] = 服务端实际采纳的续传起点，其余区间保持全零**」 | 旧客户端只读 caMsg 与 [48..55]，不读 [24..31] → 零影响；契约变更记入 §9 |
| D5 | 续传起点 == 文件大小 | **受理**：0 字节正文 + 43 success | 客户端分片已完整但 43 丢失时，续传即收尾，不误判为失败 |
| D6 | 41 的采纳起点是否也回带 | **回带**（`DOWNLOAD_ACCEPTED_OFFSET`） | 这是"新客户端 × 旧服务端"的唯一护栏：旧服务端该区间恒 0（`mkPDU` 全量 memset），客户端请求 N>0 收到 0 即判定"对端未接受"→ 明确失败，**绝不把后缀写进错误偏移造成静默损坏**。代价 1 行写入 |
| D7 | 分片文件在失败时的处置 | **保留**（续传前提）；仅「取消」删除 | 失败即删（现状 G4）使续传不可能。代价：失败任务留同名分片，直到取消或重试成功——如实记录，不做后台清理 |
| D8 | 分片文件在"服务端已变化"时的处置 | **删除分片 + 起点归零** | 前缀内容已不可信，保留只会让下次续传拼出损坏件 |
| D9 | 续传起点取值的权威来源 | **磁盘分片实际大小**（`QFileInfo(savePath).size()`），不落盘进度字节 | 顺序写入保证分片大小 == 已收前缀长度；落盘进度会与服务端/磁盘三方漂移（沿用 transfer-001 的"不落盘进度"原则） |
| D10 | 客户端块大小 | 不调（服务端仍 64KB 块） | 本需求与吞吐无关，避免混入变量 |

---

## 2. 逐字节协议设计（F1）

### 2.1 通用规则

- **R1** 起点为 `quint64`，连接内作用域，客户端生成；**0 = 从头开始**。
- **R2** `mkPDU()` 全量 `memset(pdu,0,len)`（`Server/protocol.cpp:19`、`Client/protocol.cpp:22`）→ 旧客户端不写该区间时**必为 0**，"0 = 旧语义"由构造保证，不靠代码路径保证。
- **R3** 字节序 = 本机小端 `memcpy`，与既有全部字段一致。
- **R4** 服务端仅在该请求被受理并真的建流时写入采纳起点；其余情况（拒绝/无流）该区间保持 0。

### 2.2 落位表（新增字节）

| PDU | 方向 | 既有 caData 占用 | 新增 | 新常量 |
|---|---|---|---|---|
| 40 DOWNLOADFILE_REQUEST | C→S | [0..31] 名、[48..55] transfer_id | **[32..39] 请求起点** | `DOWNLOAD_RESUME_OFFSET` |
| 41 DOWNLOADFILE_RESPOND | S→C | [0] ret、[1..8] size、[16..23] id、[32..62] 名 | **[24..31] 采纳起点** | `DOWNLOAD_ACCEPTED_OFFSET` |
| 105 GROUP_FILE_DOWNLOAD_REQUEST | C→S | [48..55] transfer_id（caMsg=信封+gfid，**不变**） | **[32..39] 请求起点** | `DOWNLOAD_RESUME_OFFSET` |
| 106 GROUP_FILE_DOWNLOAD_RESPOND | S→C | [48..55] transfer_id（caMsg=信封+code+size+name，**不变**） | **[24..31] 采纳起点** | `DOWNLOAD_ACCEPTED_OFFSET` |

空隙核验（代码事实）：40 请求只读 `caData[0..31]`（`Server/msghandler.cpp:1737`）→ [32..47] 空闲；41 响应只写 [0]、[1..8]、[16..23]、[32..62] → [24..31] 空闲；105 请求与 106 响应的 caData 除 [48..55] 外全零 → 两区间空闲。

### 2.3 拒绝路径

| 情形 | 个人（40/41） | 群（105/106） |
|---|---|---|
| 起点 > 文件大小 | 41 `ret=false`，**不建流** | 106 `code=-2108`，**不建流** |
| 起点 == 文件大小 | 建流，立即 43 success（0 字节正文） | 同左 |
| 并发上限已满（既有 D7 语义不变） | 41 `ret=false` | 106 `code=-2107` |

---

## 3. 服务端设计（F3）

| # | 改动 | 位置（当前行号） |
|---|---|---|
| S1 | `handleUploadAbort` 泛化为 `handleTransferAbort`：`id != TRANSFER_ID_LEGACY` 时先查上传槽 `findUpload(id)`（**现有分支逐字节不变**），未命中再查 `m_downloads.contains(id)` → `removeDownloadStream(id)`；两者皆无 → `ret=false`（现状）。**`id == 0` 一律不受理**（维持既有断言 T-12a）。108 原样回带 id | mytcpsocket.cpp:391-415 |
| S2 | 40 分支与 105 分支读出起点（`TRANSFER_ID_OFFSET` 的同一读法）→ `m_MsgHandler->m_downloadResumeOffset`（新公有成员，与 `m_downloadSlotAvailable` 同风格） | mytcpsocket.cpp:63-77、213-228、301-315 |
| S3 | `startDownloadStream(path, id, startOffset)`：打开后 `seek(startOffset)`、`sentSize = startOffset`；`startOffset > totalSize` → 不建流（调用方已按 §2.3 拒绝，本函数只做断言式防护） | mytcpsocket.cpp:441-466 |
| S4 | 41 响应写 `DOWNLOAD_ACCEPTED_OFFSET` = 采纳起点（受理且建流时）；106 同理（写在 §2.2 位置，不改 caMsg 体） | msghandler.cpp:1777-1786、2212-2229 |

服务端零新增调度/定时器：107 释放后 `m_downloads.size()` 自然回落，名额即时可复用；round-robin 环与 256KB 出站高水位语义不变。

---

## 4. 客户端设计（F4 / F5）

### 4.1 状态机（下载）

```
Queued ──startTask──► Running ──43 success──► Done
   │                     │  │
   │                     │  └─43 fail / 校验失败──► Failed ──retry──► Queued(从分片续)
   │                     ├─pause(107)──► Paused ──resume──► Queued(从分片续)
   │                     └─cancel(107)──► (移除 + 删分片)
   └─pause / cancel ──► Paused / (移除)
```

### 4.2 关键改动

| # | 改动 | 位置 |
|---|---|---|
| C1 | `pause()` 增下载分支：Running → `sendPause(t.id)` → `closeRecv`（**保留分片**）→ 归还 `m_activeDownloads` → **摘旧 id 映射** → Paused；Queued → 移出 `m_downloadQueue` → Paused | transfertask.cpp:822-851 |
| C2 | `resume()`/`retry()` 下载分支：**摘旧 id 映射** → 新 id 入映射 → 起点 = `QFileInfo(t.savePath).size()`（savePath 空或文件不存在 → 0）→ `t.doneBytes` = 起点 → Queued | transfertask.cpp:854-890、937-954 |
| C3 | `startTask()` 下载组包：把 `t.doneBytes` 写入 40/105 的 `DOWNLOAD_RESUME_OFFSET` | transfertask.cpp:476-501 |
| C4 | `onDownloadRespond`/`onGroupDownloadRespond`：① `!ret`/`code!=0` → 失败（起点>0 时文案「服务端未接受续传起点」，含旧服务端）；② 读采纳起点，`accepted != requested` → 同①；③ `requested > 0` 且 `size != t.totalBytes` → 失败「服务端文件已变化，无法续传」+ **删分片 + 起点归零 + savePath 清空**（D8）；④ `requested > 0` → `resumeReceiving`，否则 → 既有 `beginReceiving`((N) 递推不变)；⑤ 统一 `t.totalBytes = size` | transfertask.cpp:701-765 |
| C5 | 新增 `resumeReceiving(Task&, qint64 offset)`：重开**同一** `savePath`（`ReadWrite`）+ `seek(offset)`，失败 → `failReason="无法打开下载分片"` | 新增，紧邻 `beginReceiving` |
| C6 | `onDownloadFinish` 失败分支：**不再 `QFile::remove`**（D7） | transfertask.cpp:781-793 |
| C7 | `cancel()` 下载分支：Running 先 `sendPause(t.id)`；删除分片（`QFile::remove(t.savePath)`）；摘 id 映射 | transfertask.cpp:896-910 |
| C8 | `sendAbortIfActive` 判定由 `t.isUpload() && Running` 放宽为 `Running`（107 现对两方向同义） | transfertask.cpp:804-809 |
| C9 | 快照新增下载的 `savePath`；恢复时按分片大小回填 `doneBytes`（分片不存在 → 0，走既有 `beginReceiving` = 今日行为） | transfertask.cpp:64-130 |
| C10 | 文件头注释同步（第 48-50 行「下载整文件重传」已过期）；面板提示文案更新 | transfertask.cpp:48-50、978、1026 |

### 4.3 id 映射生命周期（**必做防御**）

`m_idToKey` 必须在任务停止使用某 id 时摘除该条目：`pause`（旧 id）、`resume`/`retry`（旧 id）、`cancel`（旧 id）、`finishTask`（下载）。否则暂停瞬间仍在途的旧 id 的 42 数据块会命中映射，被写进续传后的文件 → **静默串写**。这是 G3 的既有隐患，本需求把它变成必发路径。

### 4.4 快照与旧格式迁移（F5）

- 新增字段：下载任务的 `savePath`（字符串）。**不落盘进度字节**（D9：恢复时由分片大小推导）。
- 旧快照（无 `savePath`）→ 恢复为 Paused、起点 0 → 「继续」走 `beginReceiving`，即**今日行为**，无回归。
- 分片存在于磁盘 → 恢复后起点 = 分片大小；分片缺失 → 起点 0。

---

## 5. 非目标（本节点明确排除）

单文件分片并行（P5）、动态分片、分片级校验/哈希、限速与配额、下载自动重连续传（恢复为 Paused，需用户点继续，与上传一致）、后台分片清理调度器、`Server/main.cpp` 的 `setQuitOnLastWindowClosed(false)`（transfer-001 遗留建议，另开需求）。

---

## 6. 测试矩阵（F7）

新套件：`project-docs/evidence/REQ-EXT-TRANSFER-002/verify.py`（裸 socket 驱动，自管服务端生命周期 `_harness/server_ctl.py`，注册进 `project-manifest.yaml` 的 `test_gate.suites`）。断言总数在 development 节点落定并写入 manifest，**只增不减**。

### 6.1 静态面

| # | 断言 | 覆盖 |
|---|---|---|
| S-01 | 两端 protocol.h 的 REQ-EXT-TRANSFER-002 常量块**逐字节一致**（含注释与偏移值） | F2 |
| S-02 | 新块位于 REQ-EXT-TRANSFER-001 块**之后**（既有块文本零改动 → `parallel_e2e` 的 P1-frozen-block 仍匹配） | F2 |
| S-03 | 40/105 起点偏移 == 32、41/106 采纳起点偏移 == 24（双端同值） | §2.2 |

### 6.2 动态面

| # | 断言 | 覆盖验收 |
|---|---|---|
| T2-01 | 基线：40 起点 0 → 41 `ret=true`、采纳起点 0、正文逐字节 == 源文件、43 success | 8（回归锚点） |
| T2-02 | 40 起点 N（0<N<size）→ 41 采纳起点 == N；正文逐字节 == 源文件[N:]；43 success | 1、2 |
| T2-03 | 起点 == size → 0 字节正文 + 43 success（D5） | 2 |
| T2-04 | 起点 > size → 41 `ret=false`；连接存活（后续心跳/别的下载正常） | 6 |
| T2-05 | 中途 107 → 108 `ret=true`；静默窗口内该 id **零帧** 42/43（服务端真停发） | 3 |
| T2-06 | 暂停 → 以新 id + 起点 M 续传：累计正文 == 源文件逐字节（前缀不重传：首块首字节 == 源文件[M]） | 1、2 |
| T2-07 | 暂停后名额释放：暂停 1 个后第 4 个下载被受理（`m_downloads` 回落） | 3、4 |
| T2-08 | 未知 id 的 107 → 108 `ret=false`（既有语义不变）；id=0 的 107 → `ret=false` | 8 |
| T2-09 | 群下载 105 起点 N → 106 `code=0`、采纳起点 == N、size == 源大小；正文 == 源[N:]；43 success | 1、2 |
| T2-10 | 群下载起点 > size → 106 `code=-2108`；连接存活 | 6 |
| T2-11 | 群下载暂停（107）→ 释放 + 静默；续传 == 源后缀 | 1、3 |
| T2-12 | 并发 3 下载中暂停 1 个：其余 2 个照常完成（43 success），被暂停者静默 | 4 |
| T2-13 | 下载暂停/取消期间上传流不受影响（29/31/32 正常走完） | 4 |
| T2-14 | id=0 的 40/42/43 全流程逐字节不变（与 `parallel_e2e` T-15 同源双保险） | 8 |

### 6.3 NEEDS-HUMAN（GUI，主人一轮完成，不重试）

1. 下载中点「暂停」→ 行状态变「已暂停」、进度停在当前百分比。
2. 点「继续」→ 进度从断点继续推进（不回 0）。
3. 续传完成的文件可正常打开，大小与源一致。
4. 点「取消」→ 任务行消失，且下载目录内**不再有**该分片文件。
5. 失败任务（含中途关服务端）重试 → 从断点继续而非 (N) 新文件。
6. 关闭客户端再启动 → 未完成下载显示「已暂停」，分片在磁盘则「继续」可续。
7. 分片被手动删除后点「继续」→ 从头重下、不报错、不产生损坏文件。
8. 暂停/取消期间上传任务照常推进（同连接互不干扰）。

### 6.4 回归面

既有 12 套件全绿，其中 `parallel_e2e` **54/54 零改动**（id=0 旧路径 + 多文件并行的双重锚点）；`groupfile`/`fastupload` 涉及 106/40 的路径零改动。

---

## 7. 不变式（F6）

- **I1** 旧客户端（起点恒 0）在新服务端上的行为与今日**逐字节一致**。
- **I2** 旧服务端（采纳起点恒 0）遇新客户端的续传请求 → 客户端**明确失败**，不产生损坏文件（D6）。
- **I3** 旧快照（无 savePath）恢复后的行为与今日一致（4.4）。
- **I4** 任一时刻，客户端分片文件大小 ∈ [0, 服务端已发送字节]，且续传起点 == 分片物理大小。
- **I5** 任一 id 只属于一个任务；任务换 id 时旧 id 立即失效（4.3）。
- **I6** 冻结面 diff 仅含 §9 批准稿内容，零删除、零重编号。
- **I7** 40/105 的 caMsg 与 106/41 的 caMsg 逐字节不变（群信封尾随字节检查语义不变）。

---

## 8. （保留）

---

## 9. 冻结面逐字批准稿（F2）

在两端 `protocol.h` 的 `MAX_PARALLEL_DOWNLOADS = 3;` 行**之后**、`#endif // PROTOCOL_H` **之前**插入以下 4 行（含空行分隔，两文件逐字节一致）：

```cpp

// ===== REQ-EXT-TRANSFER-002：下载续传起点（两端逐字节一致，仅追加；0 = 从头） =====
static const int DOWNLOAD_RESUME_OFFSET = 32;             // u64；40/105 请求：客户端声明的续传起点
static const int DOWNLOAD_ACCEPTED_OFFSET = 24;           // u64；41/106 响应：服务端实际采纳的起点
```

零删除、零重编号、零改动既有行。`parallel_e2e` 的 P1-frozen-block 正则终点为既有块尾行，插入点在其之后 → 该断言不受影响。

---

## 10. 风险

| 风险 | 缓解 |
|---|---|
| 陈旧 42 串写续传文件 | 4.3 摘映射 + T2-06（暂停瞬间仍在收块的场景） |
| 新客户端 × 旧服务端静默错位 | D6 采纳起点回带 + I2 |
| 冻结面双端漂移 | 9 逐字稿 + S-01/S-02 + 审计核对 diff |
| 失败留分片成垃圾文件 | D7 如实记录；取消即删；恢复时校验存在性 |
| 续传拼出损坏件 | D8 大小不一致即失败并删分片；T2-06 逐字节断言 |
| 106 契约变更被漏记 | D4 + S-03 + §2.2 落位表 |
| 客户端 UI 大改引入回归 | 本需求 UI 仅改提示文案（C10），无新控件 |

---

## 11. 批准记录

| 日期 | 项 | 结论 |
|---|---|---|
| 2026-10-08 | 契约变更批准卡（业务语言：下载可暂停/继续/取消；两端协议头各追加 4 行；老客户端零影响；新客户端配旧服务端会明确报错而非拼坏文件） | **主人批准：同意** |
| 2026-10-08 | goal 定档 LARGE（冻结面命中 `protocol.h`） | 主人决策：档位由 AI 按事实定档 → LARGE |

`design_status: CONFIRMED`（2026-10-08）。development 节点按 §9 逐字编辑冻结面；超出批准稿的改动一律不允许。
