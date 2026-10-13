# 模块划分、职责与依赖

> 生成依据：代码文件、构建文件（`.pro`）、头文件依赖
> 状态：初版

---

## 1. 客户端模块（`Client/`）

### 1.1 模块总览

| 模块 | 文件 | 职责 |
|------|------|------|
| **Client** | `client.{h,cpp}`, `client.ui` | 主窗口：登录/注册界面；TCP 连接管理；PDU 收发；验证码UI协调 |
| **MsgHandler** | `msghandler.{h,cpp}` | 响应分发：根据 `pdu->uiType` 调用各功能模块的处理方法 |
| **Index** | `index.{h,cpp}`, `index.ui` | 主界面（单例）：持有 File/Friend/Chat 子界面；登录成功后的框架 |
| **File** | `file.{h,cpp}`, `file.ui` | 文件管理界面：文件列表展示、上传/下载/删除/重命名/分享操作 |
| **Friend** | `friend.{h,cpp}`, `friend.ui` | 好友管理界面：好友列表、查找用户、添加/删除好友 |
| **Chat** | `chat.{h,cpp}`, `chat.ui` | 聊天界面：消息发送/展示；一对一即时通讯 |
| **OnlineUser** | `onlineuser.{h,cpp}`, `onlineuser.ui` | 在线用户列表界面 |
| **ShareFile** | `sharefile.{h,cpp}`, `sharefile.ui` | 文件分享：选择目标好友的界面 |
| **Uploader** | `uploader.{h,cpp}` | 单文件分块发送；分片 = `clamp(ceil(size/1024), 64 KiB, 1 MiB)`（REQ-EXT-TRANSFER-003 C1/C2，常量在 `uploader.h`，上限须 ≤ 服务端 `MAX_UPLOAD_DATA_CHUNK`）；携带 `transferId`（31 的 `caData[48..55]`），单块在途等 32 再发下一块 |
| **TransferManager / TransferTaskPanel** | `transfertask.{h,cpp}` | REQ-EXT-TRANSFER-001：文件级任务管理与调度（两方向各 3 名额）、按 `transfer_id` 路由 30/32/41/42/43/98/102/106/108、上传暂停/继续/取消（107）、SHA-256 后台线程（M5）、任务列表 UI（M6） |
| **UserInformation** | `userinformation.{h,cpp}`, `userinformation.ui` | 用户信息展示弹出窗口 |
| **SimpleCaptcha** | `simplecaptcha.{h,cpp}` | 验证码显示组件（本地绘制） |
| **ClientHeartbeat** | `clientheartbeat.{h,cpp}`（规划） | 唯一客户端心跳调度器：登录后发 PING、接收 PONG、3 次超时上报连接不健康；不依赖业务页面。 |
| **ShareLinkDialog** | `sharelinkdialog.{h,cpp,.ui}`（规划） | 应用内链接创建/解析对话框；仅经 File 的窄入口调用，不复用或改变好友分享窗口。 |
| **Protocol** | `protocol.{h,cpp}` | PDU 协议定义 + 构造：`malloc/free` 版本 |
| **Config** | `connect.config`, `config.qrc` | 连接配置（IP/端口/路径），嵌入 Qt 资源文件 |

### 1.2 模块间依赖关系

```
Client
 ├── MsgHandler (成员 m_mhptr)
 ├── protocol.h (PDU 结构)
 ├── userinformation.h (弹出窗口)
 └── Index (登录成功后切换)

Index (单例)
 ├── File
 ├── Friend
 ├── Chat
 └── sharefile (通过 File 触发)

Uploader
 ├── protocol.h (构造 PDU)
 └── QTcpSocket (通过信号回主线程发送)

TransferManager (单例，Client 持有)
 ├── Uploader (每个活跃上传一个实例)
 ├── File / GroupFile (入队、hashDue/fastcheckDue 信号)
 └── Client::seedMsg (发送 29/31/40/101/105/107)
```

> REQ-EXT-TRANSFER-001：上传/下载的**会话状态已从单例搬入任务**——`File`/`GroupFile` 不再各持
> 一个 `m_uploader`，`GroupFile::s_uploading` 静态单例已删除；`Client/MsgHandler` 不再持有下载接收单槽。

---

## 2. 服务端模块（`Server/`）

### 2.1 模块总览

| 模块 | 文件 | 职责 |
|------|------|------|
| **Server** | `server.{h,cpp}` | 服务端入口窗口：加载配置、启动 TCP 监听 |
| **MyTcpServer** | `mytcpserver.{h,cpp}` | TCP 监听器（单例）：管理连接列表、消息转发 |
| **MyTcpSocket** | `mytcpsocket.{h,cpp}` | 单客户端连接：PDU 粘包/半包处理、消息分发；REQ-EXT-TRANSFER-001 起持有 per-transfer 下载环（`m_downloads` + round-robin）与上传空闲回收定时器 |
| **ClientTask** | `clienttask.{h,cpp}` | 线程池任务包装：`QObject + QRunnable` |
| **MsgHandler** | `msghandler.{h,cpp}` | 请求处理：注册/登录/好友/聊天/文件/验证码 |
| **OperateDB** | `operatedb.{h,cpp}` | 数据库访问（单例）：全部 SQL 操作集中在此；仅追加分享链接持久化窄接口 |
| **ShareLinkService** | `sharelinkservice.{h,cpp}`（规划） | 生成、哈希、解析和校验应用内分享 token；只依赖 OperateDB 窄接口和文件根目录校验，不依赖好友分享链路。 |
| **DatabaseConnectionPool** | `databaseconnectionpool.{h,cpp}` | 数据库连接池（单例）：RAII 管理，按线程隔离 |
| **ResumableUploadSession** | `resumableuploadsession.{h,cpp}` | 断点续传会话管理：写入文件、偏移量连续性校验；REQ-EXT-TRANSFER-001 起由 `MsgHandler::UploadCtx` 按 `transfer_id` 持有 |
| **UploadWriteTask** | `uploadwritetask.{h,cpp}` | 分块写盘任务（QThreadPool）；回带 `transfer_id` 使回调按 id 归属 |
| **Protocol** | `protocol.{h,cpp}` | PDU 协议定义 + 构造：`memalloc/memfree` 版本 |
| **Config** | `connect.config`, `config.qrc` | 监听配置（IP/端口/文件路径），嵌入 Qt 资源文件 |

### 2.2 自研内存池模块

| 模块 | 文件 | 职责 |
|------|------|------|
| **Common** | `Common.h` | 常量、FreeList、SpanList、SizeClass、Span 定义 |
| **ThreadCache** | `ThreadCache.{h,cpp}` | TLS 无锁线程本地缓存：208 个 FreeList 桶 |
| **CentralCache** | `CentralCache.{h,cpp}` | 中心缓存（单例）：桶级互斥锁，Span 管理 |
| **PageCache** | `PageCache.{h,cpp}` | 页缓存（单例）：全局锁，Span 合并/切分 |
| **ConCurrentAlloc** | `ConCurrentAlloc.{h,cpp}` | 分配入口：根据大小路由到三级缓存或直接 PageCache |
| **RadixTreeHash** | `RadixTreeHash.h` | 基数树页映射（1/2/3 层按平台选择） |
| **ObjectPool** | `ObjectPool.h` | Span 对象池模板 |
| **memalloc** | `memalloc.{h,cpp}` | 分配包装层：MemHeader + 自动降级到 malloc |

### 2.3 模块间依赖关系

```
Server (入口)
 └── MyTcpServer::getinstance().listen()

MyTcpServer (单例)
 ├── MyTcpSocket* (tcpsocketlist)
 ├── ClientTask (线程池包装)
 └── QThreadPool (max 8)

MyTcpSocket (每个连接)
 ├── MsgHandler (m_MsgHandler)
 ├── protocol.h
 ├── OperateDB::instance() (clientOffLine)
 └── MyTcpServer::getinstance() (removeSocket)

MsgHandler
 ├── OperateDB::instance()
 ├── ResumableUploadSession
 ├── protocol.h
 └── Server::getinstance() (文件路径)

OperateDB (单例)
 └── DatabaseConnectionPool::instance()

DatabaseConnectionPool (单例)
 └── QSqlDatabase / QSqlQuery
```

### 2.4 内存池依赖

```
ConCurrentAlloc (入口)
 ├── ThreadCache (TLS, 无锁)
 │    └── CentralCache::FetchRangeObj / ReleaseListToSpans
 ├── CentralCache (单例, 桶级锁)
 │    └── PageCache::NewSpan / ReleaseSpanToPageCache
 └── PageCache (单例, 全局锁)
      ├── RadixTreeHash (地址→Span 映射)
      └── ObjectPool (Span 对象复用)

memalloc (包装层)
 ├── ConCurrentAlloc / ConCurrentFree
 └── malloc / free (降级路径)
```

---

## 3. 配置模块

| 文件 | 归属 | 读取方式 | 内容格式 |
|------|------|----------|----------|
| `connect.config` | Client 和 Server 各一份 | `:/connect.config`（Qt 资源文件） | 3 行：IP / 端口 / 文件根路径 |
| `config.qrc` | Client | 资源文件索引 | 引用 `connect.config` + 图标 |
| `config.qrc` | Server | 资源文件索引 | 引用 `connect.config` |
| `.gitignore` | 仓库根 | Git 过滤 | 忽略 `build-*/` 和 `Client/_github_bundle_repo/` |

> **注意**：`Server/Server.pro` 中 `config.qrc` 被引用了 3 次（`RESOURCES += config.qrc` 重复），此**无编译错误**但属于冗余，应当在重构时清理。

---

## 4. 构建模块

| 构建目标 | 构建文件 | Qt 模块依赖 | 编译标准 |
|----------|----------|-------------|----------|
| Client | `Client/Client.pro` | `core gui network` | C++11, UTF-8 源码 |
| Server | `Server/Server.pro` | `core gui network sql` | C++11 |

**构建命令**：
```bash
cd Client && qmake Client.pro && make
cd Server && qmake Server.pro && make
```

**当前开发环境**：Qt 5.14.2 MinGW 64-bit

---

## 5. 群聊模块（REQ-EXT-GROUP-001 / 002 / 003 / 004）

### 5.1 客户端

| 模块 | 文件 | 职责 |
|------|------|------|
| **Group** | `group.{h,cpp}` | 群聊模块主界面：我的群聊列表、待处理邀请、群聊窗口、成员列表与群主/管理员操作区（建群/邀请/接受/拒绝/退出/移除/任免/转让/解散/摘要查询），进入模块后按需查询。GROUP-002 起兼群消息窗口入口与未读徽标，把 89/92/94 响应路由到对应群窗口（`handleMessageResponse`），把 90 事件按 `group_id` 路由（`handleMessageEvent`） |
| **GroupChat** | `groupchat.{h,cpp}` | 单群聊天窗口（可靠文本消息）：发送 88 → 89 已持久化确认（发送中/已发送/失败重试，`QUuid` 作幂等键）；接收 90 在线事件，并按 `seq` 与拉取结果去重渲染；以 91/92 前向补齐未读、上翻历史走后向翻页；已读经 93/94 单调推进。系统消息居中渲染。GROUP-004 起本窗口只是读状态的读写者：推进统一走 `onMessageArrived`（实时/自己消息/拉取同一入口），上报走 `scheduleReadReport`（400ms 合并、批次结束与关闭前立即落地） |
| **GroupChatState** | `groupchatstate.{h}` | GROUP-004 新增。群读状态的纯逻辑（无 UI/网络/QObject）：`waterSeq`（群水位，乐观推进）与 `readCursor`（连续前缀末尾，悲观推进）分列，超前到达先入乱序缓冲。由 `Client` 按 `group_id` 持有，登录成功时建立锚点、重登时清空。头文件刻意不依赖 QSet/Qt 库，以便被 `cursor_probe.cpp` 直接编译断言 |
| **Protocol** | `protocol.{h,cpp}` | 群信封编解码（`groupAppendU16/U32/U64/String`、`groupRead*`）与 60–96 枚举定义；`GROUP_PROTOCOL_VERSION=1`、`GROUP_ENVELOPE_LENGTH=20` |

- `Client.pro` 已纳入 `group.{h,cpp}`、`groupchat.{h,cpp}`；`Client::handlMsg` 将群响应（60–87 与 95/96）分发到 `Group`，将消息类响应（89/92/94）与事件（90）分发到 `Group`/`GroupChat`。
- GROUP-004：`Client` 持有会话级群读状态（`Client::groupState`），登录成功后取一次全部群的未读锚点（`Group::initializeSession`）；95/96 与 90 的路由是应用级（`Client::handlMsg`），不依赖群聊页是否显示。

### 5.2 服务端

| 模块 | 文件 | 职责 |
|------|------|------|
| **MyTcpSocket** | `mytcpsocket.{h,cpp}` | actor 门与事件伪造拒绝（客户端发入的 84/85/90 一律丢弃并记审计日志）；**每连接有界出站队列** `enqueueOutbound`（上限 256 条 / 256KB，先到者，满则丢弃并计数）、`pumpOutboundQueue`（256KB 高水位停写，`onBytesWritten` 回落续冲），断线在 `clientOffLine` 清空 |
| **MyTcpServer** | `mytcpserver.{h,cpp}` | `broadcastGroupMessage`：持 `m_tcpsocketlistMutex` 仅收集目标连接快照，释放锁后锁外逐连接入队（绝不持全局锁写 socket）；GROUP-001 的 `sendGroupEvent`（成员/邀请事件）保持同步 fan-out 不变 |
| **MsgHandler** | `msghandler.{h,cpp}` | `groupRequest`：群请求统一入口，先验 20B 信封（版本/长度/字段边界）再经 `groupOperation` 分派，经 `groupRespondType` 统一 `code+body` 响应；DB 提交成功后触发 90 广播（`code==0 && broadcastMessage`） |
| **OperateDB** | `operatedb.{h,cpp}` | `handleGroupOperation`（生命周期）与 `handleGroupMessageOperation`（发送/拉取/已读/未读）；`GroupOperation`/`GroupRequest`/`GroupMessageRecord`/`GroupResult`；群消息事务含序号原子分配、幂等短路、游标分页、未读计数；`kGroupMessageSchemaVersion=2026100101`，`checkSchemaVersion()` 取文件资产/群/群消息三者 `qMax` |
| **Protocol** | `protocol.{h,cpp}` | 群信封编解码 + 60–96 枚举（与客户端同值同名） |
| **迁移** | `migrate_ext_group_001.sql`、`migrate_ext_group_002.sql`（+ rollback） | 群表（`group_info`/`group_member`/`group_invitation`）与群消息表（`group_info.last_seq`、`group_message`）；幂等、可重复执行，服务端 fail-fast 不静默建表 |

### 5.3 依赖关系

```
Client  Index → Group → GroupChat
                 └── protocol.h（群信封 + 60–96）

Server  MyTcpSocket（actor 门 / 出站队列）
          └── MsgHandler::groupRequest ──> OperateDB::handleGroupMessageOperation
                                              └── DatabaseConnectionPool（RAII、线程隔离）
          ↖ MyTcpServer::broadcastGroupMessage（锁内快照 → 锁外 enqueueOutbound）
```

---

## 6. 文件子系统（REQ-EXT-FILE-001 / 002）

### 6.1 客户端

| 模块 | 文件 | 职责 |
|------|------|------|
| **File** | `file.{h,cpp}`, `file.ui` | 文件管理界面；上传入口在发起前**流式计算 SHA-256**（1MiB 窗口，内存 O(1)）并生成 32hex 请求键，先发 FASTCHECK 预检查；命中则提示并刷新列表（零正文上传），未命中复用既有 `Uploader` 分块上传 |
| **MsgHandler (Client)** | `msghandler.{h,cpp}` | 新增 `UPLOADFILE_FASTCHECK_RESPOND`(98) 分发；命中时携带服务端返回的最终展示名 |

### 6.2 服务端

| 模块 | 文件 | 职责 |
|------|------|------|
| **MsgHandler** | `msghandler.{h,cpp}` | `uploadfilefastcheck()`：请求形态校验 → actor 根逻辑路径校验 → 请求级幂等短路（`client_request_id`）→ 摘要预检查命中（`attachExistingObject`，**零正文**）；未命中登记暂存会话并回续传起点。`finalizeUploadOnComplete()` 落盘时**复算大小 + 摘要**双对照，不符不转正、会话置 `failed` |
| **FileObjectStore** | `fileobjectstore.{h,cpp}` | 内容寻址对象仓：`hashStaging`/`commitUpload(..., expectedSha)`/`attachEntry`/`attachExistingObject`/`findObjectBySha`/`releaseEntry`/`normalizeParent`/`logicPathInActorRoot`；全部写入在**同一事务**内完成，命中时丢弃 staging，物理只留一份 |
| **OperateDB** | `operatedb.{h,cpp}` | 文件资产与上传会话 SQL：`createUploadSession`/`updateUploadOffset`/`finishUploadSession` + FILE-002 追加 `lookupUploadSessionByRequestId`/`restartUploadSession`/`insertCompletedUploadSession`；`isLogicKeyForm`/`normalizeLogicKeys` 逻辑键判别与一次性转换；`checkSchemaVersion()` 取文件资产/群/群消息/秒传四者 `qMax` |
| **ResumableUploadSession** | `resumableuploadsession.{h,cpp}` | 断点续传会话管理：确定性 staging 路径、偏移量连续性校验 |
| **Protocol** | `protocol.{h,cpp}` | 追加 `UPLOADFILE_FASTCHECK_REQUEST=97`/`_RESPOND=98` 与 `FASTCHECK_*` 偏移/magic 常量（与客户端同值同名） |
| **迁移** | `migrate_ext_file_001.sql`、`migrate_ext_file_002.sql`（+ rollback） | FILE-001：`schema_version`/`file_object`/`user_file`/`upload_session`/`file_migration_map` 五表；FILE-002：附加式给 `upload_session` 加 `client_request_id`/`file_id` + `uk_req` |

### 6.3 依赖关系

```
Client  File ──97──> protocol.h（FASTCHECK 布局）
          └──(未命中) Uploader → 既有分块上传（29/30/31/32）
          ↖ MsgHandler ←98─ 命中/未命中结果

Server  MyTcpSocket（actor 门）
          └── MsgHandler::uploadfilefastcheck / finalizeUploadOnComplete
                ├── OperateDB（upload_session / user_file / file_object）
                └── FileObjectStore（对象仓：暂存→复算→转正/命中安置）
                      └── DatabaseConnectionPool（RAII、线程隔离）
```


---

## 7. 多文件并行传输子系统（REQ-EXT-TRANSFER-001）

### 7.1 客户端

| 模块 | 文件 | 职责 |
|------|------|------|
| **TransferManager** | `transfertask.{h,cpp}`（新增） | 单例任务表：`QHash<key,Task>` + `id→key` 映射；两方向独立名额（上传 3 / 下载 3，D6），`pump()` 在槽位释放时启动同方向队首；按 `transfer_id` 路由 30/32/41/42/43/98/102/106/108；`pause/resume/cancel/retry`（仅上传）；97 预检查连接级单飞（`pumpFastcheck`，因 97 不带 id 且服务端旧槽单份） |
| **TransferTaskPanel** | `transfertask.{h,cpp}`（新增） | 文件级任务列表（`QTableWidget`：文件名/方向/进度/状态/失败原因 + 暂停/继续/取消/重试），替代并发下互相打架的逐文件 `QMessageBox`（M6） |
| **File** | `file.{h,cpp}` | 多选 `getOpenFileNames`；**保留**流式 SHA-256（1MiB）与 97 预检查（P4/P5）——任务入队后由 `hashDue`/`fastcheckDue` 信号驱动本窗口计算并发送；下载支持多选并发；新增"传输列表"入口（程序化按钮，不改 .ui） |
| **GroupFile** | `groupfile.{h,cpp}` | 多选上传（SHA 由 `TransferManager` 在后台线程算，M5）与下载；删除 `m_uploader` 与静态 `s_uploading` 单例；102/106 交由 `TransferManager` 路由 |
| **MsgHandler (Client)** | `msghandler.{h,cpp}` | 移除下载接收单槽与旧上传响应处理（30/32/41/42/43/98/102/106 已改由 `TransferManager` 承载） |
| **Uploader** | `uploader.{h,cpp}` | 构造时接收 `transferId`，31 的 `caData[48..55]` 写入；`acknowledge()` 只接受本 id 的响应（由 `TransferManager` 保证） |

### 7.2 服务端

| 模块 | 文件 | 职责 |
|------|------|------|
| **MsgHandler** | `msghandler.{h,cpp}` | `UploadCtx` 表（`QHash<quint64,UploadCtx>`，id=0 = 旧单流槽）：29/101 建槽（`acquireUploadSlot` 判上限 D7 与重复 id T-17）、31 校验逐 transfer 化、`abortUpload`（107 处置：会话 `expired`、staging 保留）、`reapIdleUploads`（D5 60s 回收）、`finalizeUploadOnComplete` 释放上下文；97 未命中暂存会话供随后 29 采纳（`m_pendingFastcheck`）；下载上限经 `m_downloadSlotAvailable` 在 40/105 拒绝 |
| **MyTcpSocket** | `mytcpsocket.{h,cpp}` | 落位读取请求 id（29/31/40/101/105/107）；`handleUploadData`/`handleUploadWriteFinished` 按 id 取上下文（未知 id 拒绝不回退）；`handleUploadAbort` 107→108；**下载环**：`m_downloads` + `m_downloadRing` + `m_rrCursor`，每 tick 一块 round-robin，与 256KB 高水位联动；连接级 15s 回收定时器；`clientOffLine` 清空两表不写 DB |
| **UploadWriteTask** | `uploadwritetask.{h,cpp}` | 增加 `transferId`，经 `MyTcpServer` 回调按 id 归属 |
| **MyTcpServer** | `mytcpserver.{h,cpp}` | `handleUploadWriteFinished` 增加 `transferId` 形参并透传 |

### 7.3 依赖关系

```
Client  File/GroupFile ──enqueue──> TransferManager ──29/31/40/101/105/107──> Client::seedMsg
                ↑ hashDue/fastcheckDue        ↑ 30/32/41/42/43/98/102/106/108（Client::handlMsg 按 id 分发）
        Uploader（每活跃上传一个，带 transferId）

Server  MyTcpSocket（落位读 id；下载环；107/108；15s 回收）
          └── MsgHandler::UploadCtx 表（29/31/101/107/回收）
                └── UploadWriteTask ──(connectionId, transferId)──> MyTcpServer ──> MyTcpSocket
```

### 7.4 不变式与缺口

- id=0 走同一容器但字节与语义与旧实现逐字节等价（D8/I1/I3）；旧 e2e 直接打 id=0 全流程。
- 每 transfer 保持单块在途 + 顺序 offset，不引入乱序分片。
- 已知缺口：**下载不可 abort**（非目标）——客户端放弃某下载后服务端仍发完，有界且断线即止（design §10 R1）。
