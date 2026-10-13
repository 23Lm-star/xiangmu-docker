# BUG-DB-001 修复方案 — SQL 注入风险（全部使用 QString::arg 拼接）

> **优先级**：P4（严重，但改动面大，建议独立轮次修复）
> **裁决人**：等待项目架构师审阅（需确定分批方案）
> **执行人**：项目工程师

---

## 问题

`Server/operatedb.cpp` 全部 15+ 个 SQL 操作使用 `QString::arg()` 字符串拼接用户输入，未使用预编译语句。恶意用户名可注入任意 SQL。

## 影响范围

1 个文件：`Server/operatedb.cpp` — 全部 15+ 处 SQL 操作

## 修复原则

所有 SQL 查询统一迁移为 `QSqlQuery::prepare()` + `bindValue()` 方式，禁止 `QString::arg()` 拼接用户可控数据。

---

## 完整修改对照表

| # | 函数 | 行号 | 当前拼接 | 改为预编译 |
|:-:|------|:----:|----------|-----------|
| 1 | `handleRegit()` | 32 | `"select * from user_info where name='%1'"` | `"select * from user_info where name = :name"` |
| 2 | `handleRegit()` | 41 | `"insert into user_info(name,pwd) values('%1','%2')"` | `"insert into user_info(name,pwd) values(:name, :pwd)"` |
| 3 | `handleLogin()` | 59 | `"select * from user_info where name = '%1' and pwd = '%2'"` | `"select * from user_info where name = :name and pwd = :pwd"` |
| 4 | `handleLogin()` | 70 | `"update user_info set online = 1 where name = '%1' and pwd = '%2'"` | `"update user_info set online = 1 where name = :name and pwd = :pwd"` |
| 5 | `handleOffLine()` | 88 | `"update user_info set online = 0 where name = '%1'"` | `"update user_info set online = 0 where name = :name"` |
| 6 | `handleFindUser()` | 106 | `"select * from user_info where name = '%1'"` | `"select * from user_info where name = :name"` |
| 7 | `handleOnlineUsers()` | 124-125 | `"select name from user_info where id not in ... where name ='%1'"` | `"select name from user_info where id not in ... where name = :name"` |
| 8 | `handleAddFriend()` | 150 | `"select * from user_info where name ='%1' and online = 1"` | `"select * from user_info where name = :name and online = 1"` |
| 9 | `handleAddFriendFin()` | 171-175 | `"insert into friend(user_id,friend_id) values(...'%1','%2'...'%3','%4')"` | 四个占位符 `:usrname1 :tarname1 :tarname2 :usrname2` |
| 10 | `handleFlushFriend()` | 189-191 | `"select name from user_info where id in ... where name ='%1'"` | `"select name from user_info where id in ... where name = :name"` |
| 11 | `handleDeleteFriend()` | 214-222 | `"delete from friend where(...'%1'...'%2'...'%3'...'%4')"` | 四个占位符 `:userName1 :tarName1 :tarName2 :userName2` |
| 12 | `seedchatMsg()` | 240 | `"select * from user_info where name ='%1' and online = 1"` | `"select * from user_info where name = :name and online = 1"` |
| 13 | `seedchatMsg()` | 246-248 | `"insert into message_info(...) values('%1','%2','%3',%4,'%5')"` | 五个占位符 `:seedName :revName :msg :ans :seedTime` |
| 14 | `getchatMsg()` | 267 | `"select msg,seed_time from message_info where seed_name ='%1' and rev_name = '%2' and issuccess = 0"` | 两个占位符 `:seedName :revName` |
| 15 | `getchatMsg()` | 279 | `"update message_info set issuccess = 1 where seed_name ='%1' and rev_name = '%2'"` | 两个占位符 `:seedName :revName` |

---

## 修改示例（#1 — handleRegit 查询）

### 修改前

```cpp
QString sql_select=QString("select * from user_info where name='%1'").arg(strname);
qDebug()<< "query user exist" << sql_select << endl;
if(!q.exec(sql_select)){
    return false;
}
```

### 修改后

```cpp
QString sql_select = "select * from user_info where name = :name";
qDebug()<< "query user exist" << sql_select << "name=" << strname << endl;
q.prepare(sql_select);
q.bindValue(":name", QString(strname));
if(!q.exec()){
    return false;
}
```

---

## 函数级实现（以 handleAddFriendFin 为例）

### 修改前

```cpp
QString sql=QString(R"(
    insert into friend(user_id,friend_id)
    values((select id from user_info where name='%1'),
           (select id from user_info where name='%2')),
          ((select id from user_info where name='%3'),
           (select id from user_info where name='%4'))
)").arg(usrname).arg(tarname).arg(tarname).arg(usrname);
return q.exec(sql);
```

### 修改后

```cpp
QString sql = R"(
    insert into friend(user_id, friend_id)
    values((select id from user_info where name = :usr1),
           (select id from user_info where name = :tar1)),
          ((select id from user_info where name = :tar2),
           (select id from user_info where name = :usr2))
)";
q.prepare(sql);
q.bindValue(":usr1", QString(usrname));
q.bindValue(":tar1", QString(tarname));
q.bindValue(":tar2", QString(tarname));
q.bindValue(":usr2", QString(usrname));
return q.exec();
```

---

## 验证

```bash
cd Server && qmake && make clean && make
```

**通过标准**：编译成功，零错误零警告。

## 安全测试

修复后，以下输入不应导致 SQL 注入：

| 输入 | 预期行为 |
|------|---------|
| `admin' -- ` | 登录失败（`--` 被作为普通字符） |
| `' OR '1'='1` | 查询不返回意外结果 |
| `test'; DROP TABLE user_info; --` | 仅被当作普通用户名查询 |

## 手工功能测试

1. 启动 Server + Client
2. 注册新用户（正常流程）
3. 登录（正常流程）
4. 查找用户、添加好友、发送消息、文件操作
5. 离线再上线，验证离线消息功能

**通过标准**：全部核心功能流程正常。

## 分批策略（架构师选择）

| 批次 | 范围 | 涉及函数数 | 回归范围 |
|:----:|------|:---------:|---------|
| **A（推荐）** | 一次性全部迁移 | 15 | 全功能回归 |
| **B** | 先核心：注册/登录/好友 | 8 | 仅核心功能 |
| **C** | 拆分 3 轮：每轮 5 个函数 | 15 | 三轮回归 |

> 建议采用 **批次 A**：虽然改动面大，但所有修改模式完全相同（`arg` → `prepare+bindValue`），风险可控，一次性完成可避免反复回归。

## 不在本次范围

- BUG-DB-003（断线重连）— 修复延后
- BUG-SEC-001（密码哈希）— 修复延后
- 数据库表结构变更
