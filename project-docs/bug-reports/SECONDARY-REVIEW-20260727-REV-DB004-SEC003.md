# 二次静态审查报告

> **审查日期**：2026-07-27  
> **审查 Agent**：Bug 查找师  
> **审查类型**：二次静态审查（缺陷修复后复查）  
> **审查对象**：BUG-DB-004（好友系统 JSON 迁移）、BUG-SEC-003（PDU 双层缓冲安全）

---

## 审查概述

本次审查依据工程师修复后提交的可执行提示词，对以下两个缺陷的修复实现进行独立复查：

| 缺陷编号 | 标题 | 审查重点 |
|----------|------|----------|
| BUG-DB-004 | 好友表从双向关系行迁移为单用户单行的 `friend_list` JSON 数组；迁移和双向增删必须事务化 | OperateDB 写操作事务完整性、所有好友关系增删改路径覆盖 |
| BUG-SEC-003 | 服务端 C→S PDU 缺少 16 KB 单包校验及 256 KB 未处理接收缓存高水位 | recvmsg() 内存安全校验充分性、无恶意畸形 PDU 攻击漏洞、无误触风险 |

---

## 第一层：接口一致性审查

### BUG-DB-004 — OperateDB 接口

依据 `interfaces.md` 第 2.3 节检查：

| 接口方法 | 签名一致性 | 说明 |
|----------|-----------|------|
| `handleAddFriendFin(usr, tar)` → `int` | ✅ 一致 | 签名与文档一致，内部使用 JSON 操作 |
| `handleDeleteFriend(usr, tar)` → `bool` | ✅ 一致 | 签名与文档一致，内部使用 JSON 操作 |
| `handleFlushFriend(name)` → `QStringList` | ✅ 一致 | 签名与文档一致，使用 JSON_CONTAINS |
| `handleOnlineUsers(name)` → `QStringList` | ✅ 一致 | 签名与文档一致，排除已有 JSON 好友 |
| `handleAddFriend(usr, tar)` → `int` | ✅ 一致 | 仅查在线状态，无 JSON 变更 |

**结论**：接口签名与 `interfaces.md` 完全一致 ✅

### BUG-SEC-003 — recvmsg() 安全校验

依据 `interfaces.md` 第 1.5 节检查：

| 校验规则 | 文档要求 | 实现 | 一致性 |
|----------|---------|------|--------|
| `pdu->uiTotalLen >= sizeof(PDU)` | 必需 | 第 278 行 `pduTotalLen >= sizeof(PDU)` | ✅ |
| `pdu->uiTotalLen <= MAX_INBOUND_PDU_SIZE` | 必需 | 第 278 行 `pduTotalLen <= MAX_INBOUND_PDU_SIZE` | ✅ |
| `pdu->uiMsgLen == pduTotalLen - sizeof(PDU)` | 必需 | 第 279 行 `pdu->uiMsgLen == pduTotalLen - sizeof(PDU)` | ✅ |
| `pdu->uiType > ENUM_MSG_TYPE_MIN && uiType < ENUM_MSG_TYPE_MAX` | 必需 | 第 280-281 行 | ✅ |
| 失败动作：清空 `m_dpulist`、`disconnectFromHost()`、记录对端地址 | 必需 | 第 287-293 行 | ✅ |
| `MAX_PENDING_INBOUND_BYTES` 256KB 水位线 | 必需 | 第 264-270 行 | ✅ |

**结论**：安全校验实现与 `interfaces.md` 完全一致 ✅

---

## 第二层：变更影响分析

### BUG-DB-004 影响范围

**变更类型**：数据模型变更（friend 表结构）+ 数据库访问层变更

**涉及模块**：
- `Server/operatedb.cpp` — 5 个方法被修改
- `Server/msghandler.cpp` — 调用者，接口签名未变，无需修改
- `Server/migrate_friend_to_json.sql` — 新增迁移脚本

**影响分析**：
- ✅ 未改变任何公共接口签名（`interfaces.md` 不需要更新）
- ✅ 未改变模块边界（OperateDB 仍是唯一数据库访问入口）
- ✅ 未引入新的模块依赖
- ⚠️ 需要确保生产数据库已运行迁移脚本后再部署

### BUG-SEC-003 影响范围

**变更类型**：新增安全校验逻辑（仅在 `recvmsg()` 方法内）

**涉及模块**：
- `Server/mytcpsocket.cpp` — `recvmsg()` 方法新增安全校验
- `Server/protocol.h` — 新增 `MAX_INBOUND_PDU_SIZE` 和 `MAX_PENDING_INBOUND_BYTES` 常量

**影响分析**：
- ✅ 未改变任何公共接口签名
- ✅ 未改变模块边界
- ✅ 未影响其他模块的数据流
- ✅ 仅约束 C→S 方向，不影响 S→C 方向（文件下载流式分块不受限）

---

## 第三层：深度逻辑审查

### BUG-DB-004 — 数据库事务完整性审查

#### 1. `handleAddFriendFin()` — 添加好友双向建立（第 180-258 行）

```
[事务开始] → 查询 usrId → 查询 tarId → INSERT/UPDATE usrId 行 + tarId → INSERT/UPDATE tarId 行 + usrId → [提交]
```

**事务保护**：
- `db.transaction()` 于第 193 行
- 任何中间步骤失败执行 `db.rollback()`
- `db.commit()` 于第 251 行

**SQL 正确性验证**：

| SQL 操作 | 位置 | 验证 |
|----------|------|------|
| `INSERT ... VALUES (usrId, JSON_ARRAY(tarId)) ON DUPLICATE KEY UPDATE ... JSON_ARRAY_APPEND(... tarId)` | 第 217-224 行 | ✅ 正确：首次插入创建 `[tarId]`，已存在时追加且用 JSON_CONTAINS 防重 |
| `INSERT ... VALUES (tarId, JSON_ARRAY(usrId)) ON DUPLICATE KEY UPDATE ... JSON_ARRAY_APPEND(... usrId)` | 第 234-241 行 | ✅ 同上，反向操作 |
| 参数绑定顺序（4 个 `?`：user_id, JSON_ARRAY, CAST for CONTAINS, CAST for APPEND） | 第 225-228 行 / 242-245 行 | ✅ 顺序与 SQL 中 `?` 位置一致 |

#### 2. `handleDeleteFriend()` — 删除好友双向解除（第 285-355 行）

```
[事务开始] → 查询 usrId → 查询 tarId → UPDATE 移除 tarId 从 usrId 的 friend_list → UPDATE 移除 usrId 从 tarId 的 friend_list → [提交]
```

**事务保护**：
- `db.transaction()` 于第 298 行
- 任何中间步骤失败执行 `db.rollback()`
- `db.commit()` 于第 348 行

**SQL 正确性验证**：

| SQL 操作 | 位置 | 验证 |
|----------|------|------|
| `UPDATE ... JSON_REMOVE(friend_list, JSON_UNQUOTE(JSON_SEARCH(friend_list, 'one', CAST(tarId AS JSON)))) WHERE user_id = usrId AND JSON_SEARCH(...) IS NOT NULL` | 第 322-326 行 | ✅ WHERE 子句防止不存在的 ID 误更新 |
| `UPDATE ... JSON_REMOVE(friend_list, JSON_UNQUOTE(JSON_SEARCH(friend_list, 'one', CAST(usrId AS JSON)))) WHERE user_id = tarId AND JSON_SEARCH(...) IS NOT NULL` | 第 335-339 行 | ✅ 反向操作同样受保护 |

#### 3. `handleFlushFriend()` — 刷新好友列表（读操作，第 260-283 行）

```sql
SELECT name FROM user_info WHERE IFNULL(
    JSON_CONTAINS(
        (SELECT friend_list FROM friend WHERE user_id = (SELECT id FROM user_info WHERE name = ?)),
        CAST(id AS JSON), '$'), 0)
```

- ✅ 读操作不需要事务
- ✅ 用户无 friend 行时（NULL），`IFNULL(..., 0)` 正确返回空列表
- ✅ `id` 在子查询上下文正确解析为外层的 `user_info.id`

#### 4. `handleOnlineUsers()` — 获取可添加的非好友（读操作，第 130-156 行）

```sql
SELECT name FROM user_info WHERE online = 1 AND name != ?
AND NOT IFNULL(JSON_CONTAINS(
    (SELECT friend_list FROM friend WHERE user_id = (SELECT id FROM user_info WHERE name = ?)),
    CAST(id AS JSON), '$'), 0)
```

- ✅ 读操作不需要事务
- ✅ `NOT` + `IFNULL(..., 0)` 正确排除已有好友
- ✅ 用户无 friend 行时正确显示所有在线用户为可添加

#### 5. 迁移脚本验证（`migrate_friend_to_json.sql`）

```sql
RENAME TABLE friend TO friend_backup;
CREATE TABLE friend (user_id INT NOT NULL PRIMARY KEY, friend_list JSON NOT NULL);
INSERT INTO friend (user_id, friend_list)
SELECT user_id, IFNULL(JSON_ARRAYAGG(friend_id), JSON_ARRAY())
FROM friend_backup GROUP BY user_id;
```

- ✅ 备份原表 → 创建新表 → 聚合迁移，三步骤顺序正确
- ✅ `JSON_ARRAYAGG` 正确聚合每个用户的 friend_id
- ✅ `IFNULL(..., JSON_ARRAY())` 防止无好友用户产生 NULL

#### 事务完整性总结

| 操作路径 | 事务保护 | 双向一致性 | 防重/防误操作 |
|----------|---------|-----------|-------------|
| 添加好友（usr→tar） | ✅ `transaction/commit/rollback` | ✅ 同时更新双方 | ✅ JSON_CONTAINS 防重 |
| 删除好友（usr↔tar） | ✅ `transaction/commit/rollback` | ✅ 同时移除双方 | ✅ WHERE IS NOT NULL 防止误删 |

---

### BUG-SEC-003 — recvmsg() 内存安全校验审查

#### 第一层：256KB 未处理缓存水位线（第 263-270 行）

```cpp
if (m_dpulist.size() > MAX_PENDING_INBOUND_BYTES) {
    // 记录安全告警 → 清空缓存 → 断开连接 → 立即返回
}
```

**审查要点**：

| 检查项 | 结果 | 说明 |
|--------|------|------|
| 检查时机 | ✅ 正确 | 在 `readAll()` + `append()` 之后，在 while 解析循环之前 |
| 上限值 256KB | ✅ 适当 | 正常操作中 m_dpulist 最大积累 ~4KB（一个上传块），256KB 远高于正常阈值 |
| 溢出动作 | ✅ 正确 | `clear()` + `disconnectFromHost()` + `return` |
| 对端地址记录 | ✅ 提供 | `this->peerAddress()` 用于审计 |
| 正常上传是否误触 | ✅ 不会 | 每次 `uploadfiledata()` 处理后即 `remove(0, uiTotalLen)`，缓存不增长 |

#### 第二层：PDU 头部四维校验（第 272-294 行）

```cpp
// 1. 最小长度门禁
pduTotalLen >= sizeof(PDU)     // 76 字节
// 2. 最大长度门禁  
pduTotalLen <= MAX_INBOUND_PDU_SIZE    // 16384 字节
// 3. 字段一致性
pdu->uiMsgLen == pduTotalLen - sizeof(PDU)
// 4. 类型范围
pdu->uiType > ENUM_MSG_TYPE_MIN && pdu->uiType < ENUM_MSG_TYPE_MAX
```

**审查要点**：

| 检查项 | 结果 | 说明 |
|--------|------|------|
| 最小长度控制 | ✅ 完整 | 76B，阻止零长度或过小 PDU |
| 最大长度控制 | ✅ 完整 | 16KB，阻止超大恶意 PDU |
| 字段一致性验证 | ✅ 完整 | 确保 uiMsgLen 与总长度一致，防止长度欺骗 |
| 类型范围验证 | ✅ 完整 | 仅允许 [1, 99998] 的有效类型 |
| 失败动作 | ✅ 完整 | 清空缓存 + 断开连接 + 记录对端 + 字段详情 |
| 内存分配前置 | ✅ 正确 | 在访问 caData/caMsg 之前完成校验，不按未验证长度分配内存 |
| 校验顺序 | ✅ 正确 | 先宽度后精度：先检全局水位线 → 再检单包 |

#### 误触风险分析

对正常 C→S 消息类型的最大 PDU 尺寸评估：

| 消息类型 | 最大 C→S PDU 尺寸 | 是否超 16KB | 说明 |
|----------|-------------------|-------------|------|
| 注册/登录请求 | ~76B | ✅ 否 | caMsg 仅含验证码文本（~6B） |
| 查找/在线用户 | ~76B | ✅ 否 | 仅 caData |
| 添加好友请求/响应 | ~76B | ✅ 否 | 仅 caData |
| 刷新/删除好友 | ~76B | ✅ 否 | 仅 caData |
| 聊天消息 | ~200B | ✅ 否 | caMsg 含 Msg 结构体 + 文本 |
| 获取聊天消息 | ~76B | ✅ 否 | 请求仅 caData |
| 创建/跳转/删除目录 | ~256B | ✅ 否 | caMsg 含路径字符串 |
| 刷新文件 | ~256B | ✅ 否 | caMsg 含路径字符串 |
| 重命名文件 | ~256B | ✅ 否 | caData + caMsg 路径 |
| 上传文件 DATA | ~4172B | ✅ 否 | sizeof(PDU) + 4096B 块 |
| 上传文件 INIT | ~258B | ✅ 否 | caMsg 含目录路径 |
| 下载文件请求 | ~260B | ✅ 否 | caMsg 含路径 |
| 分享/移动文件 | ~260B | ✅ 否 | caData + caMsg |
| 验证码请求 | ~76B | ✅ 否 | 仅 caData |

**结论：所有合法 C→S 消息均在 4.2KB 以内，远低于 16KB 上限，无误触风险。** ✅

#### 256KB 水位线误触分析

正常操作下，m_dpulist 的积累量：

- 简单请求：每次 `recvmsg()` 触发时仅读取到 1 个 PDU（~76B），处理后立即移除
- 文件上传：每次触发读到 1 个上传块 PDU（~4KB），其中 `uploadfiledata()` 处理，处理完移除
- 突发连续请求：Qt 事件循环驱动，每次 `readyRead` 接收一批数据，累积可能到几 KB，但如果处理是同步的，则每次循环都能立刻移除

最坏情况：如果 `handlMsg(chat)` 需要转发消息到另一客户端（异步逻辑），但 `m_dpulist.remove()` 在 seedMsg 后立即执行。因此 buffer 不会持续增长。

**结论：正常条件下不可能触及 256KB 水位线，无误触风险。** ✅

---

## 审查发现汇总

### BUG-DB-004 复查结论：✅ **已修复**

| 复查项 | 结果 | 依据 |
|--------|------|------|
| 事务保护覆盖所有写路径 | ✅ 完整 | `handleAddFriendFin` 和 `handleDeleteFriend` 均已包裹完整事务 |
| 双向一致性 | ✅ 正确 | 增删操作同时更新双方 friend_list |
| JSON 操作正确性 | ✅ 正确 | INSERT ON DUPLICATE KEY UPDATE、JSON_ARRAY_APPEND、JSON_REMOVE 使用正确 |
| 防重/防误 | ✅ 到位 | 添加时 JSON_CONTAINS 查重，删除时 WHERE IS NOT NULL 保底 |
| 读路径兼容性 | ✅ 正常 | handleFlushFriend、handleOnlineUsers 查询逻辑正确 |
| 迁移脚本 | ✅ 可用 | migrate_friend_to_json.sql 三步骤清晰 |
| 数据结构破坏现有逻辑 | ✅ 无 | 接口签名未变，调用方无需修改 |

### BUG-SEC-003 复查结论：✅ **已修复**

| 复查项 | 结果 | 依据 |
|--------|------|------|
| 第一层 256KB 水位线 | ✅ 已实现 | 阻断内存耗尽攻击，记录对端地址 |
| 第二层 16KB 单包校验 | ✅ 已实现 | 四维校验：最小长度、最大长度、字段一致性、类型范围 |
| 校验顺序正确性 | ✅ 正确 | 先整体水位线 → 再单包验证 |
| 失败动作完整性 | ✅ 完整 | clear() + disconnectFromHost() + qWarning 记录字段详情 |
| 对正常消息无误触 | ✅ 已验证 | 合法 C→S PDU 最大 4.2KB，远低于 16KB |
| 对正常流无误触 | ✅ 已验证 | 每次处理后立即 remove，buffer 不累积 |
| 接口文档一致性 | ✅ 一致 | 与 interfaces.md 1.5 节完全匹配 |

---

## 发现问题

本次二次审查未发现新的阻塞或严重缺陷。记录以下两个属于已有代码（非本次修复引入）的轻微观察：

### OBS-001（轻微 — 已有问题，非本次引入）

**标题**：`handlMsg(chat)` 中 PDU 所有权转让后 MsgHandler 的 `this->pdu` 指针在循环迭代间处于悬空状态

**位置**：`Server/mytcpsocket.cpp:304-306`
```cpp
PDU* respdu=handlMsg(pdu);  // 将 m_dpulist.data() 指针存储到 m_MsgHandler->pdu
seedMsg(respdu);
m_dpulist.remove(0,pdu->uiTotalLen);  // remove 后旧 pdu 指针悬空
```

**说明**：`handlMsg` 将 `m_dpulist.data()` 的指针存入 `m_MsgHandler->pdu`，`m_dpulist.remove()` 后该指针悬空。当前没有代码在下一次迭代之前访问 `m_MsgHandler->pdu`，因此不会出错，但这是一个脆弱模式，未来修改时有风险。

**建议**：在 `handlMsg` 返回后在 `recvmsg()` 中设置 `m_MsgHandler->pdu = nullptr`。

### OBS-002（轻微 — 已有问题，非本次引入）

**标题**：`MyTcpServer::seedAddFriend()` 中的 PDU 转发存在内存泄漏

**位置**：`Server/msghandler.cpp:197-200` + `Server/mytcpserver.cpp:43-55`

**说明**：`addfriend()` 中分配的转发 PDU（`mkPDU()` → `memalloc`）通过 `seedAddFriend()` 发送后未被释放。`addfriend()` 返回 `nullptr`，`recvmsg()` 中 `seedMsg(nullptr)` 不执行释放。转发 PDU 不会被释放。

**注意**：此问题与本次审查的 BUG-DB-004 和 BUG-SEC-003 无关，属于已有代码中的独立问题。

---

## 缺陷状态更新

| 缺陷编号 | 原状态 | 新状态 | 说明 |
|----------|--------|--------|------|
| BUG-DB-004 | 待复查 | ✅ **已修复** | 二次审查通过，事务保护完整，JSON 操作正确 |
| BUG-SEC-003 | 待复查 | ✅ **已修复** | 二次审查通过，双层安全机制完整，无误触风险 |

---

## 可测试范围（向测试师交接）

### 正常功能回归测试（必须覆盖）

| 测试场景 | 涉及模块 | 验证要点 |
|----------|---------|---------|
| 用户 A 添加用户 B 为好友 | 端到端 | 双向 friend_list 均正确追加 |
| 用户 A 删除好友 B | 端到端 | 双向 friend_list 均正确移除 |
| 删除最后一个好友后刷新 | OperateDB | 返回空好友列表 |
| 添加已存在的好友（重复） | OperateDB | 不产生重复条目 |
| 查看在线可添加用户 | OperateDB | 不显示已是好友的用户 |
| 刷新好友列表 | OperateDB | 正确返回所有好友的用户名 |

### 安全机制测试（必须覆盖）

| 测试场景 | 验证要点 |
|----------|---------|
| 发送超大 PDU（>16KB） | 服务端应断开连接并清理缓存 |
| 发送非法 uiType（0 或 99999） | 服务端应断开连接 |
| 发送 uiMsgLen 与 uiTotalLen 不一致的 PDU | 服务端应断开连接 |
| 发送 uiTotalLen < sizeof(PDU) 的 PDU | 服务端应断开连接 |
| 正常上传大文件（>100MB 分块） | 不断连，上传正常完成 |
| 恶意慢速发送累积 >256KB | 服务端应断开连接 |

### 禁测区域及原因

| 路径 | 原因 |
|------|------|
| 旧 friend 表结构（备份表 friend_backup） | 数据已迁移，旧表仅作为备份存在 |
| S→C 方向的大型 PDU（如文件下载分块） | 安全校验仅针对 C→S 方向 |
| Client 端的 recvmsg() | 未修改，安全校验仅在服务端 |

---

## 标准交接指令

```
下一执行 Agent：测试师

可执行提示词：
“请对 BUG-DB-004（好友系统 JSON 迁移逻辑）和 BUG-SEC-003（服务端 PDU 接收双层缓冲安全机制）的修复进行功能回归测试和安全校验测试。
- BUG-DB-004 测试重点：添加好友双向事务完整性、删除好友双向事务完整性、重复好友防护、空好友列表场景
- BUG-SEC-003 测试重点：超大 PDU（>16KB）断连、非法 uiType 断连、长度不一致断连、256KB 慢速累积断连、正常上传大文件不误触
- 禁测区域：旧 friend 表备份、S→C 方向大 PDU、Client 端 recvmsg()
- 已知未修复问题（非本次变更引入）：addfriend() 中转发 PDU 存在一次 memalloc 泄漏
- 所有测试完成后更新 BUG-DB-004 和 BUG-SEC-003 状态为‘已测试，待合并’”
```

---

*报告生成完毕。所有缺陷状态已同步更新。*
