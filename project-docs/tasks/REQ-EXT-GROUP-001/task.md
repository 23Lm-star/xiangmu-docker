# REQ-EXT-GROUP-001 群生命周期与成员关系 — 任务记录（goal 节点归纳，2026-09-29）

## 档位
- **LARGE**（goal 节点定档）命中 tiers 决策规则第 1 条与第 2 条：① 两 `protocol.h` 为 `frozen_paths`（manifest 第 10-12 行），追加群 PDU 枚举即触冻结路径批准流；② 新建 `group_info/group_member/group_invitation` 三表 + 事务级并发（接受邀请与激活成员同事务、并发接受/移除唯一、离开可重激活）；③ 服务端权限体系（群主/管理员/普通成员与越权拒绝）为后续群消息/群文件的唯一事实源；④ 跨会话大特性。需求文档自声明 LARGE。
- 流程版本：Development OS v1.7。**分支 `codex/req-ext-group-001-membership` 未创建**——主人明确指示开发时才切；本 goal 记录以 docs-only 提交入 trunk（branching.md 规则 3(b)：任务记录可直入 trunk，不占需求分支），开发会话从当时 master 头切分支、dirty 时先同步。
- owner_agent：claude-code。
- 节点计划：goal（本会话：归纳+依赖核查+定档，不动生产源码）→ design（计划文档随分支，含 PDU 编号分配表）→ development → independent test（新会话只读）→ repair → independent audit（新会话、不同代理、只读）→ integration（三事实行）→ merge no-ff。

## 依赖核查（2026-09-29 goal 节点实测）
| 依赖 | 状态 | 影响 |
|---|---|---|
| REQ-EXT-SEC-001 身份与授权上下文 | ✅ 已并入 master@`7fc47c1`（closeout `1570744` 为 docs-only） | 群入口"校验会话"直接复用统一入口门（`isPublicRequestType`/`authRejectPdu`，mytcpsocket.cpp:44-64），群请求类型不在白名单即自动要求 `m_actorAuthenticated`，新 handler 用 `m_actorName` 覆盖客户端自称字段 |
| 在飞开发分支（group-002 goal 待批准；file-001 分支已切出） | ⚠️ 未合并 | 单主干 + 一次一个分支：GROUP-001 development 排在其后；本 goal 只写文档不碰源码，不阻塞任何人 |
| AUTH-002 / FS-003 / LOG-007 基线 | ✅ 已合并 master | 登录加盐、路径沙箱、日志分类可直接复用 |
| 后继 REQ-EXT-GROUP-002 / GROUP-003 接口诉求 | 已登记 | GROUP-002 以 `group_member.last_read_seq` 为未读游标载体、以本分支成员事实源做发送/拉取鉴权（见 `project-docs/tasks/REQ-EXT-GROUP-002/task.md` T1/T2）；GROUP-003 依赖本分支权限判定。→ T1 建表时**必须**包含 `last_read_seq` 列与稳定的成员状态语义，否则后继需求返工 |

**结论：依赖仅剩"在不飞分支之后排期"。开发启动条件 = 主人批准设计 + 在飞分支合并 master + 从 master 切分支。**

## 现状盘点（代码事实，master@`1570744` 树内；SEC-001 已在树内）
- **协议**：两 `protocol.h`（`frozen_paths`）枚举已显式分配至 **59**（`ENUM_MSG_TYPE_LISTDIR_RESPOND`），`ENUM_MSG_TYPE_MAX=99999`；PDU = 76B 定头（uiTotalLen/uiMsgLen/uiType/caData[64]/caMsg 柔性数组）；caData 两个 32B 槽约定为（自己, 对方）；`Msg{totalLen,msgLen,Time[30],msg[]}` 嵌于 caMsg。群 PDU 只能从 **60** 起追加，禁止重排旧编号、禁止直传带编译器填充的 C++ 结构体（需求 §5）。
- **统一入口门（SEC-001 已就位）**：`MyTcpSocket::handlMsg`（mytcpsocket.cpp:44）先做 `isPublicRequestType` 白名单判定，白名单（注册/登录/验证码/心跳）之外未认证一律 `authRejectPdu` 拒绝；随后以服务端 `m_actorName`/`m_connId` 覆盖 handler（`copySelfField` 先例，msghandler.cpp:28）。**新增群请求 case 只需挂进 switch（:66）即自动被鉴权门保护**。
- **关系表先例（唯一现成的"关系"模型）**：`friend(name, friendName)` 双向两行 + `UNIQUE(name, friendName)`；读取用双向 `UNION`（`handleFlushFriend` operatedb.cpp:432）；写入/删除走事务（`handleAddFriendFin` ≈:411、`handleDeleteFriend` :469 的 `db.transaction()`）。群成员是**角色化 + 状态化 + 需重激活**的模型，不能复用 `friend` 两行制，必须独立建表。
- **在线推送先例**：`MyTcpServer::seedAddFriend`(:83) / `sendAddFriendAndRelease`(:98) / `reseed`(:115) —— 持 `m_tcpsocketlistMutex` 遍历连接列表、按登录名匹配**首个**连接、**同步 `write()`**、单目标、无队列无重试。成员变化事件要发给**多个**在线成员（fan-out），需在成员名单上循环；慢连接阻塞面与"每用户仅首个连接"的假设在 design 节点必须显式对待（GROUP-002 的有界广播队列是其后继形态）。
- **离线查询先例**：登录 `user_info.online=1`、断线 `handleOffLine`(:303) 置 0；离线消息补偿是一次性"整批取回并置已投"（`message_info.issuccess`，`getchatMsg`/`chatmsg`）——**无分页无游标**，不可复用于"离线成员登录后查询未处理邀请与最新群摘要"（需求 §5），该路径需新查询接口。
- **事务与连接能力**：`db.transaction()` 先例已在树内 3 处（operatedb.cpp ≈:411/:469/:594）；QMYSQL + `DatabaseConnectionPool`（RAII、线程隔离、`acquire()` 先例）——T2/T3 的"同事务完成"有现实基线。
- **表**：无任何群相关表。现存：`user_info(id,name,pwd,online,salt)`、`friend(name,friendName)`、`message_info(seed_name,rev_name,msg,issuccess,seed_time)`、`share_link/publish_links`。建表先例 = 版本化 SQL 迁移文件（`migrate_add_password_salt.sql`、`Server/migrate_publish_links.sql`、`Server/share_link.sql`）。
- **入站边界**：`MAX_INBOUND_PDU_SIZE=16KB`、`MAX_PENDING_INBOUND_BYTES=256KB`（存量）；群请求载荷须带版本/长度/请求唯一标识（需求 §5），长度上限校验由新 handler 自带。
- **客户端**：`Index` 单例（getFriend/getFile）持有子界面；好友为 `Friend` UI + 按好友名开窗的 `Chat`；**无任何群文件/群列表/群成员 UI**（Client/ 下无 group* 文件），需新增；`Client/msghandler.cpp` 按 uiType 分发（`:126` `handlAddFriend` 等），群响应需新 handler 分支。

## 开发任务分解（待批准，依赖合并后执行）
- **T1 数据层**：`group_info`（group_id、名称、创建者、状态、版本号、创建时间）、`group_member`（群、用户、角色、成员状态、加入/更新时间、**`last_read_seq`**——GROUP-002 未读游标载体，须本分支预留）、`group_invitation`（独立邀请 ID、群、邀请人、目标人、状态、过期时间、创建/处理时间、**幂等键**）三表 + 迁移 SQL；`OperateDB` 新增窄接口（建群、成员 CRUD、按状态查邀请、并发安全的成员激活）。离开=状态位（软删除）保证可重激活（需求 §3）。
- **T2 创建群**：建群 + 建群主成员在**一个事务**内完成；任一步失败整体回滚，不得残留群或孤立群主记录（验收 1）。
- **T3 邀请与接受/拒绝**：同待处理邀请重复请求只产生一个逻辑结果（幂等键唯一定位）；过期/已处理拒绝重复接受；拒绝/过期后可重新邀请（验收 2）；接受邀请 + 激活成员同一事务（需求 §5）。
- **T4 成员与角色管理**：退出、移除、重入、群主转让、管理员任免；越权矩阵（管理员不能移群主/其他管理员、不能转让/解散；普通成员仅查看+退出）在**服务端**判定，客户端按钮只展示（需求 §4）；并发接受/移除最终状态唯一、无重复有效成员（验收 3/5）。
- **T5 群资料与成员列表**：成员列表只返回当前有权查看的群；改群标识读他群必须拒绝（验收 4）；群摘要（供离线登录后拉取）。
- **T6 在线事件与离线查询**：成员变化向在线成员发事件（fan-out，复用/扩展 `seedAddFriend` 模式并明确慢连接策略）；离线成员登录后可查询未处理邀请 + 最新群摘要（新查询路径，见"离线查询先例"）。
- **T7 协议**：两 `protocol.h` 追加 60 起群 PDU（只追加、载荷带版本/长度/请求唯一标识）；新旧客户端对未知类型必须能明确拒绝（需求 §7 停止条件）。
- **T8 证据与回归**：端到端 harness（仿 REQ-EXT-SEC-001 `build/sec001/verify.py` 模式，绑定候选哈希）——并发接受/移除对打唯一性、重复邀请幂等、越权矩阵全拒、事务回滚无残留；回归：登录/好友/一对一聊天/旧文件业务（验收 6）。

## 验收标准（业务语言，源自需求 §6）
1. 创建群失败时库里不残留群或孤立群主记录。
2. 同一待处理邀请重复请求只有一个逻辑结果；拒绝/过期后可按规则重新邀请。
3. 转让、任免、退出、移除、重入都满足角色限制，越权请求全部被服务端拒绝。
4. 成员列表只返回本人有权查看的群，改群标识读不到别人的群。
5. 两个并发接受/移除请求最终状态唯一，数据库无重复有效成员。
6. 新旧客户端至少能完成登录、好友、一对一聊天和旧文件业务回归。

## 允许路径
`Server/msghandler.{h,cpp}`、`Server/mytcpserver.{h,cpp}`、`Server/mytcpsocket.{h,cpp}`（入口门 case）、`Server/operatedb.{h,cpp}`、新增 `Server/` 群相关文件、群迁移 SQL、**两 `protocol.h`（frozen_paths——仅追加 60 起枚举/结构，须主人批准，见 D2）**、`Client/msghandler.{h,cpp}`、`Client/index.*`、新增 `Client/` 群 UI 文件、`project-docs/tasks/REQ-EXT-GROUP-001/`、`project-docs/evidence/REQ-EXT-GROUP-001/`、handoff。
**禁止**：改既有枚举编号/删字段（协议兼容硬约束）；改 `message_info` 一对一语义；实现群消息正文/群文件上传/群公告（需求 §7 排除）；新外部依赖；`Server/filesys/` 入库；在飞分支合并前动生产源码。

## 待主人批准的决策（approval card，业务语言）
- **D1 开发启动时机**：推荐 = 等 group-002 / file-001 等在飞分支合并 master 后再切本分支开发（单主干、串行最省事，pipeline 不争抢 `build/`）。替代 = 提前并行——不推荐，pipeline 与工作区冲突。
- **D2 协议编号分配**：推荐 = design 节点出一张覆盖 GROUP-001+GROUP-002 的 PDU 编号分配表（60 起连续段，只追加），主人一次性批准，两个需求按表施工，避免冻结文件二次改动（与 group-002 记录 D2 同源）。
- **D3 邀请触达模型**：推荐 = 在线即时事件 + 离线登录拉取兜底（需求 §5 两条都实现，事件失败不丢数据）；替代 = 仅拉取轮询（实现省，但需求 §5 明确要求在线事件）。
- **D4 成员重入模型**：推荐 = `group_member` 保留行 + 状态位重激活（需求 §3 硬要求"不得因历史记录无法重入"），历史不被物理删除；替代 = 删除后插新行（丢失加入历史，不可审计）——不推荐。
- **D5 群解散语义**：推荐 = `group_info.status` 置解散 + 成员表只读保留（可审计，后续需求可判失效）；替代 = 级联物理删除。需求 §4 群主可解散，删除不可逆，需批准。

## 停止条件（需求 §7 对照）
- 成员状态无法在并发下保持唯一（出现重复有效成员）→ 独立测试报 **BLOCKED**，不得进入后继分支。
- 协议新旧版本无法明确拒绝未知类型 → 停止进入 GROUP-002。
- 基线（SEC-001 或本分支合并后）出现与本盘点冲突的事实 → 回 goal 重估，不在开发节点即兴扩面。

## 决策记录（主人答复，2026-09-29；append-only）
- **D1 ✅ 启动时机**：等 **REQ-EXT-FILE-001 分支合并 master 后**再切 `codex/req-ext-group-001-membership`。理由（主人）：GROUP-001 是 GROUP-002/003 的基础。与现有排期无冲突——GROUP-002 的 development 本身以 GROUP-001 合并为前提，不会抢先。
- **D2 ✅ 协议编号分配**：**授权本分支自行分配**，不再单设主人批准项——design 节点出 GROUP-001+GROUP-002 联合编号表（60 起连续段、只追加、禁止重排旧编号），两分支按同一张表施工（避免冻结文件二次改动）。澄清：原卡的"批准"针对**冻结文件（两 protocol.h）改动本身**按项目规则需主人点头，不是要"其他分支"批准；本答复即视为该点授权。
- **D3 ✅ 邀请触达模型**：在线即时事件 + 离线登录拉取兜底；事件失败不丢数据。
- **D4 ✅ 成员重入模型**：`group_member` 保留行 + 状态位重激活，历史不物理删除。
- **D5 ✅ 解散语义（主人细化）**：仿 QQ 群解散——解散后群**不可再有任何写操作**（不能加人/发消息/上传文件/退出/被移除），成员仍可**读取过往历史**（只读保留、可审计），不做物理级联删除。**下游硬约束**：GROUP-002 群消息发送、GROUP-003 群文件上传/删除，设计时必须对"已解散群"一律拒绝。
- **D6 ✅ 主人修正（2026-09-30）**：`group_member` 保留 `(group_id,user_name)` 群内唯一约束，并新增 `(user_name,group_id)` 联合索引，按用户查询“我的群聊”时走用户优先索引；客户端在好友、文件旁增加同级群聊模块，登录不主动拉取邀请或群消息，进入群聊模块才查询群列表/邀请，打开具体群聊才查询消息和未读摘要。

## development 节点收口（2026-09-30）
- 候选代码提交：`4dcc5a6`（群迁移、事务成员关系、60–87 协议、认证分发、在线事件和客户端按需群模块及操作区）。
- `pipeline/build.ps1`：GREEN；`pipeline/test.ps1`：GREEN（当前仓库自动化测试注册数为 0，脚本已如实记录）。
- `pipeline/audit.ps1`：对最终候选提交 PASS；完整差异仍包含上一代码提交中两份 `protocol.h` 的已批准 LARGE 冻结路径改动，独立审计必须复核该事实。
- 下一节点必须在新会话和干净分离工作树以候选提交执行 independent test，只读源码并覆盖事务回滚、邀请幂等、越权、并发唯一、重入、转让、解散后写拒绝及旧业务回归。
- **本会话范围**：主人明确"先不做开发"——本记录只更新决策，不动生产源码、不切分支。
