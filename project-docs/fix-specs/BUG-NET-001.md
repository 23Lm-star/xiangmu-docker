# BUG-NET-001 修复方案 — 应用层心跳机制（修正版）

> **优先级**：P3（一般，修复延后）
> **原方案问题**：Client 和 Server 均主动发送 `HEARTBEAT_REQUEST`，形成双向重复心跳
> **修正要点**：统一为服务端驱动——Server 发 REQUEST，Client 回 RESPOND，Server 3 次无响应则断开
> **裁决人**：等待项目架构师审阅
> **执行人**：项目工程师

---

## 问题

无应用层心跳。`QTcpSocket::disconnected` 信号仅在操作系统检测到断开时触发。NAT 超时、路由器重启等中间网络故障会导致假死连接长期占用服务端资源。

## 影响范围

4 个文件：`Server/protocol.h`（Client 共用）、`Server/mytcpsocket.h`、`Server/mytcpsocket.cpp`、`Client/client.cpp`

## 修改清单

### 1. `protocol.h`（两端同步）— 新增心跳枚举

```diff
     ENUM_MSG_TYPE_GETUSERINFORMATION_REQUEST,   // 获取用户信息请求 44
     ENUM_MSG_TYPE_GETUSERINFORMATION_RESPOND,   // 获取用户信息响应 45
+    ENUM_MSG_TYPE_HEARTBEAT_REQUEST,            // ⬅️ 仅服务端发送 46
+    ENUM_MSG_TYPE_HEARTBEAT_RESPOND,            // ⬅️ 仅客户端回复 47
     ENUM_MSG_TYPE_MAX=99999,
```

> ⚠️ **注意**：`HEARTBEAT_REQUEST` 仅由服务端发送，`HEARTBEAT_RESPOND` 仅由客户端回复。两端不得交换角色。

### 2. `Server/mytcpsocket.h` — 新增心跳成员

```diff
 class MyTcpSocket : public QTcpSocket
 {
     Q_OBJECT
 public:
     MyTcpSocket();
     ~MyTcpSocket();
+    void startHeartbeatCheck(int intervalMs = 30000);
+    void stopHeartbeatCheck();

     QString m_strLoginName;
     // ...
 private:
+    QTimer* m_heartbeatTimer;
+    int m_missedHeartbeats;
+    static const int MAX_MISSED = 3;

+private slots:
+    void onHeartbeatTimeout();
 };
```

### 3. `Server/mytcpsocket.cpp` — 实现服务端驱动心跳

**构造函数初始化**：

```cpp
MyTcpSocket::MyTcpSocket()
{
    connect(this, &MyTcpSocket::readyRead, this, &MyTcpSocket::recvmsg);
    connect(this, &MyTcpSocket::disconnected, this, &MyTcpSocket::clientOffLine);
    m_MsgHandler = new MsgHandler;

    // 心跳初始化（QTimer 由 MyTcpSocket 持有，MyTcpSocket 是 QObject）
    m_heartbeatTimer = new QTimer(this);
    connect(m_heartbeatTimer, &QTimer::timeout,
            this, &MyTcpSocket::onHeartbeatTimeout);
    m_missedHeartbeats = 0;
}
```

**启动/停止心跳**：

```cpp
void MyTcpSocket::startHeartbeatCheck(int intervalMs)
{
    m_missedHeartbeats = 0;
    m_heartbeatTimer->start(intervalMs);
}

void MyTcpSocket::stopHeartbeatCheck()
{
    m_heartbeatTimer->stop();
}
```

**心跳超时处理**（服务端驱动：Server 发送 REQUEST）：

```cpp
void MyTcpSocket::onHeartbeatTimeout()
{
    if (++m_missedHeartbeats >= MAX_MISSED) {
        qDebug() << "Heartbeat timeout for user:" << m_strLoginName
                  << "missed:" << m_missedHeartbeats;
        // 连续 3 次未收到 RESPOND，断开连接
        disconnectFromHost();
        return;
    }

    // 发送心跳请求
    PDU* pdu = mkPDU();
    pdu->uiType = ENUM_MSG_TYPE_HEARTBEAT_REQUEST;
    this->write((char*)pdu, pdu->uiTotalLen);
    memfree(pdu);
}
```

### 4. `Server/mytcpsocket.cpp:handlMsg()` — 处理心跳响应

```diff
+    case ENUM_MSG_TYPE_HEARTBEAT_RESPOND:{
+        // 收到客户端心跳响应，重置计数器
+        m_missedHeartbeats = 0;
+        respdu = nullptr;  // 无需回复
+        break;
+    }
     default:
         break;
```

### 5. `Server/mytcpsocket.cpp:clientOffLine()` — 停止心跳

```diff
 void MyTcpSocket::clientOffLine(){
+    stopHeartbeatCheck();
     OperateDB::instance().handleOffLine(m_strLoginName.toStdString().c_str());
     MyTcpServer::getinstance().removeSocket(this);
     qDebug() << "用户" << m_strLoginName << "已经退出登录" << endl;
```

### 6. `Server/mytcpserver.cpp:incomingConnection()` — 登录成功后启动心跳

```diff
 MyTcpSocket* socket = new MyTcpSocket;
 // ...配置...
+
+// 注意：心跳不在 incomingConnection 启动，而在用户登录成功后启动
+// 登录完成时（handleLogin 成功后），调用 socket->startHeartbeatCheck()
```

> ⚠️ **重要**：心跳应仅在用户**登录成功后**启动，而不是在 TCP 连接建立时。未登录的连接不需要心跳检测（超时断开成本低）。

**在 `MsgHandler::login()` 的成功路径末尾**：

```cpp
// login() 返回 RESPOND 前调用
MyTcpSocket* socket = qobject_cast<MyTcpSocket*>(sender());
if (socket) {
    socket->startHeartbeatCheck();
}
```

### 7. `Client/client.cpp` — 处理心跳请求并回复

**在 `handlMsg()` 中新增**：

```cpp
case ENUM_MSG_TYPE_HEARTBEAT_REQUEST:{
    // 服务端发来心跳探测，立即回复 RESPOND
    PDU* respdu = mkPDU();
    respdu->uiType = ENUM_MSG_TYPE_HEARTBEAT_RESPOND;
    seedMsg(respdu);  // seedMsg 会调用 free(respdu)
    break;
}
```

> ⚠️ **客户端不发送 HEARTBEAT_REQUEST**：心跳方向统一为 Server→REQUEST / Client→RESPOND。客户端不主动检查服务端存活（若服务端挂了连接自然断开）。

---

## 交互时序

```
Server                              Client
  │                                    │
  ├── [30s] HEARTBEAT_REQUEST ──────►  │
  │                                    ├── HEARTBEAT_RESPOND ◄──────
  │◄── HEARTBEAT_RESPOND received      │
  │ m_missedHeartbeats = 0             │
  │                                    │
  ├── [30s] HEARTBEAT_REQUEST ──────►  │
  │    (无响应...)                      │  ← Client 断开/卡死
  ├── [30s] HEARTBEAT_REQUEST ──────►  │
  │    (无响应...)                      │
  ├── [30s] m_missedHeartbeats=3       │
  │ disconnectFromHost() ────────────  │
  │ cleanup resources                  │
```

---

## 安全性

- 心跳 PDU 无敏感数据，仅含类型枚举
- 服务端不依赖心跳做认证决策（仅用于资源清理）
- 伪造心跳 RESPOND 只会推迟超时断开，不会提升权限

## 验证

```bash
cd Server && qmake && make clean && make
cd Client && qmake && make clean && make
```

**通过标准**：两端编译成功，零错误零警告。

## 手工测试步骤

1. 启动 Server + Client，正常登录
2. 观察 Server 日志，每 30 秒应有心跳超时检测输出
3. 确认 Client 日志显示每次收到 REQUEST 后回复 RESPOND
4. 将 Client 的网络断开（如拔网线）
5. 预期：约 90 秒后（3 次超时），Server 自动断开 Client 并清理资源

## 不在本次范围

- 客户端自动重连 — 当前仅由服务端断开，客户端需手动重新登录
- 心跳间隔外部化配置 — 当前为硬编码 30 秒，后续可外部化到 `connect.config`
- 心跳与文件上传/下载的交互 — 大文件上传时应暂停心跳检测，避免干扰

## 否决方案

### 方案 B：双向心跳（Client 和 Server 各自发 REQUEST）

- **否决理由**：架构师裁决明确禁止双向重复心跳。两端同时做相同的事浪费带宽、增加复杂性、两侧超时配置不一致可能导致误断。
- **不采用**

### 方案 C：客户端驱动（Client 发 REQUEST，Server 回 RESPOND）

- **否决理由**：服务端需要主动检测客户端存活以释放资源；客户端驱动只能知道自己到服务端的连通性，服务端无法确定客户端是否存活（客户端可能还在发 REQUEST 但服务端收不到）。
- **不采用**
