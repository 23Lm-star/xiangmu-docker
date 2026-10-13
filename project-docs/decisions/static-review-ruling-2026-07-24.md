# ADR-005：静态审查缺陷 BUG-POOL-001～008 的架构裁决

- **裁决日期**：2026-07-24
- **输入**：`project-docs/bug-reports/BUG-静态审查-20260724.md`
- **适用分支**：`add_数据库连接池功能`
- **状态**：已裁决，待工程师实施 / Bug 查找师复查

## 已验证事实

1. `Server/mytcpsocket.cpp` 中，普通请求的响应由 `recvmsg()` 统一调用 `seedMsg()`；下载请求在 `handlMsg()` 内部发送下载元数据并返回 `nullptr`。`seedMsg()` 对非空 PDU 恰好释放一次；当前未发现报告所称的实际 double-free。
2. Client 与 Server 的 `Msg` 结构布局一致，字段 `totoalLen` 在两端代码中均有引用。拼写不改变二进制布局，但属于公共协议源码接口的错误命名。
3. 服务端 PDU 由 `memalloc/memfree` 配对，客户端 PDU 由 `malloc/free` 配对；PDU 不跨进程转移所有权，接收缓冲区中的 PDU 也不经 `seedMsg()` 释放。该差异是 ADR-002 的既定设计，不是内存管理缺陷。
4. `ClientTask::run()` 为空，但每次连接仍提交一个 `QRunnable` 给 `QThreadPool`。实际 socket I/O 由创建 socket 的主线程 Qt 事件循环驱动。
5. `Server/msghandler.cpp` 文件编码为 UTF-8，但多处中文注释的字节内容已经是乱码文本；仅增加 qmake 的 `utf8_source` 不会修复既有注释。

## 决策

### 1. PDU 响应所有权：保留“统一发送”为默认，下载流例外显式封装

不把 `DOWNLOADFILE_REQUEST` 的下载元数据直接改为从 `handlMsg()` 返回：流式下载必须在元数据写入 socket 后才调度首个数据块，不能在调用方不知情时提前调度。

工程师必须：
- 保留 `recvmsg()` 对普通 `handlMsg()` 返回值的统一 `seedMsg()`；
- 新增命名能表达所有权转移的私有辅助方法（建议 `sendAndRelease(PDU*)`），下载分支和流式下载内部只通过该方法发送；
- `seedMsg()` 保持非空检查且只负责“写入 + 释放服务端自有响应 PDU”；
- 在 `handlMsg()` 与辅助方法附近注明：返回非空 PDU 的所有权交给 `recvmsg()`，返回 `nullptr` 表示已发送或无需响应；禁止同一 PDU 同时返回与发送。

这是可维护性改进，非当前严重运行时缺陷。

### 2. `Msg::totalLen`：更正公共源码字段名，同步两端

将 Client/Server 两份 `protocol.h` 中 `totoalLen` 统一更名为 `totalLen`，同步修正 Client `chat.cpp`、`msghandler.cpp` 与 Server `msghandler.cpp` 的所有引用。不得改变 `Msg` 字段顺序、类型、对齐或任何消息类型枚举值；因此 wire layout 保持兼容。该公共协议头的变更必须经 Bug 查找师二次审查。

### 3. 跨端分配策略：维持不同实现，明确所有权契约

否决“客户端引入服务端内存池”与“为统一命名删除服务端内存池”。客户端和服务端是独立进程，分配器无需一致；唯一强制规则是分配/释放必须在同一进程同一策略内配对。工程师只补充协议文档和关键源码注释，不改分配器实现。

### 4. 登录返回值：显式映射数据库 UPDATE 结果

`OperateDB::handleLogin()` 仍沿用 `1=成功、0=系统/数据库错误、-1=认证失败` 的既有接口。验证成功后，必须以显式条件返回 `q.exec() ? 1 : 0`，并在 UPDATE 失败时记录 `q.lastError()`。不改变客户端响应协议或登录业务规则。

### 5. 服务端并发模型：移除无效线程池包装，采用单线程事件驱动

选择移除 `ClientTask`、`QThreadPool` 成员和每连接 `start()` 调用。不得把现有 `QTcpSocket` 的处理逻辑草率移入线程池：这需要 socket 线程亲和性迁移、线程事件循环、跨线程列表和数据库模型的完整重构，超出本次修复范围且风险更高。

当前服务端的正式模型为：主线程 `QTcpServer` 接收连接，每个 `MyTcpSocket` 留在主线程，Qt 事件循环以非阻塞信号槽处理 I/O。数据库连接池继续按线程隔离；在当前模型中主要使用主线程连接，仍为未来受控并发扩展保留边界。

### 6. 服务端配置失败：无 UI、可报告、启动前失败

将 `Server::loadconfig()` 改为返回 `bool`，构造函数不再隐式加载配置或启动监听。新增显式初始化入口（建议 `initialize()`）：只在配置解析、必要字段校验和 `listen()` 均成功后返回 true。`main.cpp` 必须先初始化 `Server`，失败时 `qCritical()` 输出可诊断原因并以非零状态返回；仅成功后初始化数据库并进入事件循环。移除 `QMessageBox` 与 `exit()`。

此变更不改变 `connect.config` 的 8 行格式，也不改变数据库配置来源。

### 7. 客户端 `free`：不改实现，仅补注释

这是 ADR-002 的直接结果，`malloc/free` 正确配对。与 BUG-POOL-003 一并关闭为“非缺陷”。

### 8. 乱码注释：修正源文件文本与编译声明

工程师需将 `Server/msghandler.cpp` 中已经乱码的中文注释改写为正确 UTF-8 中文；将 `Server/Server.pro` 的 `CONFIG += c++11` 更新为 `CONFIG += c++11 utf8_source`。不得以全文件自动转码替代人工确认，不得修改运行时代码逻辑。

## 质量与复查要求

- BUG-POOL-002 属公共协议源码接口变更；BUG-POOL-005、006 涉及架构/启动生命周期；四项严重项及所有跨模块变更完成后均须由 Bug 查找师静态二次审查。
- 工程师只可将缺陷状态更新为“修复中”或“待复查”；不得自行标记“已修复”。
- 动态构建、启动、登录、文件上传/下载等验证由项目测试师执行；工程师应提交可复现的构建步骤和自检证据。

## 关联缺陷

`BUG-POOL-001` ～ `BUG-POOL-008`
