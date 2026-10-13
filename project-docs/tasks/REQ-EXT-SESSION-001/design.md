# REQ-EXT-SESSION-001 设计文档（design 节点，2026-09-30）

基线：`claude/req-ext-session-001-unique-login`@bb09313（=master@ccfd5de+本任务文档）。FILE-001 实测未合并（独立测试节点，7eba642）→ 本设计按其**未合并态**施工，遵守禁触清单；C1 适配规则见 §8。

## 1. 设计决策（D 表）

| # | 决策点 | 裁决 | 理由（代码事实） |
|---|---|---|---|
| D1 | 客户端唯一 ID 承载 | 登录 PDU caMsg **NUL 分隔附加区**：`[验证码\0][ID≤32字节]`，服务端按 `strlen(caMsg)+1 .. uiMsgLen` 取剩余字节，不足/为空→空 ID | `validateCaptcha` 用 `QString::fromUtf8(caMsg).trimmed()`(:78) 只读到首个 `\0`，追加字节**不影响现有校验语义**；老客户端剩余 0 字节=空 ID，天然向后兼容；注册报文不动（注册不需要 ID）。两 `protocol.h` 零命中 |
| D2 | 版本成员归一 | **新增** `m_loginVersion`(quint64, 0=未认证) 与 `m_actorSessionVersion` 并存，不归一 | 后者是连接内自增审计计数（登录/登出/断线 ++，mytcpsocket.cpp:77/187/453），前者是服务端下发的**用户域**权威版本，语义不同；SEC-001 回归断言依赖原字段 |
| D3 | 每请求校验载体 | 服务端**内存版本登记表**（`MyTcpServer` 内 `QHash<QString,quint64>` + 独立 `QMutex`），登录事务提交后写入；校验=内存比对，热路径**零 DB 查询** | 权威持久源=库（重启后所有 socket 消亡，客户端重连被强制回登录页 client.h:82 forceReLogin，登记表进程内重建即可）；每请求 `SELECT login_version` 给全部业务加一跳同步 DB 往返，不可取 |
| D4 | 校验生效域 | 版本校验只对 **`m_actorAuthenticated==true` 的连接**生效，此时不区分白名单（心跳也踢）；未认证连接维持 SEC-001 原白名单语义 | 关闭"被踢客户端靠心跳永久存活"的洞（心跳在 `isPublicRequestType`:461 白名单内）；验收 §6.3 的白名单豁免指未认证态四类请求，回归断言按此写 |
| D5 | 踢旧连接手法 | 复用僵尸扫描模式：`m_tcpsocketlistMutex` 下快照目标（同名+已认证+版本≠新版），**出锁后 abort()** | `onSweepTimeout`(mytcpserver.cpp:167-183) 已有同款出锁注释（abort→disconnected→clientOffLine→removeSocket 重入同锁会死锁），照抄纪律 |
| D6 | 下线竞态守卫 | `clientOffLine`(:442) 在 `handleOffLine` 前判定：已认证且登记表[name]≠本连接版本 → 跳过 `handleOffLine`+跳过登记删除，仅清理下载/actor/socketlist | 被踢旧 socket 的断开是**预期事件**，其 online=0(:316 无条件) 会误清新会话在线态——验收 §6.5 的核心断言 |
| D7 | 版本原子分配 | 登录事务内 `UPDATE user_info SET login_version=LAST_INSERT_ID(login_version+1) WHERE name=?` → `SELECT LAST_INSERT_ID()` → 插 `login_record` → 现有 online=1 UPDATE 一并纳入同事务 | GROUP-002 D3 同款推荐手法；行锁天然串行化并发登录，版本号唯一单调；operatedb.cpp 已有 3 处 transaction 先例 |

## 2. 数据层

- `Server/migrate_ext_session_001.sql`（+`_rollback.sql`，仿 FILE-001 双文件先例）：
  - `ALTER TABLE user_info ADD COLUMN login_version BIGINT NOT NULL DEFAULT 0;`
  - `CREATE TABLE login_record (id INT AUTO_INCREMENT PRIMARY KEY, username VARCHAR(32) NOT NULL, client_id VARCHAR(64) NULL DEFAULT NULL, login_version BIGINT NOT NULL, login_time DATETIME NOT NULL, KEY idx_user_ver (username, login_version)) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;`
    （修订 2026-09-30：审计发现本行残留旧 `NOT NULL DEFAULT ''` 表述——实测 QMYSQL 空 QString 参数按 NULL 发送，实现/迁移/种子均为 NULL 可空形态，见 task.md 偏差记录；本行同步为事实。）
- `project-docs/init_test_data.sql`：user_info 建表加同列 + login_record 建表（种子不插登录行）。
- `handleLogin` 签名扩展（唯一调用点 msghandler.cpp:268）：`int handleLogin(char* name, char* pwd, const QString& clientId, quint64& outVersion)`；返回 1 时 outVersion=新版本。凭据失败路径不 +1、不写记录（登录记录=成功登录档案，验收 §6.2 措辞即此）。

## 3. 服务端改动点（文件:函数级）

1. `MyTcpServer`：新增 `QHash<QString,quint64> m_loginVersionRegistry + QMutex m_registryMutex`；方法 `setLoginVersion(user,ver)`、`currentLoginVersion(user)`、`removeLoginVersion(user)`、`closeStaleSockets(user,newVer)`（D5）。
2. `MsgHandler::login()`：D1 解析 ID → 调新 `handleLogin` → 成功时把版本经出参交连接层；响应报文**不变**（零协议）。
3. `MyTcpSocket` 登录成功 hunk(:71-83)：`m_loginVersion=outVer`；`server.setLoginVersion(...)`；`server.closeStaleSockets(...)`；顺序=登记先于踢旧（防被踢方判定竞态）。
4. `MyTcpSocket::handlMsg()` 鉴权入口(:53 后追加)：`if(m_actorAuthenticated && m_loginVersion!=server.currentLoginVersion(m_strLoginName)) → qWarning [KICK] + 回 authRejectPdu(若有响应语义) + abort() 并 return nullptr`。仅新增，不改动 SEC-001 原判定行。
5. `clientOffLine()`：D6 守卫 + 合法下线时 `removeLoginVersion(name)`（仅当本连接为权威会话）。
6. 日志面：`[KICK]`/`[LOGINVER]` 入 lcAuth，不含口令/ID 全量（截断 8 字符）。

锁顺序纪律（全任务统一）：DB 事务 → registry mutex → socketlist mutex，只允许此嵌套方向；任何两锁不并发持有即各自短临界区。

## 4. 客户端改动点

1. `Client`：QSettings 键 `client/unique_id`，首启生成 `QUuid::createUuid()` 32hex 并落盘（loadconfig 区，client.cpp:51-81）。
2. 登录发送点 `on_login_PB_clicked`(Client/client.cpp:375-381)：caMsg 在验证码 NUL 之后追加 32 字节 ID；`mkPDU(captchaBytes.size()+1)` 与 memcpy 长度相应扩 +32。
3. 被踢 UX：复用既有 `onSocketDisconnected`→(m_loggedIn)→提示文案改为含"账号已在其他设备登录或连接被服务端关闭"+ `forceReLogin` 回登录页；**不新增 PDU、不加原因通知**。

## 5. 验收映射（evidence harness，仿 sec001/entry_ops 双件套模式，py3.13）

| 证据 | 覆盖验收 |
|---|---|
| E1 双客户端互踢（A 登录→B 登录→A 业务请求被拒+连接断；login_record 两行、版本 1/2；user_info.login_version=2） | §6.1/§6.2 |
| E2 未认证白名单四类正常（登录前心跳/验证码不受版本影响） | §6.3 |
| E3 并发双登录对打（两线程同时 login，版本集={N+1,N+2} 无重复无空洞） | §6.4 |
| E4 竞态断言：B 登录后强制 A 断开，随后读库 online==1；无版本 socket 跳过下线 | §6.5 |
| E5 空 ID 登录成功且 login_record.client_id='' | §6.6 |
| E6 已认证过期连接心跳也被踢（D4 语义） | §6.3 精化 |
| E7 SEC-001 全量 48 回归 + 一对一/离线/文件冒烟 + `git diff master...HEAD` 对两 protocol.h 零命中 + pipeline 三脚本绿 | §6.7 |

## 6. 风险与对策
- R1 验证码负载兼容：**已实测关闭**——客户端登录打包本就 NUL 结尾（Client/client.cpp:376 `mkPDU(size+1)`、:381 memcpy `size+1`），D1 NUL 分隔方案一成立；老客户端（不追加 ID）剩余字节=0，解析得空 ID，兼容闭环。
- R2 登记表与库脱节：仅存登录成功路径写表；任何手工 SQL 改 login_version 不反映到运行中进程——档案语义而非访问语义，文档声明，不做轮询同步。
- R3 abort 时机与在途写：handlMsg 内 abort 后仍 return nullptr，既有 bytesWritten/下载清理路径已被 clientOffLine 覆盖（:443-449），断言 E1/E4 含下载中断场景。

## 7. 禁触清单（开发段硬边界）
两 `protocol.h`；`main.cpp` 启动区；`OperateDB::checkSchemaVersion` 相关（master 上尚不存在，预占 hunk 概念）；FILE-001 的 `finalizeUploadOnComplete`/上传 hunk（mytcpsocket.cpp:246 区）；GROUP 表域与 `message_info` 语义；新外部依赖。

## 8. C1 适配规则（与任务记录一致，再确认）
FILE-001 未合并（现状）→ 迁移=独立 SQL 文件+init_test_data 种子，**不接启动闸门**，迁移执行按 salt 先例人工/脚本。FILE-001 已合并后（trunk-sync 时点）→ 把本迁移注册进其 `schema_version` 机制：版本常量递增+迁移链加本文件+启动 fail-fast 自然覆盖；该适配是一次性、小面、语义零变更。
