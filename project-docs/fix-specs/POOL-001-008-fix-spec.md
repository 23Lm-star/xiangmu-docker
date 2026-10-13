# 修复规格：POOL-001 ~ POOL-008 批量修复

> **来源**: ADR-005（`project-docs/decisions/static-review-ruling-2026-07-24.md`）
> **分配日期**: 2026-07-25
> **工程师**: 项目工程师
> **复查方**: Bug 查找师（静态二次审查）
> **工作分支**: `add_数据库连接池功能`（当前分支，已有 11 个缺陷的修复未提交）

---

## 总则

1. **所有 8 个缺陷在同一次提交中完成**，不要拆分为多个提交。
2. 修复后必须自检：构建通过 + 服务端启动正常。
3. 不要更改架构师裁决中明确禁止修改的内容（分配器实现、业务规则、协议布局）。
4. **不得自行将缺陷状态改为"已修复"**，代码变更完成后将状态改为"待复查"。
5. 各缺陷有明确依赖顺序（见下文执行顺序），请严格遵守。

---

## 执行顺序

```
POOL-005 (移除 ClientTask) ─── 清除废弃代码，避免干扰后续修改
    │
    ├── POOL-006 (配置启动重构) ─── 变更 server/main 生命周期
    │
    ├── POOL-001 (PDU 所有权) ────── 只改 mytcpsocket.cpp
    │
    ├── POOL-002 (totalLen 拼写) ──── 跨协议/chat 多处改动
    │
    ├── POOL-004 (登录返回值) ─────── 只改 operatedb.cpp
    │
    ├── POOL-003+007 (分配注释) ───── 只改文档/注释，不改运行时代码
    │
    └── POOL-008 (乱码注释) ──────── 只改注释 + .pro
```

POOL-005 排第一是因为它删除 `ClientTask`，如果没有先完成，后续在其他文件中建立新的所有权/注释模式后还要回改。——但实际上它们互不干涉，你可以按任意顺序做，**但 POOL-003 和 POOL-007 必须合并处理**。

---

## POOL-005 — 移除无效 ClientTask 线程池包装

| 项目 | 内容 |
|------|------|
| **等级** | 一般 |
| **涉及文件** | `Server/clienttask.h`, `Server/clienttask.cpp`, `Server/mytcpserver.h`, `Server/mytcpserver.cpp`, `Server/Server.pro` |
| **风险** | 改动简单但需确认无遗漏引用（grep 全项目 `ClientTask`、`QThreadPool`、`start()`） |

### 具体要求

1. **删除 `Server/clienttask.h` 和 `Server/clienttask.cpp`** 两个文件。
2. **`Server/mytcpserver.h`**：移除 `#include "clienttask.h"`、`QThreadPool*` 成员变量、`ClientTask*` 相关声明。
3. **`Server/mytcpserver.cpp`**：
   - 移除构造函数中的 `QThreadPool` 创建与 `setMaxThreadCount`。
   - 移除 `incomingConnection()` 中的 `ClientTask` 创建和 `start()` 调用。
   - 移除 `QThreadPool` 的析构/释放代码。
4. **`Server/Server.pro`**：从 `SOURCES` 和 `HEADERS` 中移除 `clienttask.cpp` 和 `clienttask.h`。
5. **验证**：`grep -r "ClientTask\|QThreadPool" Server/` 应只显示合理的无关引用（如 Qt 自身头文件）。

### 禁止

- 不得在本次修复中引入新的线程池模型或线程迁移。
- 不得将 Socket I/O 移入独立线程。

---

## POOL-006 — 服务端配置启动生命周期重构

| 项目 | 内容 |
|------|------|
| **等级** | 一般 |
| **涉及文件** | `Server/server.h`, `Server/server.cpp`, `Server/main.cpp` |
| **风险** | 中等——启动流程变更需确保 main.cpp 正确协调初始化顺序 |

### 具体要求

1. **`Server/server.h`**：
   - 新增 `bool initialize();` 声明。
   - 构造函数改为无参或保持默认，**不再**在构造函数内调用 `loadconfig()` 或 `listen()`。
   - 保留 `loadconfig()` 声明，但改为返回 `bool`。

2. **`Server/server.cpp`**：
   - **`loadconfig()`**：改为 `bool` 返回值。成功返回 `true`，失败 `qCritical()` 输出原因并返回 `false`。**移除所有 `QMessageBox` 调用和 `exit(1)` 调用**。
   - **构造函数**：只初始化成员变量，不做配置加载、监听启动。
   - **`initialize()`** 新方法按序执行：
     1. 调用 `loadconfig()`，失败返回 `false`。
     2. 检查配置完整性（IP、端口、文件路径、DB 配置各字段非空/合理）。
     3. 调用 `listen()`，失败 `qCritical()` 并返回 `false`。
     4. 全部成功返回 `true`。
   - 保留 `show()`（窗口显示），但移到 `main.cpp` 中 `initialize()` 成功之后。

3. **`Server/main.cpp`**：
   ```cpp
   int main(int argc, char *argv[])
   {
       QApplication a(argc, argv);
       Server& s = Server::getinstance();
       if (!s.initialize()) {
           qCritical() << "Server initialization failed. Exiting.";
           return 1;
       }
       // 仅成功后初始化数据库连接池
       DatabaseConnectionPool::instance().init(...);
       s.show();
       return a.exec();
   }
   ```

4. **配置格式**：保持现有 `connect.config` 的 8 行格式不变，不做增删行。

### 验证

- 配置合法时服务端正常启动。
- 配置缺少关键字段时输出 `qCritical` 内容并退出码非零。
- 无 `QMessageBox` 弹出。
- 无 `exit()` 调用（`return 1` 除外）。

---

## POOL-001 — PDU 响应发送/释放路径所有权约定统一

| 项目 | 内容 |
|------|------|
| **等级** | 一般 |
| **涉及文件** | `Server/mytcpsocket.h`, `Server/mytcpsocket.cpp` |
| **风险** | 低——可维护性改进，不改变运行时行为 |

### 具体要求

1. **`mytcpsocket.h`**：新增私有方法声明：
   ```cpp
   /// 发送 PDU 并释放其内存（所有权转移到此方法）
   /// pdu 为 nullptr 时安全空操作
   void sendAndRelease(PDU* pdu);
   ```

2. **`mytcpsocket.cpp`**：
   - 实现 `sendAndRelease()`:
     ```cpp
     void MyTcpSocket::sendAndRelease(PDU* pdu)
     {
         if (pdu == nullptr) return;
         write((char*)pdu, pdu->uiTotalLen);
         memfree(pdu);
     }
     ```
   - **下载分支**（`handlMsg()` 中 `DOWNLOADFILE_REQUEST` case 内）：将对 `seedMsg()` 的调用替换为 `sendAndRelease()`。
   - **`seedMsg()`** 保持现有实现（非空检查 + write + memfree），不做改动。
   - **`recvmsg()`** 中调用 `seedMsg(pdu)` 的路径保持不变。
   - 在 `handlMsg()` 函数注释前增加：
     ```
     // 所有权规则：
     // - 返回非空 PDU → 由 recvmsg() 调用 seedMsg() 发送并释放
     // - 返回 nullptr   → PDU 已由 handlMsg() 内部发送并释放（如下载流），或无需响应
     // 禁止同一 PDU 既返回又通过 sendAndRelease/seedMsg 发送
     ```

3. **不对 `seedMsg(nullptr)` 做特殊处理**——`seedMsg` 内部已有 nullptr 防护。

### 验证

- 代码编译通过。
- 所有 `seedMsg`/`sendAndRelease` 的调用路径中，同一 PDU 不会被执行两次释放。
- 功能性无变化（由测试师验证）。

---

## POOL-002 — Msg::totalLen 拼写修正

| 项目 | 内容 |
|------|------|
| **等级** | 一般 |
| **涉及文件** | `Client/protocol.h`, `Server/protocol.h`, `Client/chat.cpp`, `Client/msghandler.cpp`, `Server/msghandler.cpp`（以及其他所有引用 `totoalLen` 的文件） |
| **风险** | 中——公共协议源码接口变更，须全部引用同步，Bug 查找师二次审查 |

### 具体要求

1. **两端 `protocol.h`**：`struct Msg` 中将 `totoalLen` → `totalLen`。
2. **搜索全部引用**：使用 `grep -rn "totoalLen" Client/ Server/` 找到所有使用处，全部改为 `totalLen`。
3. **不得改变**：
   - 字段类型（`uint`）
   - 字段顺序（`totoalLen` 在 `msgLen` 之前）
   - 结构体大小（`sizeof(Msg)` 不变）
   - 任何枚举值或 PDU 结构
4. 修改完成后运行 `grep -rn "totoalLen" Client/ Server/` 确认零残留。

### 验证

- 两端编译通过。
- `grep "totoalLen"` 零结果。
- 运行时协议兼容（字段名变更不影响 wire layout）。

---

## POOL-004 — handleLogin() 返回值显式化

| 项目 | 内容 |
|------|------|
| **等级** | 一般 |
| **涉及文件** | `Server/operatedb.cpp` |
| **风险** | 低——单文件局部改动 |

### 具体要求

1. 找到 `handleLogin()` 中登录认证成功后更新 `online` 状态的代码段。
2. 将隐式 `return q.exec()` 改为：
   ```cpp
   if (!q.exec()) {
       qCritical() << "UPDATE online status failed:" << q.lastError();
       return 0;
   }
   return 1;
   ```
3. 保持整体返回值语义不变：
   - `1` = 登录成功（认证通过 + online 更新成功）
   - `0` = 系统/数据库错误（认证失败或 UPDATE 失败）
   - `-1` = 用户不存在（保持现有逻辑不变）

### 验证

- 编译通过。
- 登录成功时返回 1。
- UPDATE 失败时记录 `lastError()` 并返回 0。

---

## POOL-003 + POOL-007 — 分配策略文档注释补充

| 项目 | 内容 |
|------|------|
| **等级** | 轻微（两项合并） |
| **涉及文件** | `Client/protocol.cpp`, `Server/protocol.cpp`, `Client/client.cpp`（`seedMsg` 附近）, `Server/mytcpsocket.cpp`（`seedMsg` 附近）, `project-docs/interfaces.md` |
| **风险** | 无运行时代码变更 |

### 具体要求

1. **`Client/protocol.cpp` / `Server/protocol.cpp`**：在 `mkPDU()` 函数上方或文件顶部添加注释块：
   ```cpp
   // PDU 内存管理规则：
   // 1. 所有 PDU 必须由分配该 PDU 的同一进程内配套函数释放
   //    - Client: mkPDU() 用 malloc → 必须用 free() 释放
   //    - Server: mkPDU() 用 memalloc → 必须用 memfree() 释放
   // 2. 禁止跨进程/跨策略释放 PDU
   // 3. 接收缓冲区中的原始字节不由 seedMsg() 释放
   ```

2. **`Client/client.cpp`**：在 `seedMsg()` 函数定义前添加注释：
   ```cpp
   // 发送 PDU 到服务端。pdu 必须由 mkPDU() (malloc) 分配。
   // 发送完成后调用 free(pdu) 释放。
   // 调用方不得在 seedMsg 返回后再访问 pdu。
   ```

3. **`Server/mytcpsocket.cpp`**：在 `seedMsg()` 函数定义前添加（如果尚未有）：
   ```cpp
   // 发送 PDU 给客户端。pdu 必须由 Server 端 mkPDU() (memalloc) 分配。
   // 发送完成后调用 memfree(pdu) 释放。
   // 调用方不得在 seedMsg 返回后再访问 pdu。
   ```

4. **`project-docs/interfaces.md`**：在 §1.5 `mkPDU` 接口说明中补充：
   > **所有权规则**：Client 端 PDU 必须用 `malloc` 分配、`free` 释放；Server 端 PDU 必须用 `memalloc` 分配、`memfree` 释放。同一进程内的分配/释放始终配对。PDU 不跨进程转移所有权。

### 禁止

- 不改动任何运行时逻辑。
- 不修改 Client 分配器为 Server 内存池，反之亦然。

---

## POOL-008 — 中文注释乱码修正

| 项目 | 内容 |
|------|------|
| **等级** | 轻微 |
| **涉及文件** | `Server/msghandler.cpp`（多处乱码注释）, `Server/Server.pro` |
| **风险** | 极低——仅改动注释和编译声明 |

### 具体要求

1. **`Server/Server.pro`**：将 `CONFIG += c++11` 改为 `CONFIG += c++11 utf8_source`。
2. **`Server/msghandler.cpp`**：
   - 手动查找所有显示为乱码的中文注释（可能在 `\x` 转义或乱码字符处）。
   - 根据上下文将每条乱码注释改写为正确的 UTF-8 中文。
   - 示例常见位置：第 ~573 行附近和其他有中文注释的区域。
   - **不得**使用全文件自动转码工具——必须人工逐条确认。
   - **不得**修改除注释外的任何代码行。

### 验证

- 编译通过（`utf8_source` 确保 MinGW 正确处理 UTF-8 源码）。
- 注释在编辑器中显示为可读中文。
- 运行时代码行为不变。

---

## 提交要求

所有 8 个缺陷修复完成后：

1. 确认编译通过（Server + Client 分别构建）。
2. 确认服务端启动正常（配置合法时启动成功，配置缺失时非零退出）。
3. 将 `project-docs/defects/README.md` 中 POOL-001~008 状态全部改为 **"待复查"**。
4. 在 `project-docs/defects/README.md` 每个缺陷的末尾增加一行 `**修复备注**：（工程师填写修复摘要）`。
5. 输出修复摘要包含：每个缺陷的修改文件列表和关键变更说明。
6. 通知架构师，准备进入 Bug 查找师二次审查阶段。

---

## 代码变更清单总览

| 缺陷 | 文件 | 变更类型 |
|------|------|---------|
| POOL-005 | `Server/clienttask.h` | 🗑️ 删除 |
| POOL-005 | `Server/clienttask.cpp` | 🗑️ 删除 |
| POOL-005 | `Server/mytcpserver.h` | ✏️ 移除引用和成员 |
| POOL-005 | `Server/mytcpserver.cpp` | ✏️ 移除线程池代码 |
| POOL-005 | `Server/Server.pro` | ✏️ 移除文件引用 |
| POOL-006 | `Server/server.h` | ✏️ 新增 initialize() |
| POOL-006 | `Server/server.cpp` | ✏️ 重构启动生命周期 |
| POOL-006 | `Server/main.cpp` | ✏️ 修复启动失败处理 |
| POOL-001 | `Server/mytcpsocket.h` | ✏️ 新增 sendAndRelease |
| POOL-001 | `Server/mytcpsocket.cpp` | ✏️ 新增方法+注释 |
| POOL-002 | `Client/protocol.h` | ✏️ totoalLen→totalLen |
| POOL-002 | `Server/protocol.h` | ✏️ totoalLen→totalLen |
| POOL-002 | 所有引用 totoalLen 的文件 | ✏️ 同步改名 |
| POOL-004 | `Server/operatedb.cpp` | ✏️ 显式返回值+错误记录 |
| POOL-003+7 | `Client/protocol.cpp` | ✏️ 注释 |
| POOL-003+7 | `Server/protocol.cpp` | ✏️ 注释 |
| POOL-003+7 | `Client/client.cpp` | ✏️ 注释 |
| POOL-003+7 | `Server/mytcpsocket.cpp` | ✏️ 注释 |
| POOL-003+7 | `project-docs/interfaces.md` | ✏️ 文档补充 |
| POOL-008 | `Server/msghandler.cpp` | ✏️ 乱码注释修正 |
| POOL-008 | `Server/Server.pro` | ✏️ 增加 utf8_source |
