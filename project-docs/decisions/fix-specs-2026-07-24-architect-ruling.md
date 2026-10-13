# 架构师裁决记录：fix-specs 批量审阅

> 裁决人：项目架构师  
> 裁决时间：2026-07-24  
> 审阅范围：`project-docs/fix-specs/` 中 10 个待审 fix-spec；另发现 `BUG-ARCH-001-R1a.md` 已有裁决且缺陷记录显示已修复，本次仅确认归档状态。  
> 事实来源：`project-docs/defects/README.md`、各 fix-spec、`Server/msghandler.cpp`、`Server/mytcpsocket.cpp`、`Server/operatedb.cpp`、`Server/databaseconnectionpool.*`、`Client/client.cpp`、`Client/msghandler.*`、`Client/userinformation.cpp`、两端 `protocol.h`。

---

## 一、总体裁决

本批 fix-spec **批准进入工程实施**，但其中部分方案需按本文修正后执行，不允许工程师按原文中存在架构问题的伪代码直接落地。

### 排期优先级

| 排期 | 缺陷编号 | 等级 | 裁决 | 下一责任 Agent | 二次审查 |
|---:|---|---|---|---|---|
| P0 | BUG-DB-002 | 严重 | 已按既有裁决执行：配置不完整时拒绝启动；本轮只要求验证 `connect.config` 与构建结果 | 测试师 / Bug 查找师 | 需要 |
| P1 | BUG-FILE-002 / BUG-ARCH-001-R1 | 一般 | 批准修复；必须拆分 `movefile()`，`MOVEFILE_REQUEST` 返回 `MOVEFILE_RESPOND`；分享接收保持复制语义，不得改成移动 | 工程师 | 需要 |
| P2 | BUG-ARCH-003 | 一般 | 批准最小闭环实现；本轮不扩展数据库表结构，只返回现有 `user_info` 可提供的用户名与在线状态 | 工程师 | 需要 |
| P3 | BUG-FILE-001 | 一般 | 选择方案 A1：`MyTcpSocket` 状态机 + `QFile` 流式分块；禁止方案 B；禁止把全文件读入 `QByteArray remainingData` | 工程师 | 需要 |
| P4 | BUG-DB-001 | 严重 | 选择一次性全改：`Server/operatedb.cpp` 所有用户可控 SQL 同轮迁移为 `prepare()` + `bindValue()` | 工程师 | 需要 |
| P5 | BUG-BUILD-001 | 轻微 | 批准修复 `Server.pro` 重复 `config.qrc` | 工程师 | 不强制 |
| P6 | BUG-BUILD-002 | 轻微 | 批准扩展 `.gitignore` 覆盖 Qt/qmake 构建产物 | 工程师 | 不强制 |
| P7 | BUG-DB-003 | 一般 | 延后；原方案需修正：`DatabaseConnectionPool` 当前不是 `QObject`，不得直接使用 `QTimer(this)`/slots；优先实现 acquire 懒重连 | 架构师后续再派发 | 需要 |
| P8 | BUG-NET-001 | 一般 | 延后；原方案需修正为“单一心跳方向”，建议服务端驱动：Server 发 REQUEST，Client 回 RESPOND | 架构师后续再派发 | 需要 |
| P9 | BUG-SEC-001 | 一般（安全增强） | 延后；依赖 DB-001 完成后再做。批准未来扩表加 `salt`，但需独立迁移方案和旧用户兼容策略 | 架构师后续再派发 | 需要 |

---

## 二、主要决策

### 1. BUG-FILE-001 — 分块传输选择方案 A 还是 B？

**裁决：选择方案 A1（状态机 + 服务端 socket 持有 QFile 流式发送），否决方案 B。**

**必须修正 fix-spec 原方案中的一点**：原文“状态机”示例仍使用 `QByteArray remainingData` 和 `file.readAll()`，这不能解决内存暴涨问题。工程师必须改为：

- `MsgHandler::downloadfile()` 仅做路径合法性、文件存在性、文件名与大小确认；
- `MyTcpSocket` 在处理 `DOWNLOADFILE_REQUEST` 后启动下载状态；
- 下载状态持有 `QFile*` 或成员 `QFile`、`filePath`、`fileName`、`totalSize`、`sentSize`；
- 每次 `sendNextChunk()` 只 `read(CHUNK_SIZE)`，发送一个数据 PDU；
- 发送完成/断连/出错时关闭文件并清理状态；
- 两端 `protocol.h` 需新增下载数据/完成/失败必要枚举，且 Client `handlMsg()` 必须能追加写入文件，而不是只处理单个 `DOWNLOADFILE_RESPOND`。

**理由**：
1. 现有 `MyTcpSocket::handlMsg()` → `seedMsg()` 是单请求单响应结构，方案 B 在 `MsgHandler::downloadfile()` 内直接写 socket 会破坏处理层职责和 PDU 生命周期。
2. 真正目标是避免 `Server/msghandler.cpp:506 file.readAll()`；任何仍一次性读入内存的“分块”实现均不合格。
3. 状态机对断连、取消、错误清理更可控，适合作为后续断点续传基础。

**影响范围**：`Server/mytcpsocket.h/.cpp`、`Server/msghandler.h/.cpp`、`Client/client.cpp`、`Client/msghandler.h/.cpp`、两端 `protocol.h`。

---

### 2. BUG-DB-001 — SQL 注入修复一次性全改还是分批？

**裁决：一次性全改。**

**执行边界**：仅限 `Server/operatedb.cpp` 中现有 SQL 拼接迁移为预编译绑定，不在同一任务中做密码哈希、表结构、业务规则调整。

**理由**：
1. 当前 `Server/operatedb.cpp` 至少 15 处 `QString::arg()` 拼接用户输入，安全风险是同一类缺陷，分批会留下可利用窗口。
2. 修改模式一致，单文件集中迁移更容易审查，也避免多轮重复回归。
3. 若先只修登录/注册，聊天、好友、离线消息仍可通过用户名/消息内容注入，安全目标不完整。

**影响范围**：`Server/operatedb.cpp` 全部数据库访问函数；需完整回归注册、登录、好友、聊天、离线消息。

---

### 3. BUG-ARCH-003 — 是否扩展数据库表结构支持更多用户信息字段？

**裁决：本轮不扩展数据库表结构。**

**批准方案**：做最小闭环，使 `GETUSERINFORMATION_REQUEST/RESPOND` 可用，返回现有数据：用户名、在线状态、查询结果码。客户端 UI 中无法由现有表提供的昵称、年龄、性别、地址、职业、学校、签名等字段显示为“未设置/暂无资料”，不得伪造。

**理由**：
1. 现有事实显示 `user_info` 只被代码当作 `id/name/pwd/online` 使用；扩展用户资料字段属于数据模型变更，需要数据库迁移、旧数据默认值、UI 保存入口和测试策略，不应混入“Server 端缺失处理函数”的一般缺陷修复。
2. 先补齐协议分发闭环可解除功能无响应问题，后续再以独立需求做“用户资料完善”。
3. 避免在 DB-001/SEC-001 尚未完成前扩大数据库变更面。

**影响范围**：`Server/msghandler.h/.cpp`、`Server/mytcpsocket.cpp`、`Client/client.cpp`、`Client/msghandler.h/.cpp`、`Client/userinformation.cpp`。

---

## 三、各 fix-spec 具体裁决

### BUG-DB-002 — 数据库密码硬编码

**裁决：维持既有方案 A，批准进入验证。**

- 配置不足 8 行时 `exit(1)` 拒绝启动；不得保留 `123456` 或任何密码回退值。
- 当前 `Server/server.cpp:31-44` 已体现该方向。
- 下一步不再由工程师扩大修改，交由 Bug 查找师/测试师验证嵌入资源 `connect.config` 为 8 行、服务端构建和启动行为正确。

### BUG-FILE-002 / BUG-ARCH-001-R1 — 移动文件响应类型错误

**裁决：批准修复，但修正原方案语义。**

- `MOVEFILE_REQUEST` 必须独立调用 `MsgHandler::movefile()` 并返回 `ENUM_MSG_TYPE_MOVEFILE_RESPOND`。
- `SHAREFILE_AGREE_RESPOND` 对应的 `sharefilemove()` 是“接收分享文件到接收者目录”，业务语义应是复制，不应删除发送者源文件；原 fix-spec 中把 `sharefilemove()` 改为 rename/copy+remove 的建议不批准。
- `movefile()` 内部需做路径合法性校验，防止跨用户目录或任意路径移动。
- Client `handlMsg()` 增加 `MOVEFILE_RESPOND`，成功后刷新文件列表。

### BUG-ARCH-003 — 用户信息查询功能 Server 端缺失

**裁决：批准最小闭环，不扩表。**

- Server 新增处理函数和分发 case。
- Client 新增响应处理函数。
- 返回字段以现有表为准：查询结果码、用户名、在线状态；详细资料字段显示“未设置/暂无资料”。
- 若用户后续要求可编辑个人资料，再新建独立需求与数据库迁移决策。

### BUG-FILE-001 — 文件下载一次性加载到内存

**裁决：批准修复，采用 A1 状态机流式分块。**

- 不批准方案 B。
- 不批准任何仍保留 `readAll()` 的实现。
- 需新增 Client 端分块接收与落盘逻辑，验证大文件 MD5 一致。

### BUG-DB-001 — SQL 注入

**裁决：批准一次性全改。**

- 所有 SQL 拼接同轮迁移为 `prepare/bindValue`。
- 日志不得输出完整含密码 SQL 或明文/哈希密码。
- 修复后必须做安全输入测试。

### BUG-BUILD-001 — `config.qrc` 重复引用

**裁决：批准。**

- `Server/Server.pro:59-62` 改为 `RESOURCES += config.qrc`。

### BUG-BUILD-002 — `.gitignore` 覆盖不足

**裁决：批准。**

- 增加 qmake/Qt 构建产物忽略规则。
- 不得忽略源码、`.pro`、`.qrc`、`project-docs/`。

### BUG-DB-003 — 数据库断线重连机制

**裁决：延后，原技术方案需重写后再实施。**

- 当前 `DatabaseConnectionPool` 不是 `QObject`，原文 `new QTimer(this)` 与 slot 连接方案不可直接编译。
- 后续优先做懒重连：`acquire()` 检出空闲连接时执行 `SELECT 1` 或失败后关闭并重建；若要定时健康检查，Timer 应由 `Server`/`OperateDB` 这类 QObject 持有，或将连接池显式改造为 QObject（需架构师另行批准）。

### BUG-NET-001 — 应用层心跳

**裁决：延后，原技术方案需统一心跳方向后再实施。**

- 不允许 Server 和 Client 同时各自发送 `HEARTBEAT_REQUEST` 形成双向重复心跳。
- 建议后续采用服务端驱动：Server 定时发 `HEARTBEAT_REQUEST`，Client 收到后立即回 `HEARTBEAT_RESPOND`，Server 连续 3 次未收到响应则断开并清理。

### BUG-SEC-001 — 密码加盐 SHA-256

**裁决：延后，依赖 DB-001。**

- 未来允许扩展 `user_info.salt`，但必须有独立数据库迁移、旧用户兼容、注册/登录协议说明。
- 本轮不得和 SQL 注入修复混做，避免安全修复之间互相遮蔽问题。

---

## 四、质量门禁

工程师完成 P1-P4 后，必须满足：

1. Server 与 Client 均可构建通过。
2. Bug 查找师完成二次审查：协议枚举同步、分发 case 完整、SQL 绑定覆盖全部用户输入、文件移动/下载没有越权路径。
3. 测试师执行回归：注册、登录、验证码、好友、聊天、文件刷新/移动/分享/下载、数据库配置启动。
4. 测试报告归档到 `project-docs/test-reports/`。

在上述门禁完成前，架构师不宣布交付通过。
