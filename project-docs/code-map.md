# 代码地图

> 维护者：项目架构师
> 最后更新：2026-09-29
> 本文件是项目的结构化索引，所有 Agent 了解项目结构时先读此文件。

---

## 1. 项目结构树

```
E:\qt\xiangmu/
├── .gitignore                           # Git 忽略规则（已扩展覆盖 Qt 构建产物）
├── project-docs/                        # 项目唯一事实来源（治理文档）
├── AGENTS.md                            # AI 工程协作增强规则（用户级）
│
├── Client/                              # 客户端（Qt 5 Widgets GUI）
│   ├── Client.pro                       # qmake 构建文件（core gui network concurrent）
│   ├── main.cpp                         # 入口（设默认日志过滤规则）
│   ├── logging.h / logging.cpp          # 日志分类：lcCli(xmu.cli)，安静默认+QT_LOGGING_RULES 开关（REQ-LOG-007）
│   ├── protocol.h / protocol.cpp        # PDU 协议定义 + malloc/free 构造
│   ├── client.h / client.cpp / .ui      # 主窗口：登录/注册、TCP 连接、PDU 收发
│   ├── msghandler.{h,cpp}              # 响应分发：按 uiType 路由到各模块处理
│   ├── index.h / index.cpp / .ui        # 主界面框架（单例），持有各子界面
│   ├── file.h / file.cpp / .ui          # 文件管理 UI：列表/上传/下载/删除/重命名/分享
│   ├── friend.h / friend.cpp / .ui      # 好友管理 UI：列表/查找/添加/删除
│   ├── chat.h / chat.cpp / .ui          # 聊天 UI：消息发送/展示
│   ├── onlineuser.h / .cpp / .ui        # 在线用户列表 UI
│   ├── sharefile.h / .cpp / .ui         # 既有好友文件分享：选择目标好友 UI（保护区）
│   ├── clientheartbeat.{h,cpp}           # ⚠️ 延后：基础心跳已内嵌于 client.cpp（PING/PONG），组件化延后至下迭代
│   ├── sharelinkdialog.{h,cpp,.ui}       # ⚠️ 待创建：分享链接展示弹窗（R18 fix-spec 任务 C）
│   ├── uploader.h / uploader.cpp        # 单文件分块上传（带 transferId，单块在途）
│   ├── transfertask.{h,cpp}             # REQ-EXT-TRANSFER-001：并行传输任务管理与列表（TransferManager/TransferTaskPanel）
│   ├── userinformation.h / .cpp / .ui   # 用户信息弹窗
│   ├── simplecaptcha.h / .cpp           # 验证码组件（本地绘制）
│   ├── connect.config                   # 连接配置（IP/端口/文件路径等）
│   ├── config.qrc                       # Qt 资源文件索引
│   └── ui_*.h                           # 自动生成的 UI 编译产物（不入库）
│
├── Server/                              # 服务端（Qt Console）
│   ├── Server.pro                       # qmake 构建文件（core gui network sql）
│   ├── main.cpp                         # 入口（设默认日志过滤规则）
│   ├── logging.h / logging.cpp          # 日志分类：lcNet/lcDb/lcFs/lcAuth/lcApp(xmu.*)，安静默认+QT_LOGGING_RULES 开关（REQ-LOG-007）
│   ├── protocol.h / protocol.cpp        # PDU 协议定义 + memalloc/memfree 构造
│   ├── server.h / server.cpp            # 服务端入口窗口：加载配置、启动监听
│   ├── mytcpserver.h / .cpp             # TCP 监听器（单例），管理连接列表
│   ├── mytcpocket.h / .cpp              # 单连接处理：粘包/半包/消息分发
│   ├── clienttask.h / .cpp              # ✅ 已删除（BUG-POOL-005）
│   ├── msghandler.h / .cpp              # 请求处理：注册/登录/好友/聊天/文件/验证码
│   ├── operatedb.h / .cpp               # 数据库操作集（单例）
│   ├── databaseconnectionpool.h / .cpp  # 数据库连接池（RAII，按线程隔离）
│   ├── resumableuploadsession.h / .cpp  # 断点续传会话管理（REQ-EXT-TRANSFER-001 起按 transfer_id 持有于 UploadCtx）
│   ├── uploadwritetask.h / .cpp         # 分块写盘任务（回带 transferId）
│   ├── sharelinkservice.{h,cpp}          # ⚠️ 待创建：安全 token 生成/哈希/路径校验（R18 fix-spec 任务 B 3.2）
│   ├── connect.config
│   ├── config.qrc                       # Qt 资源文件索引
│   ├── migrate_publish_links.sql         # 分享链接表（⚠️ 需迁移为 share_link，见 BUG-ARC-PUBLINK-001）
│   │
│   ├── 自研内存池模块：
│   │   ├── Common.h                     # 常量、FreeList、SpanList、SizeClass
│   │   ├── ThreadCache.{h,cpp}          # TLS 无锁线程本地缓存（208 桶）
│   │   ├── CentralCache.{h,cpp}         # 中心缓存（单例，桶级锁）
│   │   ├── PageCache.{h,cpp}            # 页缓存（单例，全局锁）
│   │   ├── ConCurrentAlloc.{h,cpp}      # 分配入口
│   │   ├── RadixTreeHash.h              # 地址→Span 映射
│   │   ├── ObjectPool.h                 # Span 对象池
│   │   └── memalloc.{h,cpp}             # 分配包装层
│   │
│   ├── release/                         # 构建产物（不入库）
│   └── qtbase/                          # Qt 5.14.2 源代码（配置检测、示例等）
│
├── project-master/                      # ⚠️ 已删除的旧项目遗留目录（git diff D）
│
└── 其他根目录文件：
    ├── package.json                     # （无关）
    └── skills-lock.json                 # （无关）
```

---

## 2. 模块依赖图

```
Client 依赖关系：
  Client (main) → 单例 Client::getinstance()
    ├── QTcpSocket (TCP 连接)
    ├── MsgHandler → 所有 handl*() 响应分发函数
    ├── protocol.h / protocol.cpp (malloc/free 构包)
    ├── SimpleCaptcha (验证码组件)
    └── login 成功后切换 → Index (单例主界面)
         ├── File (文件管理 UI + Uploader 独立线程)
         ├── Friend (好友管理 UI)
         ├── Chat (聊天 UI)
         ├── OnlineUser (在线用户 UI)
         ├── ShareFile (分享 UI)
         └── UserInformation (用户信息弹窗)

Server 依赖关系：
  Server (main) → 加载配置 → MyTcpServer::getinstance().listen()
    └── QList<MyTcpSocket*> tcpsocketlist (⚠️ 已加 QMutex 保护)
        ├── MsgHandler → 所有请求处理函数
        │   ├── OperateDB (单例, 预编译 SQL)
        │   └── ResumableUploadSession (断点续传)
        ├── protocol.h / protocol.cpp (memalloc/memfree 构包)
        ├── DatabaseConnectionPool (单例, RAII 连接池)
        └── 内存池: memalloc → ConCurrentAlloc → ThreadCache → CentralCache → PageCache
```

---

## 3. 关键接口签名

| 接口 | 位置 | 签名 |
|------|------|------|
| PDU 构造 | `protocol.h` | `PDU* mkPDU(uint uiMsgLen=0)` |
| PDU 发送 | `client.cpp` / `mytcpsocket.cpp` | `void seedMsg(PDU* pdu)` |
| 粘包处理 | `client.cpp` / `mytcpsocket.cpp` | `void recvmsg()` (readyRead 槽) |
| 请求分发 | Server `mytcpsocket.cpp` | `PDU* handlMsg(PDU* pdu)` |
| 响应分发 | Client `msghandler.cpp` | `void handlMsg(PDU* &pdu)` |
| 数据库查询 | `operatedb.cpp` | 各 `handle*()` 方法，已全部迁移到 `prepare()`+`bindValue()` |
| 连接池获取 | `databaseconnectionpool.cpp` | `PooledSqlConnection acquire()` (RAII) |
| 内存分配 | `memalloc.h` | `void* memalloc(size_t)` / `void memfree(void*)` |
| 断点续传 | `resumableuploadsession.h` | `BeginResult beginStaged(owner,parent,display,size,resumeEnabled)` / `acceptWrittenChunk(offset,len)` |
| 并行传输调度 | `Client/transfertask.h` | `TransferManager::enqueuePersonalUpload/enqueueGroupUpload/enqueuePersonalDownload/enqueueGroupDownload(...)`、`transferIdOf(PDU*)`、`pause/resume/cancel` |
| 传输 id 取值 | `Client/transfertask.cpp` | `quint64 TransferManager::transferIdOf(const PDU*)`（41 读 `[16..23]`，其余 `[48..55]`） |
| 服务端上传槽 | `Server/msghandler.h` | `UploadCtx* findUpload(quint64)` / `bool acquireUploadSlot(quint64)` / `void abortUpload(quint64)` / `void reapIdleUploads(nowMs,idleSec)` |
| 服务端下载环 | `Server/mytcpsocket.h` | `void startDownloadStream(path,id)` / `void removeDownloadStream(id)`（`m_downloadRing` round-robin） |
| 群读状态持有 | `Client/client.h` | `GroupChatState& groupState(quint64 groupId)` / `void resetGroupStates()`（会话级：登录建立锚点、重登清空） |
| 群读游标推进 | `Client/groupchatstate.h` | `ReadCursorVerdict onArrived(quint64)`（Advanced/Buffered/Ignored）/ `applyServerAnchor(water,cursor)` / `waterSeq()` / `readCursor()` / `unread()` |
| 唯一推进入口 | `Client/groupchat.h` | `void onMessageArrived(quint64 seq)`（90 事件、自己消息、92 拉取记录共用同一入口） |

---

## 4. 数据模型摘要

| 表 | 用途 | 关键字段 |
|----|------|---------|
| `user_info` | 用户认证 | `id`(PK), `name`, `pwd`（服务端加盐哈希）, `online`, `salt`（`''`=迁移前老数据，登录成功自动补盐） |
| `friend` | 好友关系（双向） | `user_id`, `friend_id` |
| `message_info` | 聊天消息 | `seed_name`, `rev_name`, `msg`, `issuccess`, `seed_time` |
| `share_link`（规划） | 应用内分享链接凭证 | `token_hash`(唯一), `owner_name`, `relative_path`, `expire_time`, `revoked`, `created_at`；仅新增表，不变更既有表 |
| `publish_links`（当前） | 分享链接（简化版，安全不足） | `short_code`, `file_path`, `creator_name` — ⚠️ 明文存 token、无过期/撤销；待迁移，见 BUG-ARC-PUBLINK-001 |

> 规划依赖：`ClientHeartbeat` 只依赖 `Client::seedMsg()` 与 `Client::handlMsg()` 的窄接入点；`ShareLinkDialog → File` 只经新增 PDU 调用，禁止依赖或修改既有 `ShareFile` 好友分享链路。服务端 `ShareLinkService → OperateDB` 仅使用新增窄接口；解析成功后由 `MyTcpSocket` 复用既有下载流。

---

## 5. 通信协议结构

```
PDU (76B 固定头 + 柔性数组)
├── uiTotalLen: uint  — 报文总长度
├── uiMsgLen: uint    — 消息体长度
├── uiType: uint      — 消息类型枚举（当前已分配至 108；1–55 基础段，60–96 群，97–98 秒传，99–106 群文件，107–108 传输释放）
├── caData[64]        — 固定参数区（按 uiType 复用；传输族在 [48..55] 追加 u64 transfer_id，41 例外为 [16..23]）
└── caMsg[]           — 柔性数组

Msg (聊天消息体，嵌入 PDU.caMsg)
├── totalLen: uint    — 报文总长度（BUG-POOL-002 已修复，字段名两端同步）
├── msgLen: uint
├── Time[30]: char
└── msg[]: char
```

---

## 6. 已知风险区域

| 风险区域 | 风险等级 | 说明 |
|---------|---------|------|
| `ClientTask` 空实现 | ✅ 已修复 | BUG-POOL-005，已删除空 QRunnable 与 QThreadPool 调度 |
| `tcpsocketlist` 历史无锁 | ✅ 已修复 | BUG-ARCH-002，已加 QMutexLocker |
| SQL 注入 | ✅ 已修复 | BUG-DB-001，已全部迁移到预编译 |
| 数据库密码硬编码 | ✅ 已修复 | BUG-DB-002，已外部化到 connect.config |
| 枚举值冲突 | ✅ 已修复 | BUG-ARCH-001，已重新分配唯一值 |
| 文件下载全量加载 | ✅ 已修复 | BUG-FILE-001，已改为分块流式发送 |
| PDU 所有权模式混用 | ✅ 已修复 | BUG-POOL-001，sendAndRelease 与所有权注释已验证 |
| 拼写错误 `totalLen` | ✅ 已修复 | BUG-POOL-002，`totoalLen` 零源码残留，文档已同步 |
| 数据库断线重连 | 一般 | BUG-DB-003，修复中 |
| 无盐 MD5 密码 | 一般 | BUG-SEC-001，修复中 |
| 无应用层心跳 | 一般 | BUG-NET-001，已确认，可继续测试，修复延后（基础 PING/PONG 已实现；组件化延后） |
| 应用内分享链接安全 | **严重** | BUG-ARC-PUBLINK-001：已确认，待修复 → 见 R18 fix-spec（安全加固 + UI） |
| Client 登录后 pure-virtual 崩溃 | ✅ 已修复 | BUG-TST-CLIENT-001，已关闭 |
| 中文注释乱码 | ✅ 已修复 | BUG-POOL-008，UTF-8 中文注释可读 |
| 内存池分配说明不足 | ✅ 已修复 | BUG-POOL-003/007，所有权/配对注释已补充 |
| 功能基线被错误 Git 回退 | 严重 | BUG-ARC-RECOVERY-001：`git reset HEAD~1` 使 REQ-AUTH-001 及扩展代码不在当前 HEAD；历史提交混入 PDU 释放/好友转发等危险回退，必须定点恢复。 |
| 安全注销功能被回退 | 严重 | REQ-AUTH-001：历史存在实现但基线缺失；需在协议编号稳定后恢复密码校验、事务清理和用户目录清理。 |
| 下载不可 abort | 一般 | REQ-EXT-TRANSFER-001 §10 R1：客户端放弃某下载后服务端仍发完（有界 ≤ 文件大小，断线即止）；列入后续需求 |
| 传输热路径并发状态 | 一般 | REQ-EXT-TRANSFER-001：上传上下文与下载环均为**单连接单线程**访问（Qt 事件循环 + 写盘回调经 queued invoke 回主线程），新增状态不加锁；修改时须保持"所有 m_uploads/m_downloads 访问都在主线程"这一前提 |

---

## 7. Git 状态

- **分支**: `add_数据库连接池功能`（以当前工作区实时状态为准）
- **最新提交**: `1aa982d` — init: Client + Server 合并为单一仓库
- **未提交改动**: 多项 Client/Server 修复、治理文档与 `project-master/` 遗留目录清理仍未提交；提交前必须重新执行 `git status --short`，不得依赖此静态计数。
