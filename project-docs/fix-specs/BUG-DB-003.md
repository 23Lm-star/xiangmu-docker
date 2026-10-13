# BUG-DB-003 修复方案 — 数据库断线重连（修正版）

> **优先级**：P3（一般，修复延后）
> **原方案问题**：`DatabaseConnectionPool` 不是 `QObject`，不能直接使用 `QTimer(this)`/slots
> **修正要点**：改为 `acquire()` 懒重连 + `OperateDB` 持有定时健康检查
> **裁决人**：等待项目架构师审阅
> **执行人**：项目工程师

---

## 问题

`Server/databaseconnectionpool.cpp:126-128` 中 `acquire()` 发现连接断开时，仅做一次 `db.open()` 重试，无重试机制，MySQL 超时断开后连接池不会主动恢复。

## 修正方案选择：方案 A（推荐）— 双重保障：懒重连 + 定时健康检查

### 总体架构

```
OperateDB (QObject, 有事件循环)
  ├─ 定时器每 30 秒调用 DatabaseConnectionPool::healthCheck()
  │
  DatabaseConnectionPool (非 QObject)
  ├─ acquire(): 懒重连（3 次指数退避）
  └─ healthCheck(): SELECT 1 探测 + 清理死连接
```

### 修改清单

#### 1. `Server/databaseconnectionpool.h` — 新增健康检查接口

```diff
 class DatabaseConnectionPool
 {
 public:
     static DatabaseConnectionPool& instance();
     void init(/*...*/);
     PooledSqlConnection acquire();
     void release(const QString &connectionName);
     bool testConnection();
+    void healthCheck();              // 由外部 QObject 定时调用
     void close();

 private:
     // 移除原 QTimer 相关声明
+    bool reconnect(const QString &connectionName);  // 指数退避重连
+    bool openConnection(const QString &connectionName, const QString &hostName,
+                        int port, const QString &userName,
+                        const QString &password, const QString &databaseName);
 };
```

#### 2. `Server/databaseconnectionpool.cpp` — 实现懒重连 + 健康检查

**`acquire()` — 懒重连（指数退避）**：

```cpp
PooledSqlConnection DatabaseConnectionPool::acquire()
{
    QMutexLocker locker(&m_mutex);

    ThreadConnections &tc = m_connections[QThread::currentThreadId()];

    while (!tc.idleConnectionNames.isEmpty()) {
        const QString connectionName = tc.idleConnectionNames.dequeue();

        if (!QSqlDatabase::contains(connectionName)) {
            --tc.createdCount;
            continue;
        }

        QSqlDatabase db = QSqlDatabase::database(connectionName);
        if (!db.isOpen()) {
            // 懒重连：最多重试 3 次
            bool reconnected = reconnect(connectionName);
            if (!reconnected) {
                qDebug() << "acquire: reconnect failed for" << connectionName;
                --tc.createdCount;
                continue;
            }
        }

        return PooledSqlConnection(connectionName, *this);
    }
    // ... 创建新连接 ...
}
```

**`reconnect()` — 指数退避重试**：

```cpp
bool DatabaseConnectionPool::reconnect(const QString &connectionName)
{
    const int MAX_RETRIES = 3;
    const int BASE_DELAY_MS = 500;

    QSqlDatabase db = QSqlDatabase::database(connectionName, false);
    if (db.isOpen()) db.close();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);

    // 重新创建连接
    for (int attempt = 0; attempt < MAX_RETRIES; ++attempt) {
        QSqlDatabase newDb = QSqlDatabase::addDatabase("QMYSQL", connectionName);
        newDb.setHostName(m_hostName);
        newDb.setPort(m_port);
        newDb.setUserName(m_userName);
        newDb.setPassword(m_password);
        newDb.setDatabaseName(m_databaseName);

        if (newDb.open()) {
            qDebug() << "reconnect success on attempt" << (attempt + 1);
            return true;
        }

        int delay = BASE_DELAY_MS * (1 << attempt);  // 500ms, 1s, 2s
        qDebug() << "reconnect attempt" << (attempt + 1) << "failed, retry in" << delay << "ms";
        QThread::msleep(delay);

        QSqlDatabase::removeDatabase(connectionName);
    }

    qCritical() << "reconnect failed after" << MAX_RETRIES << "attempts";
    return false;
}
```

**`healthCheck()` — 由 OperateDB 定时调用**：

```cpp
void DatabaseConnectionPool::healthCheck()
{
    QMutexLocker locker(&m_mutex);

    for (auto it = m_connections.begin(); it != m_connections.end(); ++it) {
        ThreadConnections &tc = it.value();
        QQueue<QString> aliveQueue;

        while (!tc.idleConnectionNames.isEmpty()) {
            const QString connName = tc.idleConnectionNames.dequeue();

            if (!QSqlDatabase::contains(connName)) {
                --tc.createdCount;
                continue;
            }

            QSqlDatabase db = QSqlDatabase::database(connName);
            if (db.isOpen()) {
                QSqlQuery q(db);
                if (q.exec("SELECT 1")) {
                    aliveQueue.enqueue(connName);
                    continue;
                }
            }

            qDebug() << "healthCheck: connection" << connName << "dead, removing";
            db.close();
            db = QSqlDatabase();
            QSqlDatabase::removeDatabase(connName);
            --tc.createdCount;
        }

        tc.idleConnectionNames = aliveQueue;
    }
}
```

#### 3. `Server/operatedb.h` — OperateDB 持有定时器

```diff
 class OperateDB : public QObject
 {
     Q_OBJECT
 public:
     static OperateDB& instance();
-    void connect();
+    bool connect();
+    void startHealthCheck(int intervalMs = 30000);

 private:
     OperateDB();
     ~OperateDB();
+    QTimer* m_healthCheckTimer;
+
+private slots:
+    void onHealthCheckTimeout();
 };
```

#### 4. `Server/operatedb.cpp` — 实现健康检查定时器

```cpp
bool OperateDB::connect()
{
    Server& server = Server::getinstance();
    DatabaseConnectionPool::instance().init(
        server.m_dbHost, server.m_dbPort,
        server.m_dbUser, server.m_dbPassword,
        server.m_dbName
    );

    if (DatabaseConnectionPool::instance().testConnection()){
        qDebug() << "connect database success";
        return true;
    } else {
        qCritical() << "connect database failed";
        return false;
    }
}

void OperateDB::startHealthCheck(int intervalMs)
{
    if (!m_healthCheckTimer) {
        m_healthCheckTimer = new QTimer(this);
        connect(m_healthCheckTimer, &QTimer::timeout,
                this, &OperateDB::onHealthCheckTimeout);
    }
    m_healthCheckTimer->start(intervalMs);
    qDebug() << "health check started, interval:" << intervalMs << "ms";
}

void OperateDB::onHealthCheckTimeout()
{
    DatabaseConnectionPool::instance().healthCheck();
}
```

#### 5. `Server/main.cpp` — 初始化后启动健康检查

```diff
 int main(int argc, char *argv[])
 {
     QApplication a(argc, argv);
     Server& s = Server::getinstance();
-    OperateDB::instance().connect();
+    if (!OperateDB::instance().connect()) {
+        return 1;
+    }
+    OperateDB::instance().startHealthCheck(30000);
     return a.exec();
 }
```

---

## 安全考虑

- 健康检查执行 `SELECT 1`，不涉及用户数据
- 日志中不输出密码或完整连接串
- 重连次数有限（3 次），不会无限循环耗尽资源

## 验证

```bash
cd Server && qmake && make clean && make
```

**通过标准**：编译成功，服务端可正常启动。

## 手工测试步骤

1. 启动 Server，确认 `connect database success`
2. 观察 30 秒后是否有 `health check started` 日志
3. 在 MySQL 中执行 `KILL CONNECTION <id>` 手动断开连接
4. 预期：下次健康检查检测到断开，自动清理死连接
5. 通过客户端触发数据库操作（如登录），验证 `acquire()` 懒重连生效

## 不在本次范围

- 连接池动态扩容/缩容
- 写操作的事务断线自动重试

---

## 否决方案

### 方案 B：把 DatabaseConnectionPool 改为 QObject

- **否决理由**：单例类改为 QObject 需要处理 QObject 拷贝语义和生命周期问题；连接池本质是数据管理类，引入信号槽增加不必要复杂度
- **不采用**

### 方案 C：完全靠 `acquire()` 懒重连，不做定时健康检查

- **否决理由**：懒重连只能在使用连接时恢复；不使用连接的空闲连接会一直处于断开状态，在首次使用时增加延迟
- **不采用**：保留懒重连作为兜底，定时检查作为主动发现
