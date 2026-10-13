# BUG-REV-DB-005: OperateDB::connect() 空实现 — 数据库连接池从未初始化

| 字段 | 内容 |
|------|------|
| **缺陷编号** | BUG-REV-DB-005 |
| **严重等级** | 🔴 阻塞 |
| **发现日期** | 2026-07-29 |
| **发现者** | Bug 查找师 |
| **涉及模块** | 数据库 (DB) |
| **当前状态** | ✅ 已修复 |

## 问题描述

`OperateDB::connect()` 方法体完全为空，导致 `DatabaseConnectionPool` 从未被初始化。所有数据库操作（登录、注册、好友管理、聊天、文件）全部静默失败，返回默认值（0、false、空列表）。

## 触发条件

服务端启动 → `main.cpp:15` 调用 `OperateDB::instance().connect()` → 空函数体不执行任何初始化 → 后续所有 DB 操作失败。

## 影响范围

- **所有需要数据库的功能全部不可用**：用户登录、注册、好友管理、在线用户列表、聊天消息、文件管理、注销账户、外链分享。
- 客户端表现为：登录始终失败、注册无法保存、好友列表为空等。

## 根因位置

- `Server/operatedb.cpp:24-26` — `connect()` 为空
- `Server/server.cpp:30-39` — 数据库配置已正确从 `connect.config` 加载到 `Server` 成员变量
- `Server/databaseconnectionpool.cpp:86-101` — `init()` 方法完整实现，仅未被调用

## 修复方案

实现 `OperateDB::connect()`：
```cpp
void OperateDB::connect()
{
    Server& server = Server::getinstance();
    DatabaseConnectionPool::instance().init(
        server.m_dbHost, server.m_dbPort,
        server.m_dbUser, server.m_dbPassword, server.m_dbName
    );
    bool ok = DatabaseConnectionPool::instance().testConnection();
    if (ok) {
        qDebug() << "OperateDB::connect - 数据库连接池初始化成功";
    } else {
        qCritical() << "OperateDB::connect - 数据库连接池初始化失败";
    }
}
```

## 修复状态

✅ 已修复（2026-07-29，Bug 查找师直接修复）
