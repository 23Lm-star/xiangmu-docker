# BUG-REV-AUTH-004: 注销账户密码校验因无 null 终止符导致越界读取

| 字段 | 内容 |
|------|------|
| **缺陷编号** | BUG-REV-AUTH-004 |
| **缺陷标题** | `MsgHandler::logoff()` 中 `char[32]` 无 null 终止符导致 `QString::fromUtf8()` 越界读取密码字段 |
| **严重等级** | **严重** |
| **涉及模块** | 服务端 MsgHandler / OperateDB |
| **代码位置** | `Server/msghandler.cpp:73-76`（root cause），`Server/operatedb.cpp:399-400`（崩溃点） |
| **触发条件** | 用户执行注销操作，且密码的 MD5 哈希恰好为 32 字符（即填满 `caData[32-63]` 全部 32 字节） |
|| **当前状态** | **待复查** |
| **发现日期** | 2026-07-29 |
| **发现者** | Bug 查找师 |

---

## 问题说明

### 链式分析

1. **客户端** (`Client/client.cpp:438`)：`copyExactField(pdu->caData+32, 32, passwordHash)` 将 32 字节 MD5 哈希复制到 PDU，无 null 终止符。这在 PDU 层面是正常的（`caData` 共 64 字节，由 `caData[32-63]` 承载）。

2. **服务端提取** (`Server/msghandler.cpp:73-76`)：
   ```cpp
   char userName[32] = {'\0'};
   char userPwd[32] = {'\0'};
   memcpy(userName, pdu->caData, 32);
   memcpy(userPwd, pdu->caData+32, 32);  // ← BUG: 填满后无 '\0'
   ```
   `{'\0'}` 初始化将所有 32 字节置零，但紧接着 `memcpy(userPwd, ..., 32)` 覆盖了全部 32 字节。若密码哈希恰好 32 字节（MD5 十六进制 = 32 字符），则 `userPwd` 数组中**没有 null 终止符**。

3. **数据库校验** (`Server/operatedb.cpp:399-400`)：
   ```cpp
   q.addBindValue(QString::fromUtf8(pwd));
   ```
   `QString::fromUtf8(const char*)` 期望以 `\0` 终止的 C 字符串。`pwd` 指向无终止符的 `char[32]`，函数继续读取栈上后续内存中的随机数据，产生一个被污染的密码字符串。

### 预期行为

`OperateDB::handleLogoff()` 应使用与 `handleLogin()` 完全相同的密码（32 字符 MD5 哈希），查询数据库后找到匹配记录，返回 `true`。

### 实际行为

密码字符串被栈垃圾污染，与数据库中存储的 MD5 值不匹配，`SELECT` 返回空结果集 → `handleLogoff` 返回 `false` → 服务器日志输出 `"logoff: user/pwd mismatch or user not found"`。

### 影响范围

- **阻塞注销功能**：任何 MD5 哈希恰好 32 字符的用户（这是绝大多数情况）无法注销账户。
- **注**：用户名 "123" 只有 3 字节，`userName` 中后续有 29 个 `\0`，`QString::fromUtf8()` 正常终止，所以用户名提取正确。只有密码字段受影响。

### 判断依据

1. 对比 `MsgHandler::login()` 使用 `fixedString()` → 正常工作
2. 对比 `MsgHandler::logoff()` 使用裸 `memcpy` + `char[32]` → 失败
3. `fixedString()` 通过 `QByteArray(data, maxLen)` 创建有界字节数组，避免越界
4. 用户提供的服务器日志证实：请求收到正确的 `"123"` 和 `"e10adc3949ba59abbe56e057f20f883e"`，但数据库查询返回 mismatch

---

## 修正建议

### 方案 A（推荐 — 与登录流程一致）

使用 `fixedString()` 替代裸 `memcpy`：

```cpp
// Server/msghandler.cpp:71-80 修改为：
PDU *MsgHandler::logoff()
{
    QString userName = fixedString(pdu->caData, 32);
    QString userPwd  = fixedString(pdu->caData+32, 32);
    
    qDebug() << "logoff request for:" << userName;
    
    bool ret = OperateDB::instance().handleLogoff(
        userName.toUtf8().constData(), 
        userPwd.toUtf8().constData()
    );
    // ... 后续逻辑不变
```

### 方案 B（最小改动）

将缓冲区声明为 `char[33]` 保留 null 终止符空间：

```cpp
char userName[33] = {'\0'};
char userPwd[33]  = {'\0'};
memcpy(userName, pdu->caData, 32);     // 第 33 字节保持 '\0'
memcpy(userPwd, pdu->caData+32, 32);   // 第 33 字节保持 '\0'
```

**推荐方案 A**，因为与同文件中的 `login()`、`rigist()` 使用 `fixedString()` 保持一致。

---

## 建议验证方式

1. 使用测试用户（密码 "123456"，MD5 = `e10adc3949ba59abbe56e057f20f883e`，恰好 32 字符）
2. 登录该用户
3. 执行注销操作
4. 确认服务器日志输出 `"logoff: successfully deleted user"`
5. 确认数据库中 `user_info` 表该记录已删除
6. 确认该用户的文件目录已从服务器文件系统移除
7. 使用一个密码 MD5 哈希不足 32 字符（虽然 MD5 始终 32 字符，但可构造边界测试）的用例验证回归
