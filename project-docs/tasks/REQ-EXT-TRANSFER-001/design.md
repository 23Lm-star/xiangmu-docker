# REQ-EXT-TRANSFER-001 设计（design 节点）

> 分支 `feat/req-ext-transfer-001-parallel-transfer`｜基线 `master@53b048b`｜节点 design｜日期 2026-10-01
> 档位 LARGE（goal 定档，不可改）｜`owner_agent: claude-code`｜`design_status: CONFIRMED`（2026-10-01 主人批准，见 §11）
> 上游：`requirements/REQ-EXT-TRANSFER-001-multi-file-parallel-transfer.md`（§4 方案 B + M1–M6）、`tasks/REQ-EXT-TRANSFER-001/task.md`
> **本节点不写任何产品代码。** 契约变更已于 2026-10-01 获主人批准（§11）；`Client/protocol.h` / `Server/protocol.h` 的编辑在 development 节点按 §9 逐字批准稿进行，**超出批准稿的改动一律不允许**。

---

## 0. 本文件冻结的内容（批准卡的对象）

| # | 冻结项 | 位置 |
|---|---|---|
| F1 | 逐字节 wire format：transfer_id 在 14 个 PDU 的落位 | §2 |
| F2 | 新增 107/108 ABORT 的形态与语义 | §2.4 |
| F3 | 冻结面新增内容（**逐字**，§9 为批准稿） | §9 |
| F4 | 服务端状态模型 / 状态迁移 / 并发上限 / 空闲回收 / 断线语义 | §3 |
| F5 | 下载 round-robin 公平调度算法 | §4 |
| F6 | 客户端调度器、路由规则、UI 改造范围 | §5 |
| F7 | 测试矩阵与验收映射 | §6 |
| F8 | 兼容性不变式的证明方式 | §7 |

`design_status` 在主人批准后由本文件改为 `CONFIRMED`，批准记录写入 §11。

---

## 1. 设计裁决（goal §8 四个开放问题）

| # | 问题 | 裁决 | 理由 |
|---|---|---|---|
| D1 | ABORT 形态 | **107 请求 / 108 响应成对**；不新增其他类型 | 需要确认"槽位已释放"客户端才能安全复用/结束任务列表项；无响应的 fire-and-forget 无法区分"已释放"与"未知 id 被丢弃"，会让 UI 停在中间态 |
| D2 | transfer_id 落位 | **统一落在 `caData[48..55]`**，全族 13 个 PDU 一致；**唯 41 例外，落在 `caData[16..23]`** | 41 的 `caData[32..62]` 已被既有文件名字段占用（`Server/msghandler.cpp:1549`），不可复用。[48..55] 在其余 13 个 PDU 的 caData 中均空闲，且 `mkPDU()` 全量 `memset(pdu,0,len)`（`Server/protocol.cpp:19`、`Client/protocol.cpp:22`）→ 旧代码不写该区间时**必然为 0**，"0=旧路径"由构造保证，不靠代码路径保证 |
| D3 | 群族落位位置 | **`caData`，不追加进群信封（caMsg）** | 群信封路径的服务端有 `offset != payload.size()` 尾随字节拒绝（`Server/msghandler.cpp:2042`）。写 caData 则 101/102/105/106 的 caMsg 逐字节不变，旧 e2e 脚本（只写 caMsg）零改动 |
| D4 | 客户端块大小（现 4KB） | **本期不调**，保持 4096 | 本期目标是"多文件同时推进 + 小文件先完成"，不是单流吞吐。同时改块大小会让并发收益无法归因。列入 §10 后续项 |
| D5 | 空闲回收阈值 | **上传上下文 60s 无进展回收**；下载不单独回收，沿用既有 90s 僵尸连接清扫 | 客户端正常暂停/取消会显式发 107，60s 是给崩溃客户端的兜底；下载流有 socket 缓冲上界、且连接死亡即终止，独立阈值没有额外收益 |
| D6 | 并发上限的作用域 | **按方向各 3**：`MAX_PARALLEL_UPLOADS = 3`、`MAX_PARALLEL_DOWNLOADS = 3`；两个方向的计数器**相互独立**，且各自覆盖个人与群两条路径（上传预算个人+群共享，下载预算个人+群共享）。超出本地排队 | **主人 2026-10-01 裁决**（备选"合计 3"未采纳）。理由：上传不会被下载占住名额。代价：同一时刻最多 6 条流。见 §11 批准记录 |
| D7 | 服务端硬兜底范围 | 上传在 **INIT(29/101)** 拒绝（超 `MAX_PARALLEL_UPLOADS`）、下载在 **40/105 的响应**拒绝（超 `MAX_PARALLEL_DOWNLOADS`）；**拒绝即 `ret=false`，不回退到 id=0** | P4 + P7。两端同常量，守规矩的客户端永远不触发 |
| D8 | 服务端状态容器 | **统一 per-transfer 容器，id=0 落入同一容器**（不做双代码路径） | 见 §7 不变式 I3 的论证：id=0 的 wire 字节由 D2 保证与今日逐字节相同，而 6 套 e2e 直接打的就是服务端 id=0 全流程，漂移会被测到而非被假定。双路径在热路径上是更大的长期风险 |

---

## 2. 逐字节协议设计（F1 / F2）

### 2.1 通用规则

- **R1** `transfer_id` 为 `quint64`，**客户端生成**，连接内作用域，客户端级单调计数器，**从 1 起**。
- **R2** `0` = 保留值 = 旧协议单流路径。服务端读到 0 即走既有语义（不登记 per-transfer 上下文）。
- **R3** 分片路径 id 在**响应中回带**（30/32/41/42/43/102/106/108），客户端据此归属。
- **R4** 未知 id（非 0 且未登记）**拒绝且不回退为 0**（P7）：数据类请求回失败响应，控制类请求回错误码。连接保持存活。
- **R5** 同一连接内 id 由个人/群两条路径**共享同一计数器**，故 id 全局唯一，42/43 无需额外判别来源。
- **R6** 字节序 = 本机小端 `memcpy`，与既有全部字段一致。

### 2.2 落位表（新增字节以粗体标注在"新"列）

| PDU | 方向 | 既有 caData 占用 | 新增 | 新偏移常量 |
|---|---|---|---|---|
| 29 UPLOADFILE_INIT_REQUEST | C→S | [0..31] 名、[32..39] size、[40..43] resumeMagic | **(+)** | `TRANSFER_ID_OFFSET` |
| 30 UPLOADFILE_INIT_RESPOND | S→C | [0] ret、[32..39] offset、[40..43] magic | **(+)** | `TRANSFER_ID_OFFSET` |
| 31 UPLOADFILE_DATA_REQUEST | C→S | [0..7] chunkOffset、[40..43] resumeMagic | **(+)** | `TRANSFER_ID_OFFSET` |
| 32 UPLOADFILE_DATA_RESPOND | S→C | [0] ok、[8..15] next、[16] complete、[40..43] magic | **(+)** | `TRANSFER_ID_OFFSET` |
| 40 DOWNLOADFILE_REQUEST | C→S | [0..31] 名（**caMsg**=路径，不变） | **(+)** | `TRANSFER_ID_OFFSET` |
| 41 DOWNLOADFILE_RESPOND | S→C | [0] ret、[1..8] size、**[32..62] 名** | **(+)** | `DOWNLOAD_RESP_TRANSFER_ID_OFFSET` |
| 42 DOWNLOADFILE_DATA | S→C | caData 全空（caMsg=块） | **(+)** | `TRANSFER_ID_OFFSET` |
| 43 DOWNLOADFILE_FINISH | S→C | [0] success | **(+)** | `TRANSFER_ID_OFFSET` |
| 101 GROUP_FILE_UPLOAD_INIT_REQUEST | C→S | caData 全空（caMsg=信封+载荷，不变） | **(+)** | `TRANSFER_ID_OFFSET` |
| 102 GROUP_FILE_UPLOAD_INIT_RESPOND | S→C | caData 全空（caMsg=信封+code+body，不变） | **(+)** | `TRANSFER_ID_OFFSET` |
| 105 GROUP_FILE_DOWNLOAD_REQUEST | C→S | caData 全空（caMsg=信封+gfid，不变） | **(+)** | `TRANSFER_ID_OFFSET` |
| 106 GROUP_FILE_DOWNLOAD_RESPOND | S→C | caData 全空（caMsg=信封+code+body，不变） | **(+)** | `TRANSFER_ID_OFFSET` |
| 107 UPLOADFILE_ABORT_REQUEST | C→S | 新类型 | **(+)** | `TRANSFER_ID_OFFSET` |
| 108 UPLOADFILE_ABORT_RESPOND | S→C | 新类型 | **(+)** | `TRANSFER_ID_OFFSET` |

**不在范围内**：97/98（秒传预检查保持单飞；P2 明文）、全部非传输族 PDU、caMsg 的任何字节。

**冲突核验（实测，非推断）**：`Client`/`Server` 中除 41 的 `caData+32` 文件名字段外，传输族 caData 的最大使用偏移为 43（`UPLOAD_RESUME_MAGIC_OFFSET`）与 35（fastcheck 响应 caData[32..35]，属 98，不在范围）。`caData[44..63]` 在整个传输族中无既有写入者。

### 2.3 会话级语义（不新增字节）

- **S1** 29/101 的 INIT 现在携带 id：服务端在 id 对应槽位上建上下文。id=0 → 既有单会话槽（`m_uploadSession` 语义）。
- **S2** 31 的 `chunkOffset` 连续性、`resumeMagic`、长度边界校验**语义不变**，只把比较对象从"连接的单一会话"换成"该 id 的上下文"。
- **S3** 暂停 = 客户端停发 + 发 107；恢复 = 用**新 id** 重新走 29/101，服务端 `beginStaged` 由 `(owner|parent|display|size)` 派生**确定性 staging 路径**（`Server/resumableuploadsession.h:26`）→ 天然从已收字节继续，续传起点在 30/102 的 `resumeOffset`/`nextOffset` 回带。**无新协议**。
- **S4** 取消 = 客户端停发 + 发 107 + 删除本地任务。服务端对 107 的处置与暂停**完全相同**（释放槽位、会话置 `expired`、staging 保留）——暂停/取消的区别纯粹在客户端。

### 2.4 107/108 形态（F2 定稿）

```
107 UPLOADFILE_ABORT_REQUEST   caData[0..47] = 0        caMsg 长度 = 0
                               caData[48..55] = transfer_id (u64)
108 UPLOADFILE_ABORT_RESPOND   caData[0] = ret (bool)   caMsg 长度 = 0
                               caData[48..55] = transfer_id 回带 (u64)
```

- 服务端处置：查得上下文 → 释放（会话置 `expired`、清在途位、从并发计数与下载环中移除）→ `ret=true`；未登记 id → `ret=false`，**不建立任何状态**，连接存活。
- **幂等**：对同一 id 重复 107，第二次回 `ret=false`（已不存在的上下文），客户端以"任务已终结"处理，无副作用。
- 下载不可 abort（非目标 §2）：对下载 id 发 107 得到 `ret=false`。这是一个**已知缺口**（见 §10 R1）。

---

## 3. 服务端设计（F4）

### 3.1 状态容器

`MsgHandler`（每连接一份，`MyTcpSocket` 持有）与 `MyTcpSocket` 分工不变，仅把"单实例状态"换成按 id 索引的表：

```cpp
// MsgHandler
struct UploadCtx {
    quint64  id;                 // 0 = 旧单流槽
    ResumableUploadSession session;   // 既有类型，原样复用
    qint64   sessionId = -1;     // upload_session.session_id
    QString  expectedSha;        // 既有 m_uploadExpectedSha
    quint64  groupId = 0;        // 既有 m_groupUploadGroupId
    QString  groupDisplayName;   // 既有 m_groupUploadDisplayName
    bool     writeInFlight = false;   // 既有 m_uploadWriteInFlight
    qint64   lastProgressMs = 0; // 空闲回收用
    qint64   lastGroupFileId = 0;// 既有 m_lastGroupFileId
};
QHash<quint64, UploadCtx> m_uploads;   // key 0 = 旧单流路径

// MyTcpSocket
struct DownloadStream {
    quint64 id;
    QFile*  file = nullptr;
    qint64  totalSize = 0;
    qint64  sentSize = 0;
};
QHash<quint64, DownloadStream> m_downloads;
QList<quint64> m_downloadRing;   // round-robin 环（活跃顺序）
int     m_rrCursor = 0;
```

`MsgHandler::m_downloadCanonicalPath` **保持单成员不动**：`handlMsg` 里"解析→`startDownloadStream(src)`"是同一次调用的同步过程，连接内单线程处理，不存在交错。并发只影响"同时活跃几条流"，不影响这一次传递的正确性。

**不变式**：`m_uploads` 与 `m_downloads` 的 key 集合在连接内不相交（一个 id 只属于一条传输）。
**两个独立计数器（D6）**：上传并发数 = `activeUploadCount()`（遍历 `m_uploads`、**排除** `fastcheckPlaceholder` 占位后的条目数）`<= MAX_PARALLEL_UPLOADS`；下载 = `m_downloads.size() <= MAX_PARALLEL_DOWNLOADS`；两个方向互不占用。

> **repair 补充（AUD-1，2026-10-01 定稿）**：`m_uploads` 中的 **id=0 槽不参与上传名额计数**，当且仅当它是 97 秒传预检查留下的
> **纯暂存占位**（`UploadCtx.fastcheckPlaceholder = true`）；`activeUploadCount()` 逐条排除该标记，故并行客户端"每文件先 97 再 29"
> 不会为每个文件留下被计入的残留槽，第 3 路新个人上传不再被上限误拒。
> **该 id=0 槽不删**——旧客户端（REQ-EXT-FILE-002 形态）97 未命中后**不发 29、直接发 31 正文**，正是靠这个上下文接收分块；
> 首版"返回前清槽（`LegacySlotGuard`）"已被 `fastupload 24/46` 否决（删槽 → 31 走 `findUpload(0)==nullptr` → 整条 legacy 直传失败），
> 故**该机制不存在于代码中，也不得按此实现**。占位失效有两条路径：① 该槽收到正文——`handleUploadWriteFinished` 置
> `fastcheckPlaceholder=false`，转为普通活跃上传并重新计入名额；② 随后的 29（同 parent/display/size）采纳该暂存会话——
> `uploadfileinit` 在 `adoptedFastcheck` 时移除 id=0 槽。旧客户端真正在做的 id=0 单流上传（29→31→…）与普通槽一样计入名额
> （旧客户端只有一条，不会被拒）。

### 3.2 上传上下文状态机

```
        INIT(29/101) ret=true
   [无] ─────────────────────▶ Active ──DATA 每块接受──▶ Active(offset 前进)
                                │  │
              107 / 空闲 60s /  │  │  末块接受且 isComplete()
              断线              │  └──────────────▶ Committed（finalizeUploadOnComplete：转正+建条目+引用）
                                ▼                        │
                             Aborted ──(staging 保留)     └─▶ [无]（上下文释放）
                             [无]
```

| 迁移 | 触发 | 服务端动作 |
|---|---|---|
| ∅→Active | 29/101 且 `ret=true`、`m_uploads` 已有条目（含 id=0 旧槽）数 `< MAX_PARALLEL_UPLOADS` | `beginStaged` + `createUploadSession` + 登记 ctx |
| Active→Active | 31 通过全部既有校验 | 派发 `UploadWriteTask(conn, id, …)`；`writeInFlight=true`；成功后 `acceptWrittenChunk`、刷新 `lastProgressMs` |
| Active→Committed | 末块接受且 `isComplete()` | 既有 `finalizeUploadOnComplete()`（逐字保留其事务/复算/群引用逻辑），释放 ctx |
| Active→Aborted | 107 / 断线 / 空闲 60s | `finishUploadSession(sessionId,"expired")`；staging **不删**；释放 ctx |
| 拒绝 | INIT 时 `activeTransferCount() >= 3` / 未知 id 的 31 | 回 `ret=false` / `32{ok=false}`；**不建状态** |

**重要的既有语义原样保留**：`finalizeUploadOnComplete()` 的"服务端复算 SHA 后转正、摘要不符则 failed 且删 staging"、"群引用失败则撤销个人条目"两条路径**一字不改**，只是把 `this` 上的成员改为 `ctx` 的成员。

### 3.3 并发上限与排队

- 服务端：`INIT` 时上传方向已有条目数 `>= MAX_PARALLEL_UPLOADS` → `ret=false`（D7）；下载在 40/105 的响应里按 `m_downloads.size() >= MAX_PARALLEL_DOWNLOADS` 判定。**两个方向互不占用**（D6）。
- 客户端：本地队列（§5.2）保证守规矩时不触发。
- **不报错、不丢文件**（验收 5）：客户端把"被拒"当作"继续排队"的合法信号（重试间隔由槽位释放事件驱动，不做定时轮询）。

### 3.4 空闲回收（D5）

- `MyTcpSocket` 持一个 `QTimer m_reapTimer`（间隔 15s，连接级；参照既有 `MyTcpServer::m_sweepTimer` 30s 模式，`Server/mytcpserver.cpp:31-33`）。
- 每次 tick 扫描 `m_uploads`：`now - lastProgressMs > UPLOAD_IDLE_REAP_SEC*1000` 且 `!writeInFlight` → 走 Active→Aborted（会话 `expired`，staging 保留），记 `qCWarning(lcFs)` 审计行。
- `UPLOAD_IDLE_REAP_SEC = 60` 定义在 `Server/mytcpsocket.h`（服务端本地调优常量，**不入冻结面**）。
- 下载不参与回收（D5）；下载流的超时由既有 90s 僵尸连接清扫兜底。

> **repair 补充（N-1，2026-10-01）**：97 秒传预检查留下的 id=0 **纯占位**在**建槽时即写 `lastProgressMs`**，纳入上述回收口径。
> 此前该字段恒为 0，而回收条件要求 `lastProgressMs > 0`，故占位槽与其 `upload_session` 行**永不被回收**，只能等会话 3600s TTL
> 或下一次 97 覆盖（有界，但残留）。另：占位被回收时同步使"待 29 采纳"登记 `m_pendingFastcheck` 失效（按 `sessionId` 精确匹配），
> 避免 60s 后迟到的 29 去采纳一条已 `expired` 的会话行。回归锚点见 `parallel_e2e.py` **T-23**（复用 T-13 静默窗口）。

### 3.5 断线语义

`clientOffLine()` 中：清空 `m_downloads`（逐个 close+delete）、清空 `m_downloadRing`、对每个 `m_uploads` 走 Aborted 并清表，**不写 DB**（与今日一致：staging 与 `upload_session` 行留给既有 3600s TTL 收敛）。今日 `clientOffLine` 对单流不做 DB 写，本设计保持同一语义（最小改动）。

### 3.6 安全（P7）

- 入口门（`isPublicRequestType` / actor 认证 / 登录版本）**不变**；107/108 走 `authRejectPdu` 增加一条 107→108 的未认证拒绝映射。
- 群成员资格仍在 101/105 的现有判定点（`OperateDB::handleGroupFileOperation`）内，**不下移、不旁路**。
- 数据阶段只认本连接已登记的 id；未知 id 数据包 → `32{ok=false}`，不建立状态、不回退 0。
- 群族 id 只读 `caData[48..55]`，**不解码 caMsg**，不改变既有信封/尾随字节校验。

---

## 4. 下载 round-robin 公平调度（F5 / M4）

### 4.1 算法

连接级**单调度环 + 单定时器**（关键：不是每流一个定时器，否则等于把 N 个写者绑在一起，公平性无从谈起）。

```
sendNextChunk():                       // 每个调度 tick 只发 1 块
  if (m_downloadRing 为空) return
  if (bytesToWrite() + CHUNK + sizeof(PDU) > DOWNLOAD_WRITE_HIGH_WATER) {   // 既有 256KB 高水位
        scheduleNext(); return          // 背压：本轮不发，等 onBytesWritten
  }
  for (尝试至多 ring.size() 次):                       // 跳过已完成的流
      id = m_downloadRing[m_rrCursor]
      m_rrCursor = (m_rrCursor + 1) % m_downloadRing.size()
      st = m_downloads[id]
      if (st.remaining() == 0) { emit 43{id,success=true}; 摘环; continue }
      chunk = st.file->read(min(64KB, remaining))
      if (read 失败) { emit 43{id,success=false}; 摘环; continue }
      emit 42{id, chunk};  st.sentSize += chunk.size()
      scheduleNext(); return          // 一次只发一块，回到事件循环
```

- **公平性**：游标每轮前进，任何一条流在环上等待的轮数上界 = `ring.size()`。8MB 大文件与 64KB 小文件并存时，小文件在 `⌈64KB/64KB⌉ = 1` 轮内发完 → 验收 6。
- **新流加入**：追加到环尾，`m_rrCursor` 不回退。
- **摘环**：完成/读失败/被 abort 时从环中移除；若被摘元素下标 < `m_rrCursor`，游标减一（避免越界与跳流）。
- **背压**：沿用既有 `DOWNLOAD_WRITE_HIGH_WATER = 256KB` 与 `onBytesWritten` 续排（`Server/mytcpsocket.cpp:381-390`），语义不变。
- **控制消息不被饿死**（验收 4）：42 的发送受高水位门控，而心跳/聊天等 PDU 由 `seedMsg()` 直接 `write()`，排在已入队字节之后但**不会被下载块继续压后**（每个 tick 最多追加 64KB，且一旦触顶就停）。e2e 断言 PONG 往返延迟上界（§6 T-08）。

### 4.2 归档与边界

- 42/43 的 caData 全空 → 加 id 后两者字节数不变（`PDU` 定长头部不变），**不改变分帧**。
- 个人下载（40/41）与群下载（105/106）**共享同一环**，因为 42/43 通道本就共用；来源区分只对控制响应有意义，对数据流无意义。

---

## 5. 客户端设计（F6）

### 5.1 新组件

```
Client/transfertask.h/.cpp      新增（非冻结）
  struct TransferTask {  quint64 id;  Kind kind;  QString localPath, displayName;
                         qint64 totalBytes, doneBytes;  State state;  QString failReason;
                         Uploader* uploader;  quint64 groupId;  QObject* owner;  }
  class TransferManager : public QObject       // Client 持有，全局唯一
     quint64 allocId();                        // 单调，从 1 起
     bool    requestSlot(TransferTask*);       // active<3 立即启动；否则入队
     void    releaseSlot(quint64 id);          // 完成/失败/暂停/取消 → 启动队首
     void    abort(quint64 id);                // 发 107（暂停/取消共用）
     TransferTask* task(quint64 id);
     static quint64 transferIdOf(const PDU*);  // 按类型读 48 或 16
```

`transferIdOf()` 放在**新文件**而非 `protocol.h`，目的是把冻结面 diff 压到"常量 + 枚举值"（§9），不含任何函数。

### 5.2 并发预算与排队

- **两个队列、两个预算**（D6）：`MAX_PARALLEL_UPLOADS = 3` 管上传队列（个人+群共享），`MAX_PARALLEL_DOWNLOADS = 3` 管下载队列（个人+群共享）。上传完成不影响下载队列，反之亦然。
- 槽位释放事件（32 complete / 43 / 108 / 失败）→ 释放对应方向的槽 → 立即启动**同方向**队首。**不轮询、不 sleep**。
- 队列在 UI 上显示为"排队中"，完成后自动开始（验收 5）。

### 5.3 路由改造（`Client/client.cpp`)

| 响应 | 今日 | 改为 |
|---|---|---|
| 30 / 32 | `File::uploadfile` / `GroupFile::uploadingInstance()` | `TransferManager::task(id)`（id=0 走原路径） |
| 41 / 42 / 43 | `MsgHandler` 单槽 `m_recvFile` | 按 id 路由到该下载任务的接收文件 |
| 102 / 106 | 按信封 `group_id` 匹配窗口 | 按 id 匹配任务；`group_id` 保留为二次校验 |
| 108 | — | 更新任务状态（暂停/取消落定） |

- `GroupFile::s_uploading`（静态单例，串行根源之一）**删除**。
- `File::m_uploader` / `GroupFile::m_uploader`（单实例）**删除**，改由 `TransferManager` 持有。
- `Uploader` 增加 `quint64 m_transferId`，在 31 的 `caData[48..55]` 写入；`acknowledge()` 只接受本 id 的响应。

### 5.4 下载接收槽

`MsgHandler` 的 `m_recvFile/m_recvFilePath/m_recvFileName` 单槽 → 移入 `TransferManager` 的 `QHash<quint64, RecvCtx>`；`beginReceiving()` 的 `(N)` 递推去重逻辑**原样保留**（验收 3：并发下载各自独立落盘）。

### 5.5 UI（M6 + 用户追加项）

- `File` 与 `GroupFile`：`QFileDialog::getOpenFileNames()`（多选）→ 每个文件建一个任务。
- 新增**任务列表组件**（`QTableWidget`：文件名 / 进度条 / 状态 / 失败原因 / [暂停][继续][取消]）。**不再使用逐文件 `QMessageBox`**（并发时会互相打架）。
- 暂停/继续/取消按钮**仅对上传任务**启用；下载任务只显示进度（非目标）。
- 暂停 → `abort(id)` + 保留任务行（状态"已暂停"）；继续 → 以**新 id** 重走 29/101（S3），沿用任务行；取消 → `abort(id)` + 移除任务行。
- 失败任务行保留并带"重试"（验收 1：单文件失败不影响其他）。

### 5.6 SHA-256 移出 UI 线程（M5）

`Client/file.cpp:407-423` 与 `Client/groupfile.cpp:121-137` 的同步 1MiB 循环读 → 移入 `QtConcurrent::run` / `QThread`，完成后回主线程发 97/101。UI 在计算期间保持响应；任务行显示"计算摘要…"。**不改摘要算法与分段大小**（避免额外变量）。

---

## 6. 测试矩阵（F7）

### 6.1 回归（必须全绿，**零脚本改动**）

| 套件 | 基线用例数 | 说明 |
|---|---|---|
| entry_ops | 43 | 全部以 id=0 帧驱动服务端 → 即 D2/D8 的字节兼容证明 |
| dedup | 13 | |
| cross_tree | 41 | junction 双 cwd 形态 |
| sec001 | 48 | 入口门/越权/踢线 |
| fastupload | 46 | 97/98 不受影响 |
| groupfile | 45 | 101/102/105/106 的 caMsg 逐字节不变 |

运行形态按 `project-docs/evidence/E2E-RUNFORM.md`（cwd=dist、`py -3.13`、串行）。

### 6.2 新套件 `project-docs/evidence/REQ-EXT-TRANSFER-001/parallel_e2e.py`

| ID | 用例 | 断言 | 验收 |
|---|---|---|---|
| T-01 | 3 路个人上传并发（大小各异） | 3 条 entry 落盘，SHA 与源一致，无交叉确认 | 1 |
| T-02 | 上传方向第 4 个文件排队 | 客户端不发第 4 个 29；槽位释放后自动开始并成功 | 5 |
| T-03 | 服务端上传硬上限（裸 socket 发 id=1..4 的 29） | 前 3 个 ret=true；第 4 个 ret=false 且**不建状态**；随后 107(id=1) 后重发第 4 个成功 | 5 / P4 |
| T-18 | **两方向预算独立**（D6） | 3 上传 + 3 下载同时进行共 6 条流全部成功；第 4 个上传被拒/排队时，下载方向不受影响（反向同理） | 5 / D6 |
| T-04 | 32 响应归属 | 逐 id 的 nextOffset 序列单调、无跳变、无串扰 | M1 / P1 |
| T-05 | 个人上传 + 群上传跨窗口并发 | 两路 id 唯一、各自 finalize、群引用 group_file_id 非 0 | 2 |
| T-06 | 公平性：512KB × 1 + 1KB × 2 同时下载（**3 条流**，与 D6 上限一致） | 2 个小文件的 43 均早于大文件的 43 | 6 |
| T-07 | 并发下载落盘 | 路径互异（`(N)` 递推生效），各文件内容与源逐字节一致 | 3 |
| T-08 | 心跳不被饿死 | 3 路下载期间每 200ms 一发 50，PONG 往返全部 < 阈值（dev 实测标定后写入脚本常量） | 4 |
| T-09 | 个人上传 暂停→恢复 | 107 后 30 返回 `resumeOffset == 已收字节`；wire 总字节 ≈ 文件大小 + 增量（**断言前缀未重传**） | 7 |
| T-10 | 群上传 暂停→恢复 | 同上，经 101/102 路径 | 7 |
| T-11 | 取消上传 | 108 ret=true；`upload_session.status='expired'`；文件列表无新条目；staging 文件仍在盘上 | 8 |
| T-12 | 未知 id 的 107 / 31 | 108 ret=false / 32 ok=false；连接存活，后续正常请求成功 | P7 |
| T-13 | 空闲回收 | INIT 后静默 > 60s → 槽位释放（第 4 个 INIT 成功）；会话置 `expired` | D5 |
| T-14 | 断线清理（**代理断言**，F-2） | 2 上传 + 1 下载进行中断线 → **重连后新传输正常**（上下文随连接销毁，同连接内不可直接观测，故不写"已直接证明无残留"） | 9 |
| T-15 | id=0 直连用例 | 单流 29/31/41/42/43 全流程与今日一致（显式回归锚点） | 10 |
| T-16 | 边界 | 0 字节文件、大小为块大小整数倍的文件，均在并发下正确落定 | 1 |
| T-17 | 重复 id 冲突 | 对活跃 id 重发 INIT → ret=false，原上下文不被覆盖 | P7 |
| T-19 | **客户端握手保真**：3 个新个人上传并发（每文件按真实序列 97→98→29→30→31/32） | 3 路 INIT 均 ret=true 且 3 条 entry 落盘、SHA 与源一致（**repair 追加，AUD-1 回归锚点**：纯裸 29 的 T-01/T-03 看不见 97 占位） | 1 / 5 |
| T-20 | **107 × 在途写竞态**（31 发出后不等 32 ack 立即发 107，重复 6 轮） | 108 均 ret=true；`upload_session.confirmed_offset` == 暂存物理大小；续传 30 回带 resumeOffset == 暂存大小（**repair 追加，AUD-4 回归锚点**：无修复时 confirmed_offset 停留 INIT 值必失败） | 8 |
| T-21 | **98 回带请求标识**（AUD-3 服务端契约）：未命中与命中两条路径 | 98 逐字节回带 97 的 reqId，且 nameLen/名字段语义不变（`msg[:nameLen]` 仍是落定名，标识紧随其后） | 7 |
| T-22 | **客户端本地队列契约（静态）** | `pump()` 以 `MAX_PARALLEL_UPLOADS` 为闸且只启动 Queued；INIT 被拒 → 回队（`needFastcheck=false`，事件驱动，非 Failed）。**动态**"第 4 个 29 未发出"无本仓自动化宿主 → 见 §6.3 | 5 |

### 6.3 人工确认（NEEDS-HUMAN，一次性清单，不重试）

1. 多选选择器可一次选中 ≥3 个文件。
2. 任务列表逐行显示进度/状态/失败原因，三个任务同时刷新不卡。
3. 上传任务的暂停/继续/取消按钮行为与文案；下载任务无这些按钮。
4. 大文件选择 3 个时，SHA 计算期间窗口可拖动、可切换页面（M5 验证）。
5. 暂停后继续，任务行进度不回退到 0（走续传）。
6. **超限排队 → 槽位释放后自动开始**：选 ≥4 个文件，第 4 行显示"排队中"且**客户端不发第 4 个 29**；前 3 个之一完成/取消后，第 4 行自动开始并成功（F-3 动态面 / F-5；验收 5）。
7. **单文件失败隔离 + 单独重试**：3 个中 1 个失败（如目标目录越权）不影响其余两条；该行保留并带失败原因，点"重试"后可成功（验收 1 / F-5）。
8. **并发下载同名 `(N)` 递推**：同时下载两个同名文件到同一目录，落盘为 `name` 与 `name(1)`，两条互不覆盖（验收 3 / F-5）。
9. **断线后任务标记可重试**：传输中断开服务端 → 任务行标记失败并给出原因；服务端恢复后点"重试"可完成（验收 9 / F-5）。
10. **延长暂停下迟到 98 不误归属**（AUD-3 人工佐证）：任务 A 处于"计算摘要…"时立即暂停，再开始任务 B 的上传；观察 B 是否按自身结果走（命中则完成 / 未命中则进入传输中），A 不产生错误的"已完成"行。

---

## 7. 兼容性与不变式（F8）

| ID | 不变式 | 证明方式 |
|---|---|---|
| I1 | **旧客户端/脚本零改动可用** | 旧帧不写 `caData[44..63]`，`mkPDU` 全量清零 → id 恒为 0 → 旧语义。6 套件全绿即证明 |
| I2 | **旧语义零改动** | id=0 的 29/31 路径在服务端走同一份既有校验代码；43/32 的 caData 其余字节不变 |
| I3 | **id=0 的 wire 字节与今日逐字节相同** | 新增字段在 id=0 时值为 0；`mkPDU` 已把整块清零，代码不写该区间 → **输出字节由构造决定，与代码路径无关**（这是选 D8 统一容器而非双路径的依据） |
| I4 | **无 DB schema 变更 / 无迁移** | 全部为协议附加值；`upload_session` 复用既有列与状态枚举（新增使用 `expired`，该值已存在） |
| I5 | **分帧不变** | `sizeof(PDU)` 未变，无 PDU 长度变化（107/108 的 caMsg 长度为 0） |
| I6 | **冻结面 diff 有界** | 仅 §9 列出的行；审计逐字比对 |

---

## 8. 风险与缓解

| 风险 | 缓解 |
|---|---|
| 两端 `protocol.h` 人工同步漂移 | §9 给出逐字批准稿；integration 事实线 #2 逐字比对 diff |
| 41 用非统一偏移被实现者写错 | 独立常量名 `DOWNLOAD_RESP_TRANSFER_ID_OFFSET`；T-07 直接覆盖 41 路径 |
| 统一状态容器让 id=0 路径悄悄漂移 | T-15 显式锚点 + 6 套件（D8 的代价与对策） |
| ring 调度在摘环/越界上出错 | 摘环时游标修正规则写入 §4.1；T-06/T-14 覆盖活跃集变化 |
| 60s 回收误伤慢速但存活的上传 | 只看 `lastProgressMs`（每块接受即刷新）；客户端暂停会显式发 107，不依赖超时 |
| 上传中队列 + 服务端硬拒的交互放大失败 | 客户端把 ret=false 当"继续排队"，不做定时轮询；T-02/T-03 覆盖 |
| UI 大改引入回归 | 任务列表独立组件 + §6.3 人工清单 + 既有窗口操作路径不变 |
| e2e 新套件与既有套件争 5000 端口 | 按 RUNFORM 串行执行，同一时刻只跑一个 |

---

## 9. 冻结面批准稿（F3，逐字；批准后按此 diff 写入）

两个文件**各自**新增以下内容（文本逐字相同）；**除此外不得有其他改动**。

**(a) `enum ENUM_MSG_TYPE` 内，紧接 GROUP-003 的 106 之后、`ENUM_MSG_TYPE_MAX` 之前：**

```cpp
    // ===== REQ-EXT-TRANSFER-001：多文件有限并发（仅追加，旧编号 1-106 不变） =====
    ENUM_MSG_TYPE_UPLOADFILE_ABORT_REQUEST = 107,   // 上传任务释放请求（暂停/取消共用）
    ENUM_MSG_TYPE_UPLOADFILE_ABORT_RESPOND = 108,   // 上传任务释放响应
```

**(b) 文件末尾 `#endif` 之前，新增常量块：**

```cpp
// ===== REQ-EXT-TRANSFER-001：transfer_id 落位（两端逐字节一致，仅追加；0 = 旧单流路径） =====
static const int TRANSFER_ID_OFFSET = 48;                 // u64；29/30/31/32/40/42/43/101/102/105/106/107/108
static const int DOWNLOAD_RESP_TRANSFER_ID_OFFSET = 16;   // u64；仅 41（其 [32..62] 已被文件名字段占用）
static const quint64 TRANSFER_ID_LEGACY = 0;              // 保留值：旧协议单流路径
static const quint32 MAX_PARALLEL_UPLOADS = 3;            // 单连接并发上传上限（个人+群共享；客户端排队 + 服务端硬兜底）
static const quint32 MAX_PARALLEL_DOWNLOADS = 3;          // 单连接并发下载上限（个人+群共享；客户端排队 + 服务端硬兜底）
```

**批准稿范围外**：`caMsg`、既有字段偏移、既有枚举值、`Server/protocol.h` 独有的 `MAX_INBOUND_PDU_SIZE`/`MAX_PENDING_INBOUND_BYTES` 两行（保持原样，不因本需求变动）。

**批准后的自检命令**（供 development 与 audit 使用）：
`git diff --stat master...HEAD -- Client/protocol.h Server/protocol.h` 的行数应与批准稿完全一致。

---

## 10. 超出范围与已知缺口

| ID | 项 | 说明 |
|---|---|---|
| R1 | 下载不可 abort | 非目标（§2）。缺口：客户端放弃某下载后服务端仍会把它发完。有界（≤ 文件大小），且断线即止。列入后续需求 |
| R2 | 动态分片 / 单文件乱序分片 | 非目标；块大小保持 4096（D4） |
| R3 | 传输任务持久化 | 非目标；进程退出即丢失队列 |
| R4 | 跨连接并行 | 非目标（与单登录版本模型冲突，goal 已排除） |
| R5 | 限速 / 配额 | 非目标 |

---

## 11. 批准记录

批准卡以业务语言提交（协议加传输编号 + 107/108 放弃报文），随卡附 Q1。

| 日期 | 决定 | 范围 | 备注 |
|---|---|---|---|
| 2026-10-01 | **同意** | 按 §9 逐字批准稿改动 `Client/protocol.h` + `Server/protocol.h`；除批准稿外无其他改动 | 批准后 `design_status: CONFIRMED` |
| 2026-10-01 | Q1 裁决：**上传 3 + 下载 3（各算各的）** | 并发名额作用域 | 已回写 D6/§3.1/§3.3/§5.2/§9；`MAX_PARALLEL_TRANSFERS` 拆为 `MAX_PARALLEL_UPLOADS` + `MAX_PARALLEL_DOWNLOADS` |

**批准后的自检命令**（与 §9 一致，供 development 与 audit 使用）：
`git diff --stat master...HEAD -- Client/protocol.h Server/protocol.h` 的行数应与 §9 批准稿完全一致。

---

## 12. 下一节点（development）任务分解（预置，批准后执行）

| # | 任务 | 冻结面 |
|---|---|---|
| T1 | 按 §9 逐字写入两端 `protocol.h` | **是**（仅批准稿） |
| T2 | 服务端 per-transfer 上传上下文：`UploadCtx` 表、29/101/31 改造、107/108、上限、60s 回收、断线清理 | 否 |
| T3 | 服务端下载 ring 调度（§4.1） | 否 |
| T4 | 客户端 `TransferManager` + `transferIdOf()` + 路由改造（30/32/41/42/43/102/106/108） | 否 |
| T5 | 客户端 UI：多选 + 任务列表 + 暂停/继续/取消（§5.5） | 否 |
| T6 | SHA-256 移出 UI 线程（§5.6，M5） | 否 |
| T7 | 新套件 `parallel_e2e.py`（§6.2） | 否 |
| T8 | 6 套件回归 + 文档同步（architecture.md / interfaces.md / modules.md / code-map.md） | 否 |

development 退出条件：`pipeline/build.ps1` + `pipeline/test.ps1` + `pipeline/audit.ps1` 全绿，6 套件 + 新套件全绿，随后交 independent test（新会话）。
