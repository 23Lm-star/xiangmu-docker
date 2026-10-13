# 总体架构与技术选型

> 生成依据：代码实现 + `docs/agent.md` + `docs/PROJECT_IMPLEMENTATION.md`
> 状态：初版，与当前实现一致

---

## 1. 系统架构总览

```
┌─────────────────────────────────────────────────────────────────────┐
│  Client (Qt GUI + QTcpSocket)                                       │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────────────────┐ │
│  │ 登录/注册 │  │ 好友管理 │  │ 聊天界面 │  │ 文件管理           │ │
│  │ (client)  │  │ (friend) │  │ (chat)   │  │ (file/index/upload)│ │
│  └─────┬─────┘  └────┬─────┘  └────┬─────┘  └─────────┬──────────┘ │
│        └──────────────┴─────────────┴──────────────────┘            │
│                           │ MsgHandler 分发                          │
│                     ┌─────┴──────┐                                  │
│                     │  QTcpSocket │ (单连接)                         │
│                     └─────┬──────┘                                  │
└───────────────────────────┼──────────────────────────────────────────┘
                            │
                    ┌───────┴────────┐
                    │  自定义 PDU    │  (76B 包头 + 柔性数组)
                    │  TCP 二进制协议 │
                    └───────┬────────┘
                            │
┌───────────────────────────┼──────────────────────────────────────────┐
│  Server (Qt Console)      │                                          │
│  ┌────────────────────────┴──────────────────────┐                  │
│  │  MyTcpServer (QTcpServer, 单例)                │                  │
│  │  ├─ QThreadPool (max 8)                       │                  │
│  │  └─ QList<MyTcpSocket*> tcpsocketlist          │                  │
│  └────────────────────┬──────────────────────────┘                  │
│                       │                                              │
│  ┌────────────────────┴──────────────────────────┐                  │
│  │  MyTcpSocket (每个客户端连接)                   │                  │
│  │  ├─ recvmsg(): 粘包/半包处理                    │                  │
│  │  ├─ handlMsg(): 通过 MsgHandler 按类型分发      │                  │
│  │  └─ seedMsg(): 发送响应并 memfree               │                  │
│  └────────────────────┬──────────────────────────┘                  │
│                       │                                              │
│  ┌────────────────────┴──────────────────────────┐                  │
│  │  MsgHandler (每个Socket一个实例)                │                  │
│  │  注册/登录/好友/聊天/文件/验证码                 │                  │
│  └────────────────────┬──────────────────────────┘                  │
│                       │                                              │
│  ┌────────────────────┴──────────────────────────┐                  │
│  │  OperateDB (单例)                              │                  │
│  │  └── DatabaseConnectionPool (RAII 连接池)      │                  │
│  │       ├── MySQL: user_info / friend / message  │                  │
│  │       └── 按线程隔离，每线程上限 8 连接          │                  │
│  └────────────────────────────────────────────────┘                  │
│                                                                      │
│  ┌────────────────────────────────────────────────┐                  │
│  │  自研内存池 (TCMalloc 风格)                     │                  │
│  │  ThreadCache → CentralCache → PageCache        │                  │
│  │  对象上限 256KB, 页大小 8KB                     │                  │
│  └────────────────────────────────────────────────┘                  │
│                                                                      │
│  ┌────────────────────────────────────────────────┐                  │
│  │  文件系统                                      │                  │
│  │  {filesPath}/{用户名}/                          │                  │
│  │  支持: 上传/下载/删除/重命名/分享/移动/断点续传   │                  │
│  └────────────────────────────────────────────────┘                  │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 2. 技术栈

| 层次 | 技术选择 | 版本/说明 |
|------|----------|----------|
| 语言 | C++11/14 | `CONFIG += c++11` |
| GUI 框架 | Qt 5 Widgets | Qt 5.14.2 MinGW 64-bit |
| 网络 | QTcpSocket / QTcpServer | 自定义 TCP 二进制协议 |
| 数据库 | MySQL + QSqlDatabase | 通过自研连接池访问 |
| 构建 | qmake (.pro) | 客户端和服务端各一份 |
| 平台 | Windows (主) / Linux (可移植) | |

---

## 3. 通信协议

**自定义 PDU (Protocol Data Unit)**：

```
sizeof(PDU) = 76 字节 (uint=4B 平台)
┌──────────────┬──────────────┬──────────────┬──────────────────────┐
│ uiTotalLen   │  uiMsgLen    │  uiType      │  caData[64]          │
│ 4B           │  4B          │  4B          │  64B                 │
├──────────────┴──────────────┴──────────────┴──────────────────────┤
│ caMsg[] (柔性数组, 实际长度 = uiMsgLen)                             │
└────────────────────────────────────────────────────────────────────┘
```

- 枚举消息类型：编号 1–108（注册/登录/好友/聊天/文件/群/传输等；`ENUM_MSG_TYPE_MAX=99999` 为哨兵）
- `caData[64]` 复用规则：前32B 和 后32B 根据消息类型承载不同参数
- **传输族 transfer_id**（REQ-EXT-TRANSFER-001）：29/30/31/32/40/42/43/101/102/105/106/107/108 在
  `caData[48..55]` 携带 u64 连接内传输编号；仅 41 例外，落在 `caData[16..23]`（其 [32..62] 被文件名字段占用）。
  `0` = 保留值 = 旧协议单流路径
- 客户端：`malloc/free` 分配释放
- 服务端：`memalloc/memfree` 分配释放（走内存池）
- **禁止混用**两端分配/释放函数

---

## 4. 并发模型

| 角色 | 线程模型 |
|------|----------|
| 服务端监听 | 主线程（QApplication 事件循环） |
| 客户端连接处理 | QThreadPool（max 8），但实际 I/O 由 Socket 所在线程的事件循环驱动 |
| 文件上传 | `Uploader` 在 UI 线程事件循环内按块推进（单块在途，等 32 再发下一块） |
| 传输摘要 | 多文件上传的 SHA-256 经 `QtConcurrent::run` 在工作线程池计算（M5），完成后回主线程 |
| 服务端上传写盘 | `QThreadPool` + `UploadWriteTask`；每 transfer 单块在途位；暂存句柄由会话共享持有（不再每块 open/close，REQ-EXT-TRANSFER-003） |
| 数据库访问 | 连接池按 `QThread::currentThreadId()` 隔离，每线程上限 8 连接 |

---

## 5. 设计模式

| 模式 | 应用位置 | 备注 |
|------|----------|------|
| 单例 | `Client`, `Server` | Meyer's Singleton |
| 单例 | `MyTcpServer`, `OperateDB` | 同上 |
| 单例 | `Index` (Client 主界面) | 同上 |
| 单例 | `CentralCache`, `PageCache` (内存池) | 同上 |
| 单例 | `DatabaseConnectionPool` | 同上 |
| 工厂 | `mkPDU()` | 构造协议数据单元 |
| 对象池 | `ObjectPool<Span>` | 内存池 Span 复用 |
| 观察者 | Qt 信号槽 | 连接、数据到达、上传进度 |
| RAII | `PooledSqlConnection` | 析构时自动归还数据库连接 |

---

## 6. 低耦合可靠性与应用内链接扩展（ADR-011，规划）

```text
ClientHeartbeat ──(空 PING/PONG PDU)──> MyTcpSocket
       │                                      │
       └── 只经 Client 的 socket 生命周期接入     └── 不访问 DB/业务模块

File ──> ShareLinkDialog ──(创建/解析链接 PDU)──> MsgHandler
                                                     │
                                                     v
                                             ShareLinkService ──> OperateDB（仅 share_link 窄接口）
                                                     │
                                                     └── 解析成功复用既有下载流
```

- `ClientHeartbeat` 是唯一客户端心跳定时器；不依赖或修改 File/Friend/Chat/ShareFile。
- 分享链接为已登录用户使用的 `cloud-disk://share/<opaque-token>` 访问凭证；不是 HTTP 匿名公网访问。
- `ShareLinkService` 只依赖文件根目录校验和 `OperateDB` 的新增窄接口；禁止改变既有好友分享、资源所有权和默认访问权限。
- 详见 `decisions/ADR-011-20260729-低耦合心跳与应用内分享链接.md`。

---

## 7. 关键依赖关系

```
Client → protocol.h (共用协议定义)
       → MsgHandler (响应分发)
       → Index (主界面单例, 持有 File/Friend/Chat)
       → Uploader (独立线程上传)

Server → protocol.h (共用协议定义)
       → MyTcpServer (监听+连接管理)
       → MyTcpSocket (单连接处理)
       → ClientTask (线程池包装, QRunnable)
       → MsgHandler (请求处理逻辑)
       → OperateDB (数据库访问)
       → DatabaseConnectionPool (连接池)
       → ResumableUploadSession (断点续传)
       → 内存池 (ThreadCache/CentralCache/PageCache/ConCurrentAlloc)
```

## 8. 日志分级（REQ-LOG-007）

- 全部 213 处日志调用改走 `QLoggingCategory`：服务端 5 类（`xmu.net/db/fs/auth/app`，标识符 `lcNet/lcDb/lcFs/lcAuth/lcApp`），客户端单类（`xmu.cli`，标识符 `lcCli`）；分类文件 `Server/logging.{h,cpp}`、`Client/logging.{h,cpp}`。
- 默认安静：两个 `main.cpp` 在 `QT_LOGGING_RULES` 未设置时 `setFilterRules("xmu.*.debug=false")`；排查时设 `QT_LOGGING_RULES="xmu.*.debug=true"`（或单类 `xmu.db.debug=true`）打开。
- 级别映射：纯执行细节 `qCDebug`；操作失败/安全拒绝（SQL 失败、登录凭据不符、路径越权拒绝、下载打开失败等）`qCWarning`；配置/监听失败保持 `qCCritical`。日志语句一条未删，客户端 8 处绝对路径输出改为文件名或 `<path suppressed>`。
- 约束：`protocol.h` 为 frozen_path 未触碰；不引入第三方日志库。

---

## 9. 群聊子系统（REQ-EXT-GROUP-001 / 002）

### 9.1 定位与事实源

- 群功能以 `group_member` 的成员关系为唯一事实源：`group_info.status='active'` 且 `group_member.status='active'` 同时成立才允许群读写；解散、退出、被移除即时失效。
- 操作者身份一律取连接级 actor（SEC-001 `m_actorAuthenticated`），忽略客户端载荷中的自报字段；群消息发送载荷不含 sender 字段。
- 群协议是 §3 之上的独立段：GROUP-001 生命周期 PDU 60–87，GROUP-002 群消息 PDU 88–96。群请求共用 20B 信封（`version`+`envelope_length`+`request_id`+`group_id`，小端手写编解码），响应统一 `code + body`，与一对一聊天的 `Msg` 模型相互独立。

### 9.2 数据流

**发送（先持久化，后广播）**：

```
Client GroupChat ──88──> MyTcpSocket（actor 门；伪造事件 84/85/90 丢弃）
   └─> MsgHandler::groupRequest（先验信封/载荷边界）──> OperateDB::handleGroupMessageOperation
         事务：SELECT group_info FOR UPDATE（群行锁）
              → 成员门 group_member.status='active'
              → 幂等短路（同 client_msg_id 命中即返回原消息）
              → UPDATE group_info SET last_seq=LAST_INSERT_ID(last_seq+1)
              → INSERT group_message → COMMIT
   ──> 89 已持久化确认（仅发送者）
   ──> 提交成功后由 MsgHandler 触发 90 事件
         MyTcpServer::broadcastGroupMessage（持锁仅取连接快照，锁外投递）
           └─> MyTcpSocket::enqueueOutbound（每连接有界队列）→ pumpOutboundQueue → 在线成员
```

**拉取 / 已读 / 未读**：

```
打开群聊 ──> 95/96 未读摘要（按 actor 汇总其全部 active 群）
        ──> 91/92 以 last_read_seq 为游标前向补齐（mode=0, seq>cursor）
            上翻历史用后向翻页（mode=1, seq<=cursor，响应按 seq 升序回传）
阅读    ──> 93/94 单调推进 last_read_seq（钳制到 [last_read_seq, group_info.last_seq]）
未读数 = 服务端 COUNT(seq > last_read_seq AND seq <= group_info.last_seq)，客户端不可自报
```

- 系统消息（成员加入/退出/移除）在 GROUP-001 成员事务内以 `msg_type='system'` 追加进同一 seq 通道，因此群内时间线连续，同样经 90 与拉取下发。

### 9.3 关键不变式

- **先提交后广播**：广播只在 `db.commit()` 成功后触发（`GroupResult.broadcastMessage=true` 仅在成功提交路径置位；幂等重放置 `false`，DB 失败路径不置位）。提交失败 ⇒ 不广播；广播失败 ⇒ 已提交消息仍在库，由游标拉取补齐。
- **每连接有界出站队列**：取代旧 `reseed()`/`sendGroupEvent()` 持 `m_tcpsocketlistMutex` 同步 `write()` 的 fan-out。`broadcastGroupMessage` 持全局锁**只**复制目标连接指针快照，锁外逐连接 `enqueueOutbound`；队列上限 256 条 / 256KB（先到者），满则丢弃该连接实时投递并 `drops` 计数告警——慢成员既不阻塞 DB 事务也不影响其他成员，改由下次拉取补齐；连接断开在 `clientOffLine` 清空队列。
  - 范围提示：GROUP-001 的成员/邀请事件（84/85）仍走 `sendGroupEvent()` 的同步持锁 fan-out，只有群消息（90）走有界队列。
- **群内序号原子分配**：`group_info.last_seq` 在发送事务内以 `LAST_INSERT_ID(last_seq+1)` 递增，群行 `FOR UPDATE` 串行化同群发送；`uk_group_seq(group_id,seq)` 唯一索引兜底。`LAST_INSERT_ID()` 为连接级，由 `PooledSqlConnection` 保证同事务同连接。
- **幂等**：`uk_group_client_msg(group_id, client_msg_id)` + 事务内短路；同键重放返回原 `(seq, msg_id)` 且不重复广播，异发送者同键返回业务错误。
- 排序与分页游标一律以 `seq` 为准，`created_at`(DATETIME(3)) 仅用于展示排序稳定性。

---

## 10. 文件子系统（REQ-EXT-FILE-001 / 002）

### 10.1 定位与事实源

- 文件存储分为**逻辑条目层**（`user_file`：用户可见的目录树，`uk_loc(owner_name, parent_path, display_name)`）与**物理对象层**（`file_object`：按 `sha256` 内容寻址，`uk_sha256` 保证物理唯一），两层按引用计数关联；同一内容被多个条目引用时物理只存一份。
- 存储键是**owner 相对逻辑键**：`parent_path` 为 `/` 结尾的 owner 根内相对路径，与机器绝对路径解耦，跨工作树/跨根可复现。
- 操作者身份一律取连接级 actor（SEC-001），忽略客户端自报字段；所有路径经 actor 根内逻辑校验（`logicPathInActorRoot` / `normalizeParent`）。
- 协议为 §3 之上的独立段：FILE-002 新增秒传预检查 PDU 97/98（只追加，旧编号 1–96 不变），既有分块上传 `UPLOADFILE_INIT/DATA`(29–32) 语义零改动。

### 10.2 数据流

**秒传预检查（命中 → 零正文）**：

```
Client File（流式 SHA-256 + 32hex 请求键）──97 FASTCHECK──> MyTcpSocket（actor 门）
   └─> MsgHandler::uploadfilefastcheck
         形态校验 → actor 根逻辑路径校验 → 请求级幂等短路（client_request_id）
         → FileObjectStore::findObjectBySha 命中？
              是 → attachExistingObject（同事务：建/复活 user_file 条目 + refcount++）
                   ──> 98 命中响应（最终展示名；无任何数据块传输）
              否 → 登记/复用 upload_session，回续传起点 ──> 复用既有 Uploader 分块上传
```

**落盘转正（未命中路径）**：

```
分块上传全部完成 ──> MsgHandler::finalizeUploadOnComplete
   └─> FileObjectStore::commitUpload(staging, expectedSha)
         事务：FOR UPDATE 对象行（uk_sha256 兜底）
              → 复算 staging 的大小 + 摘要，双对照 expectedSha
                    不符 → 会话 failed，staging 清零，不建条目、不转正
                    相符 → 命中既有对象则丢弃 staging 并安置条目；否则转正新对象 → refcount++
```

### 10.3 关键不变式

- **物理唯一性由 DB 保证**：`file_object.uk_sha256` 为唯一键，并发相同内容由数据库兜底串行化，不依赖应用层约定。
- **命中零正文**：预检查命中路径只做元数据事务，不传输文件数据块；同名冲突以 `(1)` 递推生成展示名，幂等可重放。
- **转正双对照**：客户端声明的 `client_sha256` 仅作**预检元数据**，不作转正依据；转正以服务端对 staging 的**大小 + 摘要复算**为准（伪造摘要不命中、复算不符不转正）。
- **请求级幂等**：`upload_session.uk_req(owner_name, client_request_id)` 唯一，同一用户同一请求键只允许一条会话行；失败会话同键可重试。
- **附加式迁移 + 启动 fail-fast**：FILE-001 `2026092901`、FILE-002 `2026100102`（与 GROUP-002 `2026100101` 区分，避免同名使 fail-fast 失效）；`checkSchemaVersion()` 取四者 `qMax`，低版本拒绝监听，不运行时静默建表。
- **对象回收安全**：条目软删除后对象在 `refcount==0` 且无引用时转 `pending_reclaim`，不由删除路径直接物理删除。

---

## 11. 多文件并行传输子系统（REQ-EXT-TRANSFER-001）

### 11.1 定位与事实源

- 在 §10 的单流文件通道上做**协议级多路复用**：同一连接内并发推进多条传输，方向各 3 条
  （上传 3 + 下载 3，D6/Q1 主人裁决），超限排队不报错。
- 事实源：`project-docs/requirements/REQ-EXT-TRANSFER-001-multi-file-parallel-transfer.md`、
  `project-docs/tasks/REQ-EXT-TRANSFER-001/design.md`（§9 为冻结面逐字批准稿）。
- **只追加**：旧编号 1–106 与旧语义零改动；`transfer_id=0` 即旧单流路径（D2/M2），
  旧 e2e 脚本与旧编号帧逐字节不变。
- 传输族 `transfer_id` 落位见 §3；新增 `UPLOADFILE_ABORT` 107/108（上传任务释放，暂停/取消共用）。

### 11.2 数据流

**个人上传（首次：97 预检查 → 29 建槽 → 31/32）**：

```
Client File（后台 SHA-256，M5）──97 FASTCHECK（连接级单飞，不带 id）──> 服务端
   ├ 命中 → 98{hit=1} → 任务完成（零正文）
   └ 未命中 → 98{session_id,nextOffset}；服务端暂存该会话（供采纳）
Client TransferManager ──29 INIT(id)──> MsgHandler::uploadfileinit
   └ acquireUploadSlot(id)：id=0 重置旧槽；id≠0 查重（T-17）+ 上限（D7）→ 采纳 97 的会话
      → 30{ret,resumeOffset,id}
   └ Uploader ──31 DATA(id, offset)──> MyTcpSocket::handleUploadData → UploadWriteTask
        └ 32{ok,nextOffset,complete,id}；末块 → finalizeUploadOnComplete（释放上下文）
```

**群上传**：`101(id) → 102{code,hit,nextOffset,id} → 31/32`；群作用域随上下文携带，落定时建群引用。

**下载（round-robin）**：`40/105(id) → 41/106(带 id) → 42/43(带 id)`。
服务端连接级**单调度环 + 单定时器**：每个 tick 只发一块，游标轮转（§4.1），
与既有 256KB 出站高水位联动（心跳/控制消息不被饿死）。

**暂停/取消**：客户端停发 + `107(id)` → 服务端 `108{ret,id}`（会话置 `expired`、staging 保留）。
恢复 = 以**新 id** 重走 29/101，服务端由确定性 staging 路径继续（S3），不重传前缀。

**空闲回收**：连接级 15s 定时器扫描上传上下文，`lastProgressMs` 超 60s 且无写盘在途 → 按 Aborted 处置（D5）。

### 11.3 关键不变式

- **id=0 逐字节等价**：新增字段在 id=0 时为 0，且 `mkPDU` 全量清零 → 旧路径输出由构造决定（I1/I3）；
  6 套既有 e2e + 新套件全绿即证明。
- **未知 id 拒绝不回退**（P7）：数据类请求回失败响应，控制类回错误码，连接保持存活。
- **两方向预算独立**（D6）：`MAX_PARALLEL_UPLOADS` / `MAX_PARALLEL_DOWNLOADS` 各 3，互不占用；
  服务端硬兜底（超上限 29/101 → ret=false、40/105 → ret=false/-2107），客户端排队保证守规矩时不触发。
- **每 transfer 单块在途 + 顺序 offset**：不引入单文件乱序分片；`chunkOffset` 连续性/长度边界/magic 校验语义不变。
- **无 DB schema 变更**：全部为协议附加值；`upload_session.status='expired'` 复用既有枚举值。
- **落盘语义零改动**：下载 `(N)` 递推去重、落盘目录规则与单流时代一致，仅由单槽改按 id 分桶。


