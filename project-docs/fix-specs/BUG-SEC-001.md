# BUG-SEC-001 修复方案 — 密码加盐 SHA-256（修正版）

> **优先级**：P3（一般，修复延后）
> **前置依赖**：BUG-DB-001（SQL 注入修复）已完成
> **裁决人**：等待项目架构师审阅
> **执行人**：项目工程师

---

## 问题

密码使用 `QCryptographicHash::Md5.toHex()` 无盐哈希，彩虹表可批量反向破解。

**客户端**（`Client/client.cpp`）：
```cpp
QString strpwd = QCryptographicHash::hash(pwd.toUtf8(), QCryptographicHash::Md5).toHex();
```

**服务端**（`Server/operatedb.cpp`）：
- 存储原始 MD5 值（无盐）
- 登录时比对接收到的 MD5 值

## 协议约定

```
Client → Server:  MD5(password)        (保持现状不变)
Server 存储:      SHA256(salt + MD5(password))  (存储加盐哈希)
Server 校验:      SHA256(salt + client_md5) == stored_hash
```

客户端不受影响：不需要更新或重新编译。服务端全权处理加盐逻辑。

## 影响范围

3 个文件 + 1 个数据库变更：
- `Server/operatedb.cpp` — 注册/登录逻辑
- `Server/operatedb.h` — （如需要）
- `project-docs/interfaces.md` — 更新密码方案说明
- 数据库 `user_info` 表增加 `salt` 字段

## 修改清单

### 1. 数据库变更 — `user_info` 表增加 salt 字段

```sql
ALTER TABLE user_info ADD COLUMN salt VARCHAR(32) NOT NULL DEFAULT '' AFTER pwd;
```

- `DEFAULT ''` 保证旧用户数据完整性
- 旧用户的 `salt=''`，登录时 `SHA256("" + md5) == md5` 成立（兼容旧数据）

### 2. `Server/operatedb.cpp` — 注册时生成盐值 + 加盐 SHA-256

```cpp
bool OperateDB::handleRegit(char *strname, char *strpwd)
{
    // 1. 生成 16 位随机盐值
    QString salt;
    salt.reserve(16);
    const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int i = 0; i < 16; ++i) {
        salt.append(chars[qrand() % (sizeof(chars) - 1)]);
    }

    // 2. 计算加盐 SHA-256
    QString hashedPwd = QString(QCryptographicHash::hash(
        (salt + QString::fromUtf8(strpwd)).toUtf8(),
        QCryptographicHash::Sha256).toHex());

    // 3. 存储（已使用预编译，BUG-DB-001 确保）
    q.prepare("INSERT INTO user_info(name, pwd, salt) VALUES(:name, :pwd, :salt)");
    q.bindValue(":name", QString::fromUtf8(strname));
    q.bindValue(":pwd", hashedPwd);
    q.bindValue(":salt", salt);
    return q.exec();
}
```

### 3. `Server/operatedb.cpp` — 登录时兼容旧+新密码

```cpp
int OperateDB::handleLogin(char *strname, char *strpwd)
{
    // 1. 查出用户记录（含 salt）
    q.prepare("SELECT id, pwd, salt, online FROM user_info WHERE name = :name");
    q.bindValue(":name", QString::fromUtf8(strname));
    if (!q.exec() || !q.next()) return -1;  // 用户不存在

    QString storedHash = q.value(1).toString();
    QString salt = q.value(2).toString();

    // 2. 计算加盐哈希
    QString inputHash = QString(QCryptographicHash::hash(
        (salt + QString::fromUtf8(strpwd)).toUtf8(),
        QCryptographicHash::Sha256).toHex());

    // 3. 比对
    if (inputHash != storedHash) return -1;  // 认证失败

    // 4. 旧用户迁移：如果 salt 为空（旧 MD5），更新为新格式
    if (salt.isEmpty()) {
        QString newSalt;
        newSalt.reserve(16);
        const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        for (int i = 0; i < 16; ++i) {
            newSalt.append(chars[qrand() % (sizeof(chars) - 1)]);
        }
        QString newHash = QString(QCryptographicHash::hash(
            (newSalt + QString::fromUtf8(strpwd)).toUtf8(),
            QCryptographicHash::Sha256).toHex());

        QSqlQuery updateQ;
        updateQ.prepare("UPDATE user_info SET pwd=:pwd, salt=:salt WHERE name=:name");
        updateQ.bindValue(":pwd", newHash);
        updateQ.bindValue(":salt", newSalt);
        updateQ.bindValue(":name", QString::fromUtf8(strname));
        if (!updateQ.exec()) {
            qWarning() << "Failed to migrate password for" << strname;
        }
    }

    // 5. 更新在线状态
    // ... 原有 online 状态更新逻辑 ...
    return 1;
}
```

### 4. `Client/client.cpp` — 不变

客户端仍发送 `MD5(password.toUtf8()).toHex()`。服务端负责加盐逻辑。无需更新客户端。

---

## 旧用户兼容策略

| 场景 | 登录前 | 登录后 |
|------|--------|--------|
| 旧用户（`salt=''`） | `SHA256("" + client_md5)` == `旧 MD5` → 登录成功 | salt 和 pwd 自动升级为新格式 |
| 新用户（`salt='xxx'`） | `SHA256(salt + client_md5)` == `存储值` | 正常 |
| 入侵者 | 无 salt 无法预计算彩虹表 | 即使数据库泄露，每个密码独立盐值 |

### 过渡期行为

1. 部署本修改后，所有新注册用户自动使用加盐 SHA-256
2. 旧用户首次登录时自动迁移（透明升级）
3. 旧用户迁移后密码不可逆，需重置时才走注册流程
4. 不存在新旧算法混合的竞态条件——登录查询和迁移在同一个事务语义内（MySQL InnoDB 默认）

---

## 验证

```bash
cd Server && qmake && make clean && make
cd Client && qmake && make clean && make
```

**通过标准**：两端编译成功。

## 手工测试步骤

1. **新用户注册**：注册新账号 → 检查数据库 `pwd` 字段是 64 字符 SHA-256 哈希，`salt` 是 16 位随机字符串
2. **新用户登录**：使用新账号登录 → 成功
3. **旧用户兼容**：使用已有旧账号登录 → 成功 → 检查数据库 `salt` 字段变为非空
4. **错误密码**：输入错误密码 → 登录失败
5. **断线恢复**：服务端重启后旧用户可正常登录（迁移是一次性的）

## 不在本次范围

- bcrypt/Argon2 外部库引入 — 当前 SHA-256 已足够，后续评估
- 传输层加密（TLS）— 独立安全增强
- 密码强度策略（最小长度、字符组合）— 后续独立需求

---

## 否决方案

### 方案 B：客户端计算加盐哈希（服务端下发 salt）

- **否决理由**：需要修改客户端协议（增加 salt 下发步骤），需要新协议消息类型，增加复杂度且与 BUG-DB-001 无直接关联。当前“客户端 MD5 + 服务端加盐 SHA-256”方案无需修改客户端，零部署成本。
- **不采用**
