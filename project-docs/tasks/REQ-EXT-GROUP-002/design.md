# REQ-EXT-GROUP-002 可靠群文本消息 — 设计

状态：design（设计节点）完成，待主人批准 **D2 协议编号表** 后进入 development（开发节点）。

- 工作流：Development OS v1.7。
- 分支：`codex/req-ext-group-002-group-messaging`，head `b6b3145`（含 trunk 同步：GROUP-001 已合并、SEC-001 在树内、build/dist 基础设施）。
- tier：LARGE（沿 goal 节点定档，不变）。
- owner_agent：claude-code（本会话按 takeover 协议接手，记录见 task.md）。
- 依赖终态：`REQ-EXT-GROUP-001` ✅ 已合并 `master@ee75441`；`REQ-EXT-SEC-001` ✅ 已在树内。goal 节点记录的两个硬前置全部解除。
- 本设计不执行推送、部署或不可逆数据库操作。

## 1. 目标与边界

在 `group_member`（成员关系）这一唯一事实源之上，提供**可靠**文本群消息：先持久化、后广播；重试幂等；群内序号事务内原子分配；在线有界广播；按游标分页补齐；成员级未读计数。

不实现：富媒体、编辑/撤回/引用/转发、多设备投递状态、群文件、群公告（需求 §6）。不改一对一 `message_info` 语义（D5）。

## 2. 依赖事实核对（development 前实测，2026-10-01）

| 事实 | 位置 | 对本设计的影响 |
|---|---|---|
| 群协议信封 = 版本(2)+长度(2)+request_id(8)+group_id(8)=20B，小端手写编解码 `groupAppendU16/U32/U64/String` + `groupRead*` | `protocol.h:146-205`（两份） | 群消息 PDU 直接复用同一信封，零新增编解码原语 |
| 群请求统一入口 `MsgHandler::groupRequest(uint)`，先验信封再分派，响应统一 `code+body` | `Server/msghandler.cpp:1556` | 消息类请求并挂同一入口或平级新入口，复用信封校验范式 |
| 连接级 actor 门：白名单外业务请求必须 `m_actorAuthenticated`；处理器统一用 `m_MsgHandler->m_actorName` | `Server/mytcpsocket.cpp:44-78,522` | 群消息天然被门保护；**发送者一律取 actor，忽略载荷 self 字段** |
| 群状态变更事务范式：`PooledSqlConnection` → `db.transaction()` → `SELECT ... FOR UPDATE` 锁 `group_info` → 写 → `db.commit()` | `Server/operatedb.cpp:1133-1396` | 序号分配并入同一事务，锁顺序沿用「先 `group_info` 行锁」 |
| `group_info` **无** `last_seq` 列；`group_member.last_read_seq BIGINT UNSIGNED NOT NULL DEFAULT 0` **已存在** | `Server/migrate_ext_group_001.sql:4-27` | 未读游标载体已就绪；序号分配列须由本需求迁移补齐 |
| schema 版本闸门 `checkSchemaVersion()` 取 `qMax(kFileAssetSchemaVersion, kGroupSchemaVersion)`，低版本 fail-fast、不静默建表 | `Server/operatedb.cpp:75-95` | 新增 `kGroupMessageSchemaVersion`，闸门取三者 max |
| 广播现状 `reseed()`：持 `m_tcpsocketlistMutex` 遍历、首个匹配连接同步 `write()`；`sendGroupEvent()` 为 fan-out 变体但**同样持锁同步写** | `Server/mytcpserver.cpp:116-150` | T3 新广播路径必须不持该锁同步写，且逐连接有界 |
| 客户端群模块 `Group`：`envelope()` + `sendRequest(type,groupId,body)`，事件在 `Client::handlMsg` 分发 | `Client/group.cpp`、`Client/client.cpp:296-310` | 群聊窗口复用同一 envelope/sendRequest 范式 |

结论：goal 节点盘点与本设计一致，无需回 goal 重估。

## 3. 协议设计

### 3.1 编号分配（D2，**须主人一次性批准**）

GROUP-001 已占用 60–87，并把 **88–95** 标注为「预留给 GROUP-002」。本设计把预留段扩到 **88–96**（连续、只追加、不重排旧编号），因未读摘要需要独立请求/响应对，95 单槽不足：

| 编号 | 类型 | 方向 |
|---:|---|---|
| 88 | `ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_REQUEST` | C→S |
| 89 | `ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_RESPOND` | S→C（发送者） |
| 90 | `ENUM_MSG_TYPE_GROUP_MESSAGE_EVENT` | S→C（在线成员推送） |
| 91 | `ENUM_MSG_TYPE_GROUP_PULL_MESSAGES_REQUEST` | C→S |
| 92 | `ENUM_MSG_TYPE_GROUP_PULL_MESSAGES_RESPOND` | S→C |
| 93 | `ENUM_MSG_TYPE_GROUP_READ_CONFIRM_REQUEST` | C→S |
| 94 | `ENUM_MSG_TYPE_GROUP_READ_CONFIRM_RESPOND` | S→C |
| 95 | `ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_REQUEST` | C→S |
| 96 | `ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_RESPOND` | S→C |

97–99 继续预留。88/90/91/93/95 为请求或服务端事件，**必须进 actor 门**（不加白名单）；90 由服务端生成，客户端发入的同名类型一律丢弃并记审计日志（沿用 `mytcpsocket.cpp:249-254` 对 84/85 的既有处置）。

### 3.2 载荷（沿用 20B 信封，`caMsg` 承载）

信封字段：`version=1`、`envelope_length=20`、`request_id`、`group_id`；随后是 `code`（u32，仅响应）与业务字段。业务字段一律 `u32 length + UTF-8 bytes` 或定宽整数，服务端**先验长度与边界再读**（沿用 `groupRequest` 的 `offset != payload.size()` 拒绝范式）。

| 类型 | 业务字段（信封之后） |
|---|---|
| 88 发送 | `client_msg_id`(string,≤64) + `content`(string,≤2000B UTF-8) |
| 89 发送响应 | `code` + `client_msg_id` + `seq`(u64) + `server_msg_id`(u64) + `created_at_ms`(u64) |
| 90 消息事件 | `code=0` + `group_id` + `seq` + `server_msg_id` + `sender`(string) + `msg_type`(u8: 0 文本/1 系统) + `created_at_ms` + `content` |
| 91 拉取 | `mode`(u8: 0 前向补齐/1 后向翻页) + `cursor_seq`(u64) + `limit`(u16,1–100) |
| 92 拉取响应 | `code` + `mode` + `has_more`(u8) + `next_cursor`(u64) + `group_last_seq`(u64) + `unread_count`(u64) + `count`(u32) + N×消息记录 |
| 93 已读确认 | `read_seq`(u64) |
| 94 已读响应 | `code` + `effective_seq`(u64)（服务端实际生效的单调游标） |
| 95 未读摘要 | 无业务字段（按 actor 汇总其全部 active 群） |
| 96 未读摘要响应 | `code` + `count`(u32) + N×{`group_id`(u64) + `last_seq`(u64) + `last_read_seq`(u64) + `unread`(u64)} |

消息记录编码（92 内复用）：`seq`(u64)+`server_msg_id`(u64)+`sender`(string)+`msg_type`(u8)+`created_at_ms`(u64)+`content`(string)。

校验：`content` 长度上限 2000B（客户端与服务端同值），空内容拒绝；`client_msg_id` 非空、≤64B；`limit` 越界钳到 [1,100]；未知 `mode` 拒绝。**发送 PDP 中无 sender 字段**——发送者由服务端 actor 决定。

### 3.3 兼容

旧枚举 1–87 一字不动。旧客户端不发 88–96，服务端新增 case 对旧流量零影响。未知类型落到既有 `default:`（丢弃），不映射到旧业务类型（沿用 GROUP-001 §5.2 语义）。

## 4. 数据模型

### 4.1 迁移 `Server/migrate_ext_group_002.sql`（目标版本 `2026100101`）

幂等、可重复执行；回滚脚本 `migrate_ext_group_002_rollback.sql` 只删本需求新增表/列与版本行，不动 GROUP-001 数据。

```sql
ALTER TABLE group_info
  ADD COLUMN last_seq BIGINT UNSIGNED NOT NULL DEFAULT 0;   -- 群内序号水位，事务内原子递增

CREATE TABLE IF NOT EXISTS group_message (
  msg_id        BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,  -- 服务端全局唯一消息标识
  group_id      BIGINT UNSIGNED NOT NULL,
  seq           BIGINT UNSIGNED NOT NULL,                    -- 群内序号，(group_id,seq) 唯一
  client_msg_id VARCHAR(64) NULL,                            -- 幂等键；系统消息为 NULL
  sender_name   VARCHAR(32) NOT NULL,                        -- 服务端 actor 推导，非客户端自报
  msg_type      ENUM('text','system') NOT NULL DEFAULT 'text',
  content       TEXT NOT NULL,
  created_at    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  UNIQUE KEY uk_group_seq (group_id, seq),
  UNIQUE KEY uk_group_client_msg (group_id, client_msg_id),
  KEY idx_group_created (group_id, created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT IGNORE INTO schema_version (version) VALUES (2026100101);
```

设计要点：
- `uk_group_seq` 是「序号不重复」的最后防线——即使事务逻辑退化，重复 seq 也会被数据库拒绝。
- `uk_group_client_msg` 是幂等防线：MySQL 唯一索引视 NULL 互不相等，故系统消息可多行 NULL。`(group_id, client_msg_id)` 而非全局 `client_msg_id`，允许不同群复用同一键。
- `content TEXT` + 服务端 2000B 校验；不用 `VARCHAR` 以免行溢出策略差异。
- `DATETIME(3)` 保留毫秒，用于同秒消息稳定排序展示；**排序与游标一律以 `seq` 为准**，时间仅作展示。

### 4.2 版本闸门

`OperateDB` 新增 `kGroupMessageSchemaVersion = 2026100101`；`checkSchemaVersion()` 的 `required` 改为三者 `qMax`。低版本库继续 fail-fast，迁移只由 SQL 文件人工受控执行。

## 5. 序号分配与幂等（并发正确性核心）

### 5.1 事务内原子分配（D3）

```
BEGIN
  SELECT status,last_seq FROM group_info WHERE group_id=? FOR UPDATE        -- 群行锁，串行化同群发送
    → 不存在/status<>'active' → ROLLBACK
  SELECT role FROM group_member WHERE group_id=? AND user_name=? AND status='active'   -- 成员门
    → 空 → ROLLBACK（非成员/已退出/被移除即时失效）
  -- 幂等短路：同 client_msg_id 已存在则直接返回原消息，不分配新序号
  SELECT seq,msg_id,sender_name,msg_type,created_at FROM group_message
    WHERE group_id=? AND client_msg_id=? FOR UPDATE
    → 命中 → COMMIT 并返回既有 (seq, msg_id)（不广播、不重复入库）
  UPDATE group_info SET last_seq = LAST_INSERT_ID(last_seq+1), version = version+1 WHERE group_id=?
  SELECT LAST_INSERT_ID()                                                   -- 本连接本次分配到的 seq
  INSERT INTO group_message(group_id,seq,client_msg_id,sender_name,msg_type,content)
    VALUES (?,?,?,?,'text',?)
COMMIT
```

- 选 `LAST_INSERT_ID(last_seq+1)`：单语句免显式行锁，语义等价于行锁内自增；`LAST_INSERT_ID()` 为**连接级**，`PooledSqlConnection` 保证同事务同连接。D3 替代方案 `SELECT ... FOR UPDATE` 后应用层加一在功能上等价，但多一次往返；本设计按 goal 记录推荐取前者。
- 群行 `FOR UPDATE` 使同群并发发送天然串行——**序号不重复、不倒退**由行锁保证，`uk_group_seq` 兜底。
- 幂等短路对「不存在的键」在 REPEATABLE READ 下会取唯一索引 gap 锁，使并发同键重试串行：后到事务在第一个提交后看到已存在行，走命中分支。
- 数据库重试（连接池取到新连接重放）同样命中 `uk_group_client_msg`，不产生第二行。

### 5.2 「先提交后广播」不变式

广播动作**只在 `COMMIT` 成功之后**发生，且在 `handleGroupOperation` 返回后由 `MsgHandler` 触发。提交失败 → 无广播；广播失败 → 已提交消息仍在库中，靠拉取补齐。此为可测停止条件（需求 §6）。

## 6. 在线广播（T3）

现状 `reseed()`/`sendGroupEvent()` 持 `m_tcpsocketlistMutex` 同步 `write()`，慢连接会阻塞锁与 DB 事务。群消息广播改造为：

1. `MyTcpServer::broadcastGroupMessage(groupId, pdu)`：持 `m_tcpsocketlistMutex` **仅**收集目标连接指针快照（该群 `status='active'` 成员 ∩ 当前在线且 `m_actorAuthenticated` 的连接），随即释放锁。
2. 锁外逐连接写入其**每连接有界发送队列**；队列满（按字节或条数，取先到）则**丢弃该连接的实时投递**并计数告警——该成员改为下次进入群聊时经拉取补齐（离线补偿兜底），**不影响其他成员**（D4 取每连接粒度，非全局单队列）。
3. 队列由连接自身线程在事件循环中冲刷；连接断开自动摘除并清空队列。广播路径**绝不**在持全局锁时写 socket。
4. 发送者确认（89）与在线投递（90）语义分离：89 = 已持久化，90 = 在线送达；发送方可区分（验收 1）。

成员名单来源为数据库 `group_member`（`status='active'`），与权限判定同源。

## 7. 拉取、分页与未读（T4）

- 前向补齐（`mode=0`，打开群聊/重连）：`WHERE group_id=? AND seq > cursor_seq ORDER BY seq ASC LIMIT ?`。
- 后向翻页（`mode=1`，上翻历史）：`WHERE group_id=? AND seq <= cursor_seq ORDER BY seq DESC LIMIT ?`；响应按 seq 升序回传。
- `has_more` = 是否还有更多；`next_cursor` = 本批边界 seq；`group_last_seq` = 群当前水位。
- 未读计数 = `seq > member.last_read_seq AND seq <= group_info.last_seq` 的有效消息数（`SELECT COUNT(*)`），服务端按 actor 计算，客户端不可自报。
- 拉取同受成员门（`status='active'`）保护：非成员/已退出/被移除者拉取一律拒绝，**历史权限即时失效**（验收 4）。

### 7.1 已读确认单调推进

`UPDATE group_member SET last_read_seq = LEAST(GREATEST(last_read_seq, ?), group_info.last_seq) WHERE group_id=? AND user_name=? AND status='active'`（以子查询取 `last_seq`）。单调、不可回退、不可越权代推（`user_name` 恒为 actor）；响应回 `effective_seq` 供客户端校正。单设备 MVP 只维护 `last_read_seq`；多设备维度明确排除（需求 §3）。

## 8. 系统消息（T6）

成员加入/退出/移除在**同一事务、同一序号通道**内追加一条 `msg_type='system'` 群消息（`client_msg_id=NULL`，`sender_name` = 触发者 actor，`content` 为结构化短文本，如「A 加入了群聊」）。顺序与普通消息共享 `last_seq`，故群内时间线连续。系统消息同样经 90 广播与拉取通道下发，客户端以居中灰字渲染。

契约：GROUP-001 现有成员事件（84/85）语义与类型不变，本需求只在其事务内**追加**一条群消息行；成员事件的既有行为（通知在线成员）保持不变。

## 9. 权限与失败语义

- 复用 GROUP-001 权限事实源：`group_member.status='active'` + `group_info.status='active'`。解散群、非成员、已退出、被移除者：**发送/拉取/确认三断全拒**（验收 4）。
- 统一业务返回码段（`code<0`），新增于 GROUP-001 的 -1xxx 段之后：-2xxx 段落为群消息专属（-2001 非法信封/-2002 越界载荷/-2101 非成员/-2102 群已解散/-2103 内容非法/-2104 幂等冲突异常/-2105 序号分配失败等），编码不泄露他群存在性。
- 明确失败状态：DB 失败 → 89 返回失败码且**不广播**；广播队列满/连接断开 → 记审计日志，不阻塞事务，消息留待拉取（验收 5）。

## 10. 客户端设计

- 新增群聊窗口（`Client/groupchat.*`，或并入 `Client/group.*`），从群列表双击/按钮打开，与好友/文件同级，沿用 `envelope()`+`sendRequest()` 范式。
- 发送：本地生成 `client_msg_id`（`QUuid`），发 88；收到 89 前消息为「发送中」态，成功转「已发送」，失败可对同 `client_msg_id` 重试（服务端幂等）。
- 接收：`Client::handlMsg` 新增 90 分支，按 `group_id` 路由到对应窗口；未打开窗口只累加未读计数并提示。
- 历史：打开窗口先按 `mode=0, cursor=last_read_seq` 补齐未读，到达当前水位后即可上翻（`mode=1`）。
- 未读：群列表调用 95/96 显示徽标；进入窗口并确认已读后刷新。
- Group-001 的群事件（84/85）处理不变。

## 11. 实施边界

允许修改：`Server/operatedb.{h,cpp}`、`Server/msghandler.{h,cpp}`、`Server/mytcpserver.{h,cpp}`、`Server/mytcpsocket.{h,cpp}`、新增 `Server/` 群消息源码、`Server/migrate_ext_group_002.sql` 及回滚、**两份 `protocol.h`（仅追加 88–96 的枚举与编码，frozen_paths——须主人批准）**、`Client/msghandler.{h,cpp}`、`Client/client.{h,cpp}`、新增客户端群聊窗口文件、`Client/Client.pro`、`project-docs/tasks|evidence/REQ-EXT-GROUP-002/`、handoff。

禁止：改既有枚举编号/删字段；改 `message_info` 一对一语义（D5）；实现富媒体/撤回/编辑/转发/群文件/多设备投递状态（需求 §6）；新外部依赖；`Server/filesys/` 入库；把 `group_message` 纳入一对一 `getchatMsg` 旧批量模型。

## 12. 验证方案（T7，development 后交独立测试）

仿 `build/sec001/verify.py` 模式做端到端 harness（实时服务端 + 控制台日志，绑定候选哈希）：

1. **幂等**：同 `client_msg_id` 串行/并发重放 → 库中一行、seq 不变、不重复广播。
2. **并发序号**：N 发送者并发对打 → `seq` 连续唯一无缺无重，DB 终态与预期一致。
3. **先提交后广播**：注入 DB 提交失败 → 无广播；注入广播失败 → 消息仍在库、可拉取。
4. **离线补齐 + 未读**：成员离线期间产生 M 条 → 重连按游标补齐，未读数 == M。
5. **权限三断**：非成员/已退出/被移除对发送、拉取、确认全部拒绝。
6. **分页**：前后向游标边界、`has_more`、`limit` 钳制正确。
7. **广播队列满**：慢连接灌满队列 → 只丢该连接实时投递，其他成员无影响，事后可拉取。
8. **回归**：一对一聊天、离线消息、文件全流程、GROUP-001 群生命周期全部通过。

证据要求：脚本绑定候选提交哈希，保存服务端日志、数据库终态与协议拒绝结果；独立测试用干净分离工作树重跑；独立审计只读检查差异、三门报告与 CodeGraph 影响闭包。

## 13. 停止条件与设计退出条件

停止条件（回 goal，不在开发节点临时扩面）：无法证明「先提交后广播」；序号分配拿不到数据库锁层证据；发现与 GROUP-001/SEC-001 已合并事实冲突。

design 退出条件：本文件提交到需求分支；D2 编号表、数据模型、事务/幂等/广播/分页/权限与验收映射均可供 development 直接执行；handoff 记录下一节点为 development。满足且获 D2 批准后才允许改生产代码。

## 14. 待主人批准（审批卡）

**D2 协议编号分配**（本设计唯一新增的 contract 变更；两份 `protocol.h` 为 frozen_paths）

```
要改：群消息新增 9 个协议类型编号，占用 88–96（GROUP-001 原预留 88–95，扩至 96）。
      只追加、不重排旧编号 1–87；旧客户端零影响。
风险：协议枚举属 frozen_paths，改动不可由脚本回滚；但纯追加、无删除无重排，
      兼容性可逆（撤分支即回退）。无推送、无部署、无不可逆数据操作。
建议：批准。未读摘要需要独立请求/响应对，95 单槽不足，扩到 96 是连续段内最小满足。
```

D1（启动时机）已由依赖合并满足；D3/D4/D5 按本设计推荐取值，已在 §5.1/§6/§11 落实。
