# BUG-REV-AUTH-003：测试数据存明文密码，与客户端 MD5 哈希不匹配

| 字段 | 值 |
|------|-----|
| **缺陷编号** | BUG-REV-AUTH-003 |
| **缺陷标题** | `init_test_data.sql` 中测试用户存明文密码，客户端发 MD5 哈希导致登录失败 |
| **严重等级** | 阻塞 |
| **发现者** | Bug 查找师 |
| **发现日期** | 2026-07-28 |
| **当前状态** | 已确认，待修复 |

## 涉及模块和代码位置

| 位置 | 说明 |
|------|------|
| `project-docs/init_test_data.sql:22-24` | 测试用户 INSERT 语句：`pwd='123456'` 存明文 |
| `Client/client.cpp:19-21` | `passwordHashForProtocol()` — 客户端做 MD5 哈希 |
| `Client/client.cpp:325-326` | 登录时 `copyExactField(pdu->caData+32, 32, passwordHash)` — 发送 32 字节 MD5 哈希 |
| `Server/operatedb.cpp:64-66` | `handleLogin` — 用 MD5 哈希查询 DB |

## 触发条件

1. 用户使用预置测试账号 `tester` / `123456` 登录
2. 客户端计算 `MD5("123456")` = `e10adc3949ba59abbe56e057f20f883e`
3. 发送到服务端，服务端用 MD5 哈希值查询 DB
4. DB 中 `tester` 的 `pwd` 列存的是 `123456`（明文）
5. `WHERE pwd='e10adc3949ba59abbe56e057f20f883e'` 查询无结果
6. → 返回 -1 → 客户端显示 "用户或密码其中一个错误"

## 问题说明

**架构不一致**：整个系统设计中，客户端对密码做 MD5 哈希后传输，服务端原样存储和比对。但测试数据初始化脚本直接插入了明文密码，与系统设计矛盾。

## 影响范围

- 三个预置测试账号 `tester`、`friend1`、`friend2` 全部无法登录
- 任何依赖 `init_test_data.sql` 初始化数据库的测试环境均受影响
- 用户可能反复尝试登录失败，误以为是代码 bug，浪费排错时间

## 判断依据

1. `pwd` 列定义为 `VARCHAR(32)` — 刚好容纳 32 字符 MD5 哈希
2. 客户端 `passwordHashForProtocol()` 始终对密码做 MD5 → hex → 32 字符
3. 注册流程 `handleRegit()` 原样存储客户端发来的数据（即 MD5 哈希）
4. 登录流程 `handleLogin()` 用客户端发来的数据（MD5 哈希）直接查询
5. 全链路都假设 `pwd` 存的是 MD5 哈希，唯独测试数据插入了明文

## 修正建议

### 方案 A：修复测试数据（推荐）

将 `init_test_data.sql` 中的明文密码替换为正确的 MD5 哈希值：

```sql
-- 123456 → e10adc3949ba59abbe56e057f20f883e
INSERT INTO user_info (name, pwd, online) VALUES ('tester', 'e10adc3949ba59abbe56e057f20f883e', 1);
INSERT INTO user_info (name, pwd, online) VALUES ('friend1', 'e10adc3949ba59abbe56e057f20f883e', 1);
INSERT INTO user_info (name, pwd, online) VALUES ('friend2', 'e10adc3949ba59abbe56e057f20f883e', 0);
```

或在 SQL 中添加注释说明密码格式：
```sql
-- 注意：pwd 字段存储的是 MD5(password) 的 32 字符 hex 串，不是明文！
```

### 方案 B：提供明文密码兼容（不推荐）

修改 `handleLogin` 对密码同时尝试 MD5 和明文匹配（增加复杂度，且不安全）。

## 建议验证方式

1. 重新执行 `init_test_data.sql` 初始化数据库
2. 用 `tester` / `123456` 登录 → 应成功
3. 注册新账号后用该账号登录 → 应成功
4. 检查数据库中 `pwd` 列是否为 32 字符 hex 串

---

> **记录创建日期**：2026-07-28
> **记录创建者**：Bug 查找师
