# REQ-EXT-SEC-001 服务端身份与授权上下文 — 任务记录

## 档位
- **LARGE**（goal 节点定档，2026-09-29）：架构前置门，改变服务端授权模型（连接级 actor_context 成为所有业务入口的唯一身份来源）；跨会话（本分支之后，所有群组/秒传/群文件分支依赖它）；不触碰 `protocol.h`（frozen_paths 零命中为本分支硬约束）。需求文档自声明"LARGE 前置门"。
- 流程版本：Development OS v1.7。分支：`codex/req-ext-sec-001-auth-context`，基线 master@`8a0bb84`。
- 节点计划：goal（本会话，任务归纳+主人批准）→ design（计划文档已随本记录落分支，批准即视为 design 完成）→ development → independent test（新会话）→ repair → independent audit（新会话、不同代理、只读）→ integration（三事实行）→ merge no-ff。

## 依赖核查（2026-09-29 goal 节点实测）
| 依赖 | 状态 |
|---|---|
| 登录基线 | ✅ `MsgHandler::login()` 成功后写 socket 级 `m_strLoginName`；`MyTcpSocket` 已有 `m_connectionId`（服务端生成）——连接身份可区分，**文档 §5 停止条件一不触发，无需改协议** |
| 注销基线 | ⚠️ `logoff()` 目前用**客户端 PDU 里的用户名+密码**判身份并删用户目录——正是本需求要收编的"客户端提交即授权"反面样板 |
| 路径穿越防护 | ✅ REQ-FS-003 已合并 master@`f937c90`（`isPathInSandbox` 统一校验 + renamefile 名称段 `fa88427` 修复，复审 PASS）；`build/evidence/fix-path-traversal-and-ownership/f808daf/` 下的 pending 报告为已被取代的初审 FAIL，非活动阻塞 |
| 日志分级基线 | ✅ REQ-LOG-007 已合并 @`01ccf89`，拒绝日志复用 `lcAuth/lcFs/lcNet` 分类 |

## 现状盘点（代码事实，master@8a0bb84）
- `Server/mytcpsocket.cpp handlMsg()` 分发 ~27 个 case：**仅 9 个**入口把服务端连接级 `m_strLoginName` 传入处理器（login/onlineuser/flushfriend/deldir/renamefile/movefile/listdir/createPublishLink/resolvePublishLink）；其余业务入口（MKDIR、changedir、flushfile、uploadfileinit、UPLOADFILE_DATA、sharefile、sharefilemove、downloadfile、chat、finduser、addfriend、addfriendFin、deletefriend、getchatmsg、getuserinformation、queryFilePublishLink、logoff）**既不校验登录态，身份/发送者/路径也取自客户端 PDU 字段**。
- `chat()`：发送人 `seedName` 取自 caData（客户端可伪造任意发送者）。
- 文件类入口：`caMsg` 携带客户端拼装的完整路径串，`isPathInSandbox` 只约束沙箱根、不约束"只能进自己的用户目录"——横向越权（拿他人用户名拼路径）当前无服务端拦截。
- `clientOffLine()` 与注销：未见对 `m_strLoginName` 的连接级撤销动作（design 时逐行确认）。
- 白名单（天然公开，无需登录态）：REGIST、LOGIN、CAPTCHA、HEARTBEAT。

## 开发任务分解（待批准）
- **T1 会话内核**：`MyTcpSocket` 建立 actor_context（用户名 + 已登录标志 + 连接标识 `m_connectionId`/会话版本）；`login()` 成功后置位；`logoff()` 改为**仅凭连接级身份**受理（不再信任 PDU 用户名/密码作为授权依据），成功后撤销；`disconnected/clientOffLine` 撤销。
- **T2 统一入口门**：`handlMsg()` 白名单（REGIST/LOGIN/CAPTCHA/HEARTBEAT）之外一律要求已认证上下文，未登录拒绝并回明确错误响应；为群组/秒传分支预留可复用的校验入口（一个函数，不散落 case）。
- **T3 存量入口收编**：T2 门落地后，把仍以 PDU 字段作身份语义的点改为服务端派生——`chat()` 发送人以 actor 覆盖校验；`logoff()` 依 T1；其余已传 `m_strLoginName` 的入口复核不变。
- **T4 规范路径**：文件类入口的 `caMsg` 路径统一解析为"服务端按 actor 生成的用户根目录下的相对路径"（保留 `isPathInSandbox` 作纵深防御）；越出本人用户根一律拒绝。客户端 `Client/file.cpp` 等配合改为相对路径语义，保证 A4 回归。
- **T5 拒绝审计**：每类拒绝记录原因 + 请求关联标识（连接标识 + 报文类型），走 `qCWarning` 对应分类；不落密码/令牌/文件内容。
- **T6 负向测试与回归（证据计划）**：仿 REQ-FS-003 模式——构造恶意/未认证 PDU 的端到端脚本 + `server-reject-lines.log`；覆盖验收 A1（未登录拒绝五类操作，群组类以"入口不存在则留待 GROUP 分支补测"记录在案）、A2（伪造用户/发送者/路径字段无效）、A3（注销/断开后旧连接被拒）、A4（正常单聊、上传下载、路径防护回归）；A5 全部证据绑定候选提交哈希（`pipeline/build.ps1`/`test.ps1` 运行时自动落 hash）。`pipeline/test.ps1` 现为 0 注册项，负向脚本按 TEST-008 模式暂以证据文件承载。
- **T7 收尾**：任务记录+证据归档 `project-docs/evidence/REQ-EXT-SEC-001/`；BRANCH-ORDER 补一行登记（扩展 E 阶段首支）。

## 允许路径
`Server/mytcpsocket.{h,cpp}`、`Server/msghandler.{h,cpp}`、`Server/mytcpserver.{h,cpp}`（修复节点纳入：M-1 握手登记需要连接注册表单例承载共享状态，属阻塞验收的修复提交，同分支同 owner）、`Server/server.cpp`（仅只读引用，预期不改）、`Server/operatedb.cpp`（如需在线态配合，goal 预判不改）、`Client/file.cpp`、`Client/msghandler.cpp`、`Client/client.cpp`（仅 T4 相对路径配合）、`project-docs/tasks/REQ-EXT-SEC-001/`、`project-docs/evidence/REQ-EXT-SEC-001/`、handoff。
**禁止**：两 `protocol.h`（frozen，改结构/枚举编号即违规）、`Server/filesys/` 运行数据入库、新外部依赖、顺手加群组逻辑。

## 待主人批准的决策（approval card）
- **D1 门覆盖范围**：白名单外**全部**存量业务入口统一加登录门（推荐，A1/停止条件二的要求）vs 仅新增群/文件入口（不满足验收，不可行）。
- **D2 注销后连接处置**：撤销认证上下文但**保持连接**（推荐，客户端注销后回登录页可复用连接）vs 注销即断连（行为变化大）。
- **D3 T4 路径语义改造深度**：服务端强制"actor 用户根 + 相对路径"（推荐，A2 根治）需要同时最小化改客户端路径拼装；vs 仅加"路径必须落在本人用户根"校验（服务端独改，客户端零改动，但规范相对路径要求打折扣）。
- **D4 logoff 破坏性**：现逻辑注销即删用户目录（`removeRecursively`）。本分支只把**授权依据**改为连接级身份，删目录行为保留不变（推荐，范围守恒）vs 顺带停删目录（超出范围，不做）。
- **D5 群组验收 A1 的延后声明**：群入口本分支不存在，A1 的"创建群/群消息/群成员"三条在本分支以机制就绪 + GROUP 分支补测记录，本分支先以现有五类未登录拒绝实测（推荐）。

## 开发节点完成记录（2026-09-29，owner: claude-code）
- 候选提交：`c7c4eed`（feat 主体 `2e3ff69` + 自审修复）。改动仅 4 个服务端文件；**两 protocol.h 零触碰、Client/ 零改动**（合法客户端路径本就落在自身用户根内，D3 在纯服务端达成，"客户端最小配合"实际为零）。
- 实现要点：登录成功→连接绑定 actor+会话版本；白名单外入口统一门（`isPublicRequestType`/`authRejectPdu`，含逐类型失败响应）；chat/addfriend/addfriendFin/deletefriend/chatmsg/sharefile/sharefilemove/queryFilePublishLink/logoff 的"自己"字段一律服务端 actor 覆盖（chat 转发报文同步改写，接收方看到的是真发送人）；文件七入口（MKDIR/deldir/changedir/flushfile/renamefile/uploadfileinit/downloadfile）路径收敛 `filesPath/<actor>` 本人根；logoff/disconnect 撤销并递增会话版本。
- 自审发现并修复：sharefilemove 目标路径遗留 `/%3` 占位符（格式串实参不足）→ 修复于 `c7c4eed`。
- 流水线（绑定 `c7c4eed`）：build GREEN / test honest-0 / **audit PASS**。
- 端到端（`build/sec001/verify.py`，py -3.13，实时服务端+控制台日志）：**44/44 PASS**，覆盖 G1 未认证拒绝×10、L1 登录后本人操作、X1 伪造路径×7（他人文件完好）、X2 伪造发送者（转发署名=actor）、X3 伪造注销目标（他人账号与目录毫发无损）、R1 注销后同连接被拒+本人目录按既有行为删除、R2 断线在线列表移除、K1 上传/下载/分享/重命名/穿越回归。拒绝日志仅含 conn/type/原因，grep 确认无密码哈希（e10adc… 0 命中）。
- 证据：`build/evidence/codex-req-ext-sec-001-auth-context/c7c4eed…/`（manifest-sha256 `c56e2035…`，含 verify.py / server-console.log / reject-lines.log / pipeline-run.log / test 日志）。
- 群组相关验收项（建群/群消息/群成员）按 D5 移交 GROUP 分支补测；秒传"命中"语义待 FILE 分支。
- 测试数据说明：harness 创建 sec001a/b/r/v 测试账号（密码 123456，filesys/ 为 gitignored 运行数据）；r 已注销删除，其余留存。
- 节点交接：dev 关闭 → **independent test 需新会话、只读，在 `c7c4eed` 复跑流水线与全部负向用例**。

## 停止条件（需求 §5 对照）
- 连接身份可区分：已核实成立（socket 对象 + 服务端 `m_connectionId`/`m_strLoginName`），无需协议变更。
- 权限入口分散：T2 收敛为单一门函数，审计不得接受客户端隐藏按钮作证据——验收只认服务端拒绝日志与哈希绑定流水线报告。
- 协议编号：本分支零修改两 `protocol.h`；若开发中发现必须新增字段，停止并回 goal 转 LARGE 协议变更。

## 独立测试节点记录（2026-09-29，测试会话=新会话只读）
- 结论 **PASS**：pipeline 三脚本复跑 GREEN/honest-0/PASS（frozen 零命中，改动面=Server/ 四文件）；`verify.py` 44/44 且输出与 dev 证据逐字节同哈希（可复现）；独立探针未认证 24 类 PDU 全拒、actor 覆盖判别验证、外链全链路落本人根；拒绝日志 54 行无密码/令牌/文件内容。
- 证据：`build/evidence/codex-req-ext-sec-001-auth-context/c7c4eed…/independent-test-report.md`；handoff `project-docs/handoffs/req-ext-sec-001-test-2026-09-29.txt`（master）。
- **F-1（既有缺陷，不在本分支 diff 面，不阻塞）**：`createPublishLink` 同 owner+path 去重复用旧 hash 却回新生成未入库 token → 该次链接永不可解析（基线 `8a0bb84` 即存在，测试探针复现）。按治理应另立 SMALL 分支修复，待主人定夺。
- F-3（Info，既有架构）：登录资料/在线查询为基线语义；`clientOffLine` 与线程池写回对撤销标志无原子保护，窗口极小。
- 下一步：independent audit（新会话只读）→ integration 三事实行 → merge no-ff。

## 审计 + 修复节点记录（2026-09-29）
- 独立审计（c7c4eed）：**PASS（允许合并）**，无 High 阻塞；报告 `build/evidence/.../c7c4eed…/audit-report.md`。A1/A3/A4/A5 PASS；A2 带星标 PASS*，例外即下述 M-1。改动面仅 Server/ 四文件+文档，两 protocol.h、Client/ 零命中。
- **M-1 修复（本分支，新候选 `eb0ecc1`）**：`sharefilemove` 源路径原仅默认沙箱根（基线遗留，分支注释明示维持）——已认证接收者伪造 `AGREE_RESPOND` 可把沙箱内任意他人已存在文件拷进本人目录=横向读取，违背需求 §2"路径字段不作授权依据"、A2。**根因**：分享是无状态转发，接收者同意时无从校验"该源是否被分享给自己"。**修复**：`sharefile()` 以服务端 canonical 对每位接收者登记握手授权（新增 `MyTcpServer::{add,has,consume}PendingShare`，30min TTL/上限1024/一次性消费）；`sharefilemove()` 仅放行有登记的 (actor, canonical源)，复制改以服务端 canonical 执行（顺带闭合该入口 verify-then-act）。未授权/重放/无登记源一律拒绝。**允许路径同步纳入 `mytcpserver.{h,cpp}`**（阻塞验收的修复提交，同分支同 owner）。
- 新候选流水线：build GREEN / test honest-0 / **audit PASS（@eb0ecc1）**。
- 新候选端到端：**47/47 PASS**（原 44 + K2 三例：K2a 未分享给 B 的源伪造同意被拒且无落地 / K2b 正常握手同意落地成功 / K2c 重放同意被拒一次性消费）。证据 `build/evidence/.../eb0ecc1…/`（manifest-sha256 851b6545…）。
- **F-1（既有，不在本 diff）/I-4（既有 recvmsg DEBUG）** 按治理另立 SMALL 分支，不随本需求。
- 待办：**增量复审 `c7c4eed..eb0ecc1`**（新会话只读）→ 三事实行 → merge no-ff。原 c7c4eed 审计结论已被修复超越（修复面为 M-1 一处，需再确认未引入新横向读取且握手登记无绕过）。

## B-1 修复轮记录（2026-09-29，owner）
- 增量复审（eb0ecc1，会话只读）结论 **拒绝（REJECT）**：**B-1 High——M-1 修复可绕过**。绕过链：认证攻击者用 `SHAREFILE` 把**他人文件**"分享给自己"→ sharefile() 登记不校验源归属 → 攻击者再发 `AGREE_RESPOND` 自我同意 → 完成与 M-1 等价的横向复制。K2a 只测"无登记的伪造同意"，未覆盖"自我登记"。原审计处方 `(sharer,receiver,src)` 三角绑定在实现中被简化成 `(receiver,src)` 且登记侧零校验，属修复偏离设计。
- **修复（新候选 `0efccb9`）**：`sharefile()` 登记/转发前校验源必须落在**分享人本人用户根**（`pathInActorRoot`）——只能分享自己的文件；源不存在或属他人 → 不登记、不转发邀请、拒绝日志带 conn。同意侧沿用 (actor, canonical) 一次性消费。合法分享语义不变（本人文件→他人同意可跨根复制=设计使然）。
- 新用例 **K2d**：自我登记他人源→自我同意被拒且零复制（协议面验证）。
- 流水线 @`0efccb9`：build GREEN / test honest-0 / audit PASS。端到端 **48/48 PASS**（干净注册表：服务端重启后跑，登记不跨进程）。证据 `build/evidence/.../0efccb9…/`（manifest-sha256 3407792b…；控制台 grep 密码哈希=0）。
- 过程教训（记录在案）：握手登记表是进程内存态——端到端脚本必须独占全新服务端进程运行，否则跨轮残存登记/落盘目标会污染断言（本轮 K2 三连 FAIL 即此因，非产品缺陷）。
- F-1 已另立 SMALL 分支修复并合入 master（`4ae532e`→merge `fad502b`，closeout `628e491`，e2e 5/5）。功能分支合并前需先 `git merge master` 预解 `msghandler.cpp` createPublishLink 相邻区域冲突并重跑三脚本。
- 待办：**增量复审 `eb0ecc1..0efccb9`**（重点：B-1 关闭 + 源归属校验的绕过面 + 邀请转发被拒时对合法客户端的可见行为）→ 三事实行 → 功能分支 merge master 预解冲突 → merge no-ff。

## Integration / 合并记录（2026-09-29，owner）
- 三事实行核验（合并前）：独立审计链存在且被接受（c7c4eed PASS*→eb0ecc1 REJECT/B-1→0efccb9 ACCEPT）；frozen-path diff `628e491..7fc47c1` 对两 protocol.h、Client/ 零命中；pipeline 在合并结果上实测 GREEN / honest-0 / PASS。
- 功能分支先 `git merge master`（携入 F-1 修复 `4ae532e`，自动合并零冲突，`dc06365`）→ 三脚本重跑绿 → **no-ff 合入 master @`7fc47c1`**。
- 合并结果再跑 48/48 端到端（全新服务端、干净握手注册表）：`project-docs/evidence/REQ-EXT-SEC-001/verify-48-final-7fc47c1.log`；三事实与流水线汇总 `pipeline-run-final-7fc47c1.log`；manifest `manifest-final-7fc47c1.txt`（0ebb50c4…）。
- 证据归档：`project-docs/evidence/REQ-EXT-SEC-001/`（INDEX.md 按时间列出审计链与 e2e）。
- 交付面：`Server/msghandler.{h,cpp}`、`Server/mytcpsocket.{h,cpp}`、`Server/mytcpserver.{h,cpp}`；Client/ 与两 protocol.h 零改动。
- 遗留（另立分支，不属本需求）：F-1 ✅ 已修已合并（`fad502b`）；I-4 recvmsg DEBUG 落密码哈希（基线既有，Low）；登记表按 actor 配额（Low-Med）；revUserCnt 钳制（Low）；O-3 char[32] 无终止符（FS-003 观察）。
- 下游：群组/秒传/群文件分支（REQ-EXT-GROUP-*/FILE-*）现可直接复用 actor 门与 `pathInActorRoot`/握手登记语义。
- push 未做（待主人指令）。
