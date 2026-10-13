# REQ-EXT-TRANSFER-003 设计（design 节点）

> 分支 `feat/req-ext-transfer-003-upload-session-throughput`｜基线 `master@d85e39f`｜节点 design｜日期 2026-10-08
> 档位 LARGE（goal 定档，不可改）｜`owner_agent: claude-code`｜`design_status: CONFIRMED`（2026-10-08 主人批准，见 §10）
> 上游：`requirements/REQ-EXT-TRANSFER-003-upload-session-continuity-and-throughput.md`（G1–G8 / S-A…S-E）、
> `evidence/REQ-EXT-TRANSFER-003/goal/复核发现.md`（代码锚点 + 5 支可重跑探针）
> **本节点不写任何产品代码。** 冻结面（两端 `protocol.h`）编辑一律等 §9 批准稿获批后在 development 节点按逐字稿进行。

---

## 0. 本文件冻结的内容（批准卡的对象）

| # | 冻结项 | 位置 |
|---|---|---|
| F1 | 冻结面变更（**逐字**，§9 为批准稿）：`Server/protocol.h` 三分片上限常量 + 一处按类型取上限 | §9 |
| F2 | 大分片的 wire 语义：`MAX_UPLOAD_DATA_CHUNK = 1 MiB`（仅 31；通用 PDU 上限 16 KB 不动） | §2.1 |
| F3 | 客户端分片策略与常量落位（`Client/uploader.h`，**不入冻结面**） | §4.1 |
| F4 | 服务端：暂存句柄共享所有权（方案 B）与四类释放时机 | §3.1 |
| F5 | 服务端：进度落库节流口径 + 会话行复用规则 | §3.2 / §3.3 |
| F6 | 不变式 I1–I7 | §7 |
| F7 | 测试矩阵与验收映射 | §6 |

`design_status` 在主人批准后改为 `CONFIRMED`，批准记录写入 §10。

---

## 1. 设计裁决

| # | 问题 | 裁决 | 理由 |
|---|---|---|---|
| D1 | S-C 实现机制 | **方案 B（主人 2026-10-08 裁决）**：保留 `QThreadPool::globalInstance()` 工作池；暂存句柄改为**共享所有权**（`QSharedPointer<QFile>`）挂在会话上；写任务只 `seek+write+flush`，**不 open、不 close** | 改动量最小；三条跨线程纪律可在既有纪律上补齐（§3.1）；A（每上传一线程）作为慢盘退路保留不采纳；C（内联写）未采纳（主人明确选 B） |
| D2 | 大分片的契约形态 | **按 PDU 类型分设上限**：新增 `MAX_UPLOAD_DATA_CHUNK`/`MAX_UPLOAD_DATA_PDU_SIZE` **只对 31** 生效；`MAX_INBOUND_PDU_SIZE` 通用上限 **16 KB 保持不变** | 只放宽上传数据通道，其它所有报文类型的入站上界一字不动 → 抗 DoS 面最小改动；比"全局抬到 1 MiB"精确得多 |
| D3 | 入站缓存上界 | `MAX_PENDING_INBOUND_BYTES` 256 KB → **1 MiB + 8 KB**（= 单包上限 + 余量） | 必须 ≥ 单 PDU 上限，否则一个 1 MiB 包会在多段到达时触发"缓存超限"断连（`mytcpsocket.cpp:639` 与 `:653` 两处校验耦合）；仍每连接有界（4.3× 于旧值） |
| D4 | 客户端分片策略 | `chunk = clamp(ceil(size / 1024), 64 KiB, 1 MiB)`；常量进 `Client/uploader.h`（非冻结面） | 满足主人"按固定块数生成、分片更大"；下限防小文件碎块化，上限对齐服务端 |
| D5 | 服务端超长块处置 | 31 处理内新增**显式长度拒绝**（`uiMsgLen > MAX_UPLOAD_DATA_CHUNK` → `32{ok=false}`），不落到"非法 PDU → 计数断连"路径 | 大客户端/写错客户端得到明确失败而非断连，可诊断 |
| D6 | 进度落库节流 | 每块接受后：`receivedSize - lastPersisted >= 1 MiB` **或** `now - lastPersistMs >= 2000 ms` → 落库；完成置 `expected_size`；失败置 0；中止（107/回收）沿用现状（截断后暂存大小） | 既满足"上传中能看到进度"，又把 UPDATE 频次压到 ≤1 / MiB（1 MiB 块时 ≈ 每块一次、4 KB 旧客户端时 ≈ 每 256 块一次） |
| D7 | 会话行复用键 | 按 **(owner_name, staging_path)** 查未完成行（`uploading`/`expired`/`failed`/`verifying`，取 `session_id` 最大者）→ 原地 `restartUploadSession`；97 路径复用同时改写 `client_request_id = 本次 reqId`，29/101 路径不携带故不改写 | 暂存路径 = 确定性键 `sha1(owner|parent\|display\|size)` 的产物，即"同一逻辑上传"的唯一身份；97 改键保证任何一次请求键重放都能收敛到同一行（§3.3 收敛论证） |
| D8 | 历史 `expired` 残留行 | **不做迁移、不批量清理**，由既有 3600s TTL / 回收收敛 | 本次修复只保证"不再新增"；清理属 reclaim 面，混入会扩大爆炸半径 |
| D9 | 客户端空/已完成文件 | INIT 响应 `resumeOffset >= totalBytes`（含 0 字节文件 `0>=0`）→ 直接 `Done`，不建 `Uploader` | 服务端在 `result.complete` 时已转正（30 无 complete 字段可读）；不修则 0 字节文件在 GUI 上必失败（服务端却已建条目）——与验收 8 边界项冲突 |
| D10 | flush 语义 | **保持每块 `flush()`**（QFile::flush） | 实测单句柄形态含 flush 0.1 ms/块（C2），不构成瓶颈；不动它就无持久性语义变更 |

---

## 2. 契约与常量（F1 / F2）

### 2.1 逐字节/常量表

| 项 | 现值 | 新值 | 作用域 |
|---|---|---|---|
| `MAX_INBOUND_PDU_SIZE` | `16 * 1024` | **不变** | 全部入站 PDU（含 31 之外的一切） |
| `MAX_PENDING_INBOUND_BYTES` | `256 * 1024` | `1024 * 1024 + 8192` | 每连接入站累积上界 |
| `MAX_UPLOAD_DATA_CHUNK` | — | `1024 * 1024` | **仅 31** 的单块正文上限；客户端分片上限同值 |
| `MAX_UPLOAD_DATA_PDU_SIZE` | — | `MAX_UPLOAD_DATA_CHUNK + 4096` | **仅 31** 的整包上限（正文 + 头 + 余量） |

**兼容性**：只放宽上限，不改任何既有字段/偏移/编号；`sizeof(PDU)=76` 不变；旧客户端（16 KB 以内分片）字节行为不变；
旧服务端遇新客户端的大分片 → 走既有"非法 PDU"路径（计数后断连）——**单向兼容**，与既有 §9 兼容口径一致（本需求不要求新客户端配旧服务端用大分片：客户端首次连接若被断连则任务失败可重试，属已知单向性）。

### 2.2 唯一改动的读点（`Server/mytcpsocket.cpp:653` 一带）

```cpp
const uint inboundCap = (pdu->uiType == ENUM_MSG_TYPE_UPLOADFILE_DATA_REQUEST)
        ? MAX_UPLOAD_DATA_PDU_SIZE : MAX_INBOUND_PDU_SIZE;
if (pduTotalLen >= sizeof(PDU) && pduTotalLen <= inboundCap) { ... }
```

`uiType` 位于已确认长度的 76 B 头内（`m_dpulist.size() >= sizeof(PDU)` 已成立），读它不越界。

---

## 3. 服务端设计

### 3.1 句柄共享所有权（F4，方案 B）

| # | 改动 | 位置 |
|---|---|---|
| S1 | `ResumableUploadSession` 增 `QSharedPointer<QFile> m_stagingFile`；`beginStaged` 打开后**不移交、不关闭**（`ReadWrite`；`offset==0` 时带 `Truncate`，否则直接 `ReadWrite`——不再需要 `Append`，因为每块显式 `seek`） | `resumableuploadsession.cpp:71-126` |
| S2 | 新增 `stagingFile()` 访问器 + `releaseStagingFile()`（`QSharedPointer::clear()`，最后一个引用释放即关句柄）；`reset()` 一并释放 | `resumableuploadsession.{h,cpp}` |
| S3 | `UploadWriteTask` 改为持共享句柄：`run()` 只 `seek(offset)+write(data)+flush()`；**不 open/close**；失败仍走既有 `handleUploadWriteFinished(...false)` | `uploadwritetask.{h,cpp}:8-35` |
| S4 | `MyTcpSocket::handleUploadData` 构造任务时传 `cp->session.stagingFile()`（不再传路径） | `mytcpsocket.cpp:356-367` |
| S5 | **四类释放时机**（R2：动文件前必须先没有打开句柄）：① finalize：`hashStaging` 复算与 `commitUpload` rename 之前 `releaseStagingFile()`；② abort：`f.resize(confirmed)` 截断前释放；③ 摘要不符/失败删除前释放；④ 会话被清（`clientOffLine`/`m_uploads.clear()`/回收）时随 `UploadCtx` 析构释放 | `msghandler.cpp:1665-1727` / `:1320-1351` / `fileobjectstore.cpp` 调用点 |
| S6 | **跨线程纪律（R1）**：句柄只被两类代码触碰——(a) 写任务（池线程，同一 transfer 由 `writeInFlight` 保证串行）；(b) 主线程，且**仅在 `!writeInFlight` 时**（finalize 由完成回调驱动、abort 由 `pendingAbort` 延迟、INIT 时无在途写）。共享所有权保证 ctx 被销毁时在途写仍安全（任务持引用，句柄最后由任务析构关闭） | 纪律，无新代码 |
| S7 | 31 的超长块拒绝（D5）：在既有一串校验前追加 `pdu->uiMsgLen > MAX_UPLOAD_DATA_CHUNK → ack(false)` | `mytcpsocket.cpp:355-362` |

**不改**：`writeInFlight` 单块在途闸、`pendingAbort` 延迟释放、60 s 空闲回收、`clientOffLine` 的 `m_uploads.clear()` 行为、下载路径句柄模型。

### 3.2 进度落库（F5 / S-A）

| # | 改动 | 位置 |
|---|---|---|
| S8 | `UploadCtx` 增 `qint64 lastPersistOffset = 0; qint64 lastPersistMs = 0;`（INIT 时以 `result.offset` 与 `now` 初始化） | `msghandler.h:24-39` |
| S9 | `handleUploadWriteFinished` 成功接受块后按 D6 判定 → `OperateDB::updateUploadOffset(sessionId, receivedSize)`；同时刷新两个字段 | `mytcpsocket.cpp:370-398` |
| S10 | `finishUploadSession` 增可选参数 `confirmedOffset = -1`（`-1` = 不写）：完成路径传 `expected_size`（对齐 FILE-002 design §2.4），失败路径传 `0`（暂存已删，值无意义） | `operatedb.cpp:505-523`、`msghandler.cpp:1716-1724` |
| S11 | 「续传起点仍取暂存物理大小」**不变**（不改成读 DB）；`confirmed_offset` 保持"记录"语义 | 不变式 I5 |

### 3.3 会话行复用（F5 / S-B）

| # | 改动 | 位置 |
|---|---|---|
| S12 | 新增 `OperateDB::findReusableUploadSession(owner, stagingPath)`：`SELECT session_id FROM upload_session WHERE owner_name=? AND staging_path=? AND status IN ('uploading','expired','failed','verifying') ORDER BY session_id DESC LIMIT 1` | `operatedb.{h,cpp}` |
| S13 | 29 路径：`createUploadSession` 之前先查复用（命中 → `restartUploadSession`，`client_request_id` 不动） | `msghandler.cpp:1238-1245` |
| S14 | 97 路径：`beginStagedSession(retrySessionId)` 的入参来源扩展——先按请求键查（现状），未命中再按暂存键查（S12）；复用且本次带 reqId 时**改写 `client_request_id`**（`restartUploadSession` 增可选 reqId 参数） | `msghandler.cpp:1482-1512`、`operatedb.cpp:435-461` |
| S15 | 101 群路径：同 S13（`client_request_id` 为 NULL，不携带） | `msghandler.cpp:2386-2388` |

**收敛论证（S14 为何改键）**：行与暂存键一一对应（同一 `.part`）。任何一次 97（含重放）都会执行"按 reqId 查 → 未命中 → 按暂存键查 → 命中即复用并改键"，
故不同 reqId 的多次请求最终都收敛到同一行；被改掉的旧 reqId 下次重放时同样收敛回该行。不改键则"新 reqId 查不到 → 走到步骤 4 内容预检查 → INSERT 新行"，与 S-B 目标直接冲突。

---

## 4. 客户端设计

### 4.1 分片策略（F3）

| # | 改动 | 位置 |
|---|---|---|
| C1 | `Client/uploader.h` 增常量与计算：`UPLOAD_CHUNK_TARGET_COUNT = 1024`、`UPLOAD_CHUNK_MIN = 64*1024`、`UPLOAD_CHUNK_MAX = 1024*1024`（**注释标明上限须 ≤ 服务端 `MAX_UPLOAD_DATA_CHUNK`**）；`Uploader::start()` 按文件大小算 `m_chunkSize`，`sendNextChunk()` 用它 `read()` | `uploader.h`、`uploader.cpp:19-41,65-87` |
| C2 | 分片计算：`chunk = qBound<qint64>(MIN, (size + N - 1) / N, MAX)`；`size==0` 时取 MIN（由 C3 兜底不发送） | 同上 |

### 4.2 正确性与防御

| # | 改动 | 位置 |
|---|---|---|
| C3 | 空/已完成文件（D9）：个人路径 `onUploadInitRespond` 与群路径 `onGroupUploadInitRespond` 中，若 `resumeOffset >= totalBytes` → 直接 `finishTask(Done)`（不建 `Uploader`）；`totalBytes` 以 INIT 前本地大小为准，`>=` 兼容 0 字节 | `transfertask.cpp:625-672,693-744` |
| C4 | 摘旧 id 映射（S-E）：上传 `pause()` 与 `resume()` 分支补 `m_idToKey.remove(t->id)`，与下载侧 §4.3 对齐（防旧 id 的迟到 32 喂进新 uploader） | `transfertask.cpp:917-966,1000-1019` |

### 4.3 不改动

UI/面板文案（新增失败原因文案除外）、快照格式、下载路径、组包偏移（`UPLOAD_CHUNK_OFFSET` 等一律不动）。

### 4.4 G7 断线任务态收敛（**2026-10-08 主人指令纳入本需求**；纯客户端，不涉冻结面）

现状（G7）：`Client::onSocketDisconnected()`（`client.cpp:565-573`）只弹重连窗；心跳超时（`:169-173`，`SERVER_ALIVE_TIMEOUT_MS=90000`）只调 `onServerLost()`；
`TransferManager` 零连接感知 → 在途任务既无 32/43 也无失败，**永久停"传输中"且占着名额**，队列不再前进；同一用户重连登录时 `setCurrentUser()` 提前返回（内存为准）也不复位。

| # | 改动 | 位置 |
|---|---|---|
| C5 | 新增 `TransferManager::onConnectionLost(const QString& reason)`：置 `m_connectionDown=true`；把 **Running / Hashing** 的任务逐个 `m_idToKey.remove(t->id)`（§4.3 同款防御）后 `finishTask(*t, Failed, "与服务端断开连接，可重试")`（名额归还、快照落盘）；清空 `m_fastcheckQueue` 与在途 `m_fastcheckInFlight`（迟到 98 由既有"无在途即丢弃"吞掉）；**Queued 保持 Queued、Paused 保持 Paused、Done/Failed 不动** | `transfertask.cpp`（新增）、`transfertask.h` |
| C6 | 防"失败后被迟到回调复活"：群上传 SHA 的 `QFutureWatcher` 回调在 `taskByKey` 之后补 `if (tt->state != Hashing) return;`；`submitFastcheck()` 补同款状态守卫 | `transfertask.cpp:298-331,370-378` |
| C7 | 断线期间不启动新任务、重连后恢复：`pump()` 首部 `if (m_connectionDown) return;`；新增 `TransferManager::onConnectionRestored()`（清标志 + `pump()`），由 `Client::handlMsg` 的 `LOGIN_RESPOND` 分支在 `setCurrentUser(...)` 之后调用（**不依赖 `setCurrentUser` 的提前返回**，因为同用户重连会走提前返回） | `transfertask.cpp`、`client.cpp:194-200` |
| C8 | 触发点（两处，均为主线程事件）：① `Client::onSocketDisconnected()` 内直接调用 `onConnectionLost("与服务端断开连接，可重试")`（服务端被杀/网络断）；② `onServerLost()`（90 s 心跳丢失，服务端假死）同样调用 | `client.cpp:169-173,565-573` |
| C9 | 重试即续传（**零新代码**，靠既有路径）：上传 `retry`→`resume`→新 id + 29，服务端按暂存物理大小回带 `resumeOffset`；下载 `retry`→`resumeDownload`→起点 = 分片物理大小（D7 分片保留） | `transfertask.cpp:991-1019,970-988` |

**边界与不做**：断线**不删**任何本地分片、不动服务端暂存与会话行（留给续传）；不做自动重连续传（恢复为 Failed，需用户点「重试」——与 TRANSFER-001 验收 9 措辞一致）；不做"断线自动重发"；服务端零改动。

---

## 5. 非目标

单文件乱序/多块在途（流水线）、动态限速、**G8（暂存键纳入群作用域——2026-10-08 主人裁决不处理，风险接受：见 §8）**、
D3 下载落地长度上界、历史 `expired` 行迁移清理、`ResumableUploadSession::begin()`（无调用者的死代码，保留不删）、
断线自动重连续传（G7 只做"标记失败 + 可重试"，不做自动重发）。

---

## 6. 测试矩阵（F7）

新套件 `project-docs/evidence/REQ-EXT-TRANSFER-003/verify.py`（裸 socket 自管生命周期，注册进 `project-manifest.yaml`），断言总数在 development 落定后写回 manifest，**只增不减**。

### 6.1 静态面

| # | 断言 | 覆盖 |
|---|---|---|
| S-01 | `Server/protocol.h` 的 REQ-EXT-TRANSFER-003 常量块**逐字节 == §9 批准稿**；`MAX_INBOUND_PDU_SIZE` 仍为 `16 * 1024`（通用上限未被抬高） | F1/D2 |
| S-02 | `Client/uploader.h` 三常量存在且 `UPLOAD_CHUNK_MAX ≤ 服务端 MAX_UPLOAD_DATA_CHUNK`（跨端一致性静态校验） | F3 |
| S-03 | 冻结面 diff 仅含 §9 内容（`git diff master...HEAD -- Server/protocol.h` 行数 == 批准稿） | F1 |
| S-04 | G7 接线（源码文本断言，仿 `parallel_e2e` T-22 客户端静态契约口径）：`onSocketDisconnected` 与 `onServerLost` 均调用 `onConnectionLost`；`pump()` 首部有 `m_connectionDown` 闸；`LOGIN_RESPOND` 分支调用 `onConnectionRestored` | G7 |
| S-05 | G7 防复活守卫：群上传 SHA 回调与 `submitFastcheck` 均含 `state != Hashing` 守卫 | G7 |

### 6.2 动态面

| # | 断言 | 覆盖验收 |
|---|---|---|
| T3-01 | **进度落库**：上传 4 MiB、每 1 MiB 采样一次 DB → `confirmed_offset` 单调不减且末次 ≥ 已发字节 − 1 MiB；完成后 == `expected_size` | 1 |
| T3-02 | **单行贯穿**：暂停（107）→ 续传（新 id 29）→ 完成，全程 `upload_session` 行数 == 1（G2 回归锚点） | 2 |
| T3-03 | **行复用键**：97 → 29 未完成 → 再次 97（新 reqId）→ 行数仍 == 1 且 `client_request_id` == 最后一个 reqId | 2 / D7 |
| T3-04 | **群路径单行**：101 → 107 → 101 续传，群会话行数 == 1 | 2 |
| T3-05 | **大分片合法**：单块 1 MiB 上传成功（32 ok、转正、SHA 一致）；1 MiB + 1 字节的块被 `32{ok=false}` 明确拒绝且连接存活 | 3 / D5 |
| T3-06 | **吞吐**：8 MiB 文件用新分片策略（64 KiB 块，8 MiB/1024 < MIN）裸 socket 上传，吞吐 ≥ 8 MiB/s（基线 0.28 MiB/s @4 KB；同机同法对比） | 3 |
| T3-07 | **边界**：0 字节、恰好 1 块、末块 < 块大小、恰好 1 MiB、1 MiB+1 五档均正确转正、SHA 一致 | 8 |
| T3-08 | **并发 3 路**：3 路大分片上传并发，各自逐字节正确、互不串扰（`upload_session` 各自单行） | 8 |
| T3-09 | **续传前缀不重传**（锚点保持）：107 后 30 回带 `resumeOffset == 暂存物理大小`；首块首字节 == 源[offset] | 5 |
| T3-10 | **107 × 在途写**（AUD-4 锚点保持）：31 发出后不等 32 立即 107，重复 6 轮 → 108 全 true、`confirmed_offset == 暂存物理大小`、续传起点 == 暂存大小 | 5 / I6 |
| T3-11 | **通用上限未被抬高**：发一条 17 KB 的非 31 报文（如 23 flushfile）→ 走既有非法 PDU 处理（不因本次改动变宽松） | D2 |
| T3-12 | **旧客户端兼容**：4 KB 分片 + id=0 单流全流程逐字节不变（与 `parallel_e2e` T-15 同源双保险） | 6 |

### 6.3 NEEDS-HUMAN（GUI，主人一轮，不重试）

1. GUI 上传一个 ≥100 MB 文件，肉眼可见速度提升（对比本次修复前的 0.28 MiB/s）。
2. 上传中在 DB 里看 `upload_session.confirmed_offset` 随进度前进；完成后 == 文件大小。
3. 暂停 → 继续 → 完成：`upload_session` 只有一行；任务行进度不回退。
4. 0 字节文件上传 → 任务显示"已完成"，服务端有条目（C3 人工面）。
5. **G7 断线收敛**：上传/下载进行中把服务端进程杀掉 → 任务行在秒级内变「失败」并给出原因（不再永久"传输中"）；重启服务端、客户端重连并重新登录后点「重试」→ 从断点续传完成（上传按服务端暂存、下载按本地分片）。另：连续队列中有排队任务时，断线期间不得发出新的 INIT。

### 6.4 回归面

master 侧 12 套全绿；TRANSFER-002 合并后 13 套（含 `transfer002` 25 条）。重点：`parallel_e2e`(54) / `fastupload`(46) / `groupfile`(45) 三套覆盖上传全路径与旧客户端形态。

---

## 7. 不变式（F6）

- **I1** 旧客户端（≤16 KB 分片、id=0 单流）在新服务端上字节行为**逐字节不变**。
- **I2** 通用入站上限 `MAX_INBOUND_PDU_SIZE` 保持 16 KB；放宽只对 31 生效。
- **I3** 续传起点语义不变：仍 = 暂存物理大小，不读 DB。
- **I4** 任一时刻 `upload_session.confirmed_offset` ∈ [上一落库值, 暂存物理大小]；完成后 == `expected_size`。
- **I5** 同一逻辑上传（同一 owner + 暂存键）在暂停/续传/重试/重放全过程只有一行未完成会话记录。
- **I6** `writeInFlight` 单块在途模型与 `pendingAbort` 语义零改动；AUD-4 锚点 T-20 仍绿。
- **I7** 句柄释放先于一切"动文件"（rename/delete/truncate）；在途写完成前不得释放 ctx。

---

## 8. 风险

| 风险 | 缓解 |
|---|---|
| 共享句柄跨池线程使用 | `writeInFlight` 保证同一 transfer 串行；完成回投走 QueuedConnection（同步边）；ctx 销毁不阻断在途写（共享所有权） |
| 句柄未释放导致 rename/delete 失败（Windows） | S5 四类时机显式 `releaseStagingFile()`；T3-07 覆盖转正/失败两路 |
| `MAX_PENDING_INBOUND_BYTES` 上调削弱抗 DoS | 只调到"单包 + 余量"（1 MiB + 8 KB），仍每连接有界；T3-11 断言通用上限未松动 |
| 复用键命中错误对象（历史行 staging_path 巧合相同） | 暂存键 = `sha1(owner\|parent\|display\|size)`，同键即同一物理文件、同一逻辑上传；状态白名单排除 `done` |
| 节流落库漏掉最后一次 | 完成路径强制写 `expected_size`（S10），完成态不依赖节流 |
| 分片变大后单块失败代价变大 | 客户端重试=续传（起点取自服务端回带），失败块不重传前缀 |
| 与 TRANSFER-002 并行改 `protocol.h` | 插入点不同（其在文件尾追加块；本需求改中部常量区与读点），合并按 trunk 顺序 pre-merge 处理 |
| G7 误伤：心跳 90 s 抖动被当成断线 | 触发点用事件（`disconnected` 信号）+ 90 s 级心跳丢失；失败态**保留全部续传前提**（服务端暂存/本地分片），重试成本 = 一次续传 |
| G8 不修的风险接受（主人 2026-10-08 裁决） | 同一 owner 下"群上传"与"个人根上传"同名同大小、或同一文件并发传两份 → 共用同一 `.part`（内容不同则 SHA 复算失败、不转正，不产生静默损坏件）；如实记录不改键 |

---

## 9. 冻结面逐字批准稿（F1）

`Server/protocol.h`：**仅**以下两处。`Client/protocol.h` **零改动**。

**(a) 在既有 `MAX_INBOUND_PDU_SIZE` / `MAX_PENDING_INBOUND_BYTES` 两行处替换为**（原两行删除，新增四行）：

```cpp
static constexpr uint MAX_INBOUND_PDU_SIZE = 16 * 1024;   // 通用入站单包上限（16 KB）——REQ-EXT-TRANSFER-003 抬高的只有上传数据通道
// ===== REQ-EXT-TRANSFER-003：上传数据通道（31）分片上限（仅 31 生效；客户端 Client/uploader.h 的 UPLOAD_CHUNK_MAX 须 ≤ 此处） =====
static constexpr uint MAX_UPLOAD_DATA_CHUNK = 1024 * 1024;                    // 31 单块正文上限 1 MiB
static constexpr uint MAX_UPLOAD_DATA_PDU_SIZE = MAX_UPLOAD_DATA_CHUNK + 4096; // 31 整包上限 = 正文 + 头/余量
static constexpr int  MAX_PENDING_INBOUND_BYTES = 1024 * 1024 + 8192;         // 每连接入站累积上界（≥ 单包上限 + 余量；原 256 KB）
```

**(b) `Server/mytcpsocket.cpp` 的入站校验**（非冻结面，此处列出以便审计核对与本需求一致）：

```cpp
const uint inboundCap = (pdu->uiType == ENUM_MSG_TYPE_UPLOADFILE_DATA_REQUEST)
        ? MAX_UPLOAD_DATA_PDU_SIZE : MAX_INBOUND_PDU_SIZE;
```

**批准稿范围外**：枚举值、既有偏移常量、`caMsg` 语义、`Client/protocol.h` 全部内容、`sizeof(PDU)`。

**批准后自检命令**：`git diff --stat master...HEAD -- Server/protocol.h Client/protocol.h` ——
预期 `Server/protocol.h` 为 +4/−1（或等价的批准稿行数）、`Client/protocol.h` **零改动**。

---

## 10. 批准记录

| 日期 | 项 | 结论 |
|---|---|---|
| 2026-10-08 | S-C 实现机制 | **主人裁决：方案 B**（保留线程池 + 句柄共享所有权；A 仅作慢盘退路，C 未采纳） |
| 2026-10-08 | 范围增补：G7（断线任务态收敛）纳入本需求；G8 明确不处理 | **主人指令**（"G7本次一次性修复了，G8不用管"）。G7 为纯客户端、不涉冻结面 → 不触发新批准；本文件 §4.4 为 CONFIRMED 后的设计增补 |
| 2026-10-08 | 契约变更批准卡（业务语言：上传分片上限从 16 KB 放宽到 1 MiB，**只放宽上传数据通道 31**，其它报文的入站上限一分不动；老客户端零影响） | **主人批准：行，没问题了（同意）** |

`design_status: CONFIRMED`（2026-10-08）。development 节点按 §9 逐字改冻结面；超出批准稿的改动一律不允许。

---

## 11. 下一节点（development）

| # | 任务 | 冻结面 |
|---|---|---|
| T1 | 按 §9 逐字改 `Server/protocol.h` + 入站校验按类型取上限 | **是**（仅批准稿） |
| T2 | 服务端 S1–S7（共享句柄 + 四类释放时机 + 超长块拒绝） | 否 |
| T3 | 服务端 S8–S15（节流落库 + 会话行复用） | 否 |
| T4 | 客户端 C1–C4（分片策略 + 空文件 Done + 摘旧映射）+ C5–C9（G7 断线收敛） | 否 |
| T5 | 新套件 `verify.py`（§6.1–6.2）+ manifest 注册 | 否 |
| T6 | 回归 12 套（TRANSFER-002 合并后 13 套）+ 文档同步（interfaces.md 常量表、modules/architecture 相关段） | 否 |

development 退出条件：`pipeline/build.ps1` 绿 + 全套回归绿 + 新套件全绿 → independent test（新会话）→ audit（新会话、只读）→ integration 三事实行 → merge no-ff。
