# BUG-DB-002 修复方案 — 数据库密码硬编码

> **优先级**：P0（严重）
> **状态**：修复已实施，需验证 connect.config 内容
> **裁决人**：项目架构师（方案 A — 配置不完整时 exit(1) 拒绝启动）
> **执行人**：项目工程师
> **验证人**：Bug 查找师 / 项目测试师

---

## 问题

数据库连接密码 `"123456"` 硬编码在 `Server/operatedb.cpp` 源码中，部署时无法更改，源码泄露即导致凭据泄露。

## 架构师裁决

**方案 A**（已选定）：配置不完整时 `exit(1)` 拒绝启动，不做任何回退。`connect.config` 通过 Qt Resource 嵌入二进制，由开发者控制，不存在需要"向后兼容"的场景。

## 已实施修改

### 1. `connect.config` 扩展为 8 行

| 行号 | 配置项 | 默认值 |
|:----:|--------|--------|
| 1 | IP 地址 | 127.0.0.1 |
| 2 | 端口 | 8888 |
| 3 | 文件路径 | ... |
| 4 | 数据库主机 | 127.0.0.1 |
| 5 | 数据库端口 | 3306 |
| 6 | 数据库名 | chat |
| 7 | 数据库用户 | root |
| 8 | **数据库密码** | **(由开发者填入)** |

### 2. `Server/server.h` — 新增 DB 配置成员

```cpp
QString m_dbHost;
quint16 m_dbPort;
QString m_dbName;
QString m_dbUser;
QString m_dbPassword;
```

### 3. `Server/server.cpp:loadconfig()` — 按架构师裁决实现

```diff
-        } else {
-            // ❌ 旧代码：硬编码回退密码
-            m_dbHost = "127.0.0.1";
-            m_dbPort = 3306;
-            m_dbName = "chat";
-            m_dbUser = "root";
-            m_dbPassword = "123456";
+        } else {
+            QMessageBox::critical(this, "配置错误",
+                QString("connect.config 格式不完整（%1/8 行），服务端无法启动。"
+                        "需要：IP, 端口, 文件路径, DB主机, DB端口, DB名, DB用户, DB密码")
+                .arg(lst.size()));
+            qCritical() << "connect.config: expected 8 lines, got" << lst.size();
+            exit(1);
        }
```

### 4. `Server/operatedb.cpp:connect()` — 从 Server 单例读取配置

```cpp
void OperateDB::connect()
{
    Server& server = Server::getinstance();
    DatabaseConnectionPool::instance().init(
        server.m_dbHost,
        server.m_dbPort,
        server.m_dbUser,
        server.m_dbPassword,
        server.m_dbName
    );
    // ...
}
```

## 验证步骤

### 验证 1：connect.config 文件内容

```bash
cat Server/connect.config
```

预期输出为 **8 行**，第 8 行为实际数据库密码（非 `123456`）。

### 验证 2：配置不完整时服务端拒绝启动

```bash
# 备份原配置
cp Server/connect.config Server/connect.config.bak
# 截断到 3 行
head -3 Server/connect.config > Server/connect.config.test
cp Server/connect.config.test Server/connect.config
# 重新编译 & 启动
cd Server && qmake && make clean && make
./Server  # 应弹出错误对话框并 exit(1)
# 恢复
cp Server/connect.config.bak Server/connect.config
```

**通过标准**：服务端弹框报错后立即退出，C 标准输出打印 `connect.config: expected 8 lines, got 3`。

### 验证 3：配置完整时正常启动

```bash
cd Server && qmake && make clean && make
./Server
```

**通过标准**：服务端启动成功，`qDebug` 输出 `connect database success`。

## 不在本次范围

- BUG-DB-001（SQL 注入）— 单独排期修复
- BUG-DB-003（断线重连）— 修复延后
- BUG-SEC-001（密码无盐 MD5）— 修复延后
