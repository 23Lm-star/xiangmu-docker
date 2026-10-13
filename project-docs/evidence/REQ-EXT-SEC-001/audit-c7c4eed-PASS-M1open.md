# REQ-EXT-SEC-001 独立审计报告（Development OS v1.7 · LARGE · independent-audit 节点）

- **结论：PASS（允许合并）** — 无 High 阻塞项；附 1 项 Medium（M-1，既有信任点残留，须在群文件/秒传分支消费本门之前修复或补测）
- 候选提交：`c7c4eed696dc0948ae2eb9db42092aa584b50edc`（feat `2e3ff69` + 修复 `c7c4eed`），基线 master@`8a0bb84`
- 审计会话：claude-code 独立审计（审计者≠开发者≠独立测试者），全程只读产品源码与 Git 历史；未修改 Server/、Client/、pipeline/、project-docs/ 任何已跟踪文件，未 commit/push
- 方法：detached worktree 逐行读候选源码（`git worktree add --detach build/wt-audit-sec001 c7c4eed`，已删除）+ 逐条 claim→fact 核验 + 哈希/日志实证复算，不照抄 dev/test 结论
- 日期：2026-09-29

## 0. 独立性声明

本审计在候选提交自身的 detached worktree 上读源码（不依赖主仓工作树），实测 diff、sha256、日志提取全部本会话独立执行。codegraph 仅作依赖面交叉核对，判定证据以 worktree 源码 file:line 为准。

## 1. 改动面核验（diff → fact）

| 声明 | 实测 | 判定 |
|---|---|---|
| 仅 Server/ 四文件 | `git diff 8a0bb84..c7c4eed --name-only` = `Server/msghandler.{h,cpp}`、`Server/mytcpsocket.{h,cpp}` + `project-docs/tasks/REQ-EXT-SEC-001/task.md`（goal 节点 c1770dd 的过程文档，非产品代码） | ✓ |
| protocol.h frozen | `git diff 8a0bb84..c7c4eed -- Client/protocol.h Server/protocol.h` 输出为空 | ✓ 零命中 |
| Client/ 零改动 | `git diff 8a0bb84..c7c4eed -- Client/` 输出为空 | ✓ |
| 分支 HEAD 一致性 | 主仓分支现 HEAD=`abdd7d6`（测试报告记录为 7c2e4dd）；`git diff c7c4eed..abdd7d6 -- Server/ Client/ pipeline/` 为空，全程新增仅 task.md 文档两次追加 → 产品代码与候选逐字节一致 | ✓ |
| codegraph 交叉核对 | `handlMsg→MsgHandler::login→OperateDB::handleLogin` 链、27-case 分发面、MyTcpSocket 唯一调用方为 recvmsg——与读码一致，无遗漏入口 | ✓ |

## 2. 验收标准逐条判定（A1–A5）

### A1 未登录拒绝 —— **PASS**
- 统一门位于 `handlMsg()` switch 之前：`Server/mytcpsocket.cpp:53`（`!isPublicRequestType && !m_actorAuthenticated` → 拒绝+审计+失败响应），权限入口收敛单函数，需求 §5 停止条件二（不接受"客户端隐藏按钮"作证据）满足。
- 白名单恰为 REGIST/LOGIN/CAPTCHA/HEARTBEAT 四类，无多无少：`mytcpsocket.cpp:461-472`。
- UPLOADFILE_DATA（31）门内特判回 `ack(false)`：`mytcpsocket.cpp:56-59`；uploadfileinit/downloadfile 等经 `authRejectPdu` 回失败 PDU（`mytcpsocket.cpp:483-536`）。
- 认证置位仅在服务端凭据校验成功：`mytcpsocket.cpp:74-81` ← `msghandler.cpp:270-273`（`authOk` 仅 `handleLogin` 返回 1）；`m_actorAuthenticated` 默认 false（`mytcpsocket.h:23`），无客户端可写路径。
- 群组三类入口本分支不存在（D5 延后声明成立，机制就绪）。
- 实测旁证：`reject-lines-independent.log` 54 行 `[AUTHGATE]` conn/type/session 逐类拒绝记录（本会话 wc -l 复算=54，sha256 `af2251b8` 对上报告声明）。

### A2 伪造字段无效 —— **PASS\*（部分成立，见 M-1）**
逐 case 对 27 个入口核验身份/路径来源（worktree 源码）：
- `copySelfField(…, m_actorName)` 覆盖"自己/发送人/同意人/查询者/分享人"：chat `msghandler.cpp:420`、chatmsg:1000、addfriend:323、addfriendFin:355、deletefriend:394（caData+32 发起人段）、sharefile:684、queryFilePublishLink owner=actor:1171-1174、logoff 仅按 actor 受理:115-136。伪造均记 `[addfriend/chat/...] 按连接身份改写` 审计。
- 服务端派生参数入口：onlineuser/flushfriend/deldir/renamefile/movefile/listdir/createPublishLink/resolvePublishLink 均传 `this->m_strLoginName`（`mytcpsocket.cpp:89-157,203-207`）。
- 文件入口 actor 根收敛（`pathInActorRoot`→`isPathInSandbox(path, filesPath/<actor>)`，`msghandler.cpp:156-171`）：MKDIR:471、deldir:496、changedir:536、flushfile:574、renamefile:621、uploadfileinit:901、downloadfile:971 ✓ 七入口全走。
- resolvePublishLink 落地目录=requester（服务端 loginName）`msghandler.cpp:1117-1118` ✓。
- **例外（M-1）**：sharefilemove 源路径 `sourcePath` 仍只过默认根 `isPathInSandbox`（`msghandler.cpp:732`，条件自基线 8a0bb84 未变，分支注释 `msghandler.cpp:709-710` 明示"维持沙箱校验即可"）；且 sharefile 提案路径同样未绑定分享人根。→ 见发现清单 M-1。
- 其余 caData/caMsg 中的用户名字段经核对均为**目标/对端**（好友名、收件人、peer）或公开查询参数（finduser/getuserinformation，基线公开语义 F-3），非授权依据。

### A3 注销/断线撤销 —— **PASS**
- 注销成功撤销：`mytcpsocket.cpp:184-192`（`revoked` → `m_actorAuthenticated=false; ++session; m_strLoginName.clear(); m_actorName.clear()`，连接保留符合 D2）；`revoked` 仅在 DB 删除成功置位（`msghandler.cpp:146`）；logoff 的 PDU 用户字段仅做不符告警、密码为本人二次确认（`msghandler.cpp:128-136`）。
- 断线撤销：`clientOffLine()` `mytcpsocket.cpp:442-458`，顺序正确——先 `handleOffLine(m_strLoginName)` 用旧身份置离线，再清标志/名/session 递增；disconnected 信号绑定 `mytcpsocket.cpp:21`。
- 撤销后旧连接再发业务请求被 A1 门拒绝（session 递增可在审计日志区分）。

### A4 回归 —— **PASS**（独立复跑 + 读码）
- `verify-independent.log` 与本会话复算 sha256=`e24789d3…`，与 dev `verify-output.log` **逐字节相同**，44/44 PASS/RC=0（单聊、上传/下载、分享、重命名、穿越防护回归在内）。
- FS-003 语义保留：`isPathInSandbox` canonical 归一实现原样（`msghandler.cpp:440-458`）；deldir 先校验后删除顺序保留（:494-498）；renamefile 名称段分隔符拒绝保留（:625-629）。
- `c7c4eed` 修复实证：sharefilemove 目标路径现为 `QString("%1/%2").arg(recvDir).arg(fileName)`（`msghandler.cpp:728`），旧基线 `"%1/%2/%3".arg(filesPath).arg(pdu->caData)…` 的悬空 `%3` 与客户端可控接收目录一并消除（diff 段核对）。

### A5 证据哈希绑定 —— **PASS**
- 证据目录名与 `manifest.txt` `commit:` 行均为 `c7c4eed696…`，`dirty: no`（本会话读取核对）。
- pipeline/verify 运行于 `7c2e4dd`：`pipeline-independent.log` 内嵌 `7c2e4dd5b0…` 全哈希（grep 复算），`c7c4eed..7c2e4dd` 及现 HEAD `abdd7d6` 均仅 task.md 文档增量，产品代码与候选一致——绑定关系成立且测试报告已如实声明。
- 独立复算：pipeline-independent.log=`ddbf687b…`、reject-lines-independent.log=`af2251b8…`，与报告 §7 声明前缀一致。

## 3. authRejectPdu 覆盖核验

switch 内 25 个非白名单业务类型逐一对照：23 类回对应失败 RESPOND（caData 清零= false/空列表/-1），投递型 2 类（CHAT、ADDFRIEND_RESPOND）显式 `return nullptr` 仅落审计（`mytcpsocket.cpp:531-533`，与正常路径同为无响应语义——正常 `chat()`/`addfriendFin()` 亦返回 nullptr，无客户端挂起），UPLOADFILE_DATA 在门内以 ack(false) 特判。**无漏网类型**，不触发"静默 nullptr→挂起"的 Low 项。

## 4. 安全视角排查

- 拒绝/改写日志仅含 conn/type/session/路径/原因；grep `e10adc|cloud-disk://|md5|VICTIM-CONTENT` 于 54 行拒绝日志 = 0 命中（本会话复算）——无密码、无令牌、无文件内容。
- `copySelfField` 边界：memset 32 + memcpy ≤31 字节（`msghandler.cpp:28-33`）；actor 名源于 `fixedString(caData,32)`（≤31 字节 NUL 截断），入库名同界 → 无越界。
- verify-then-act：MKDIR/deldir 校验 canonical 后在原串执行操作——FS-003 已接受的既有模式，非本分支引入（Info I-2）。
- `m_actorAuthenticated`/`m_strLoginName` 布尔与串无原子保护，依赖单 socket 事件循环串行分发——既有架构特性（F-3 同源），窗口极小（Info I-3）。
- `recvmsg()` DEBUG 级打印原始 PDU caData/caMsg（`mytcpsocket.cpp:427-433`）——LOGIN 请求的 caData+32 为密码哈希串，若开 DEBUG 级别落盘会入库；**基线既有、不在本 diff**，级别控制默认不输出（Info I-4）。

## 5. 发现清单

| 编号 | 级别 | 描述 | 证据 | 处置 |
|---|---|---|---|---|
| M-1 | **Medium** | sharefilemove 源路径 + sharefile 提案路径未绑定分享人：`sourcePath` 仅过默认沙箱根（基线原样保留，本分支注释明示该设计决定）；已认证用户可伪造 `SHAREFILE_AGREE_RESPOND`（或诱导同意他人提案），将 `filesys/` 内**任意他人已存在文件**复制进本人目录——横向读取，违背需求 §2"物理路径…不能作为授权依据"与 A2 全量语义。dev 与独立测试的 A2 探针（MKDIR/MOVE/chatmsg/owner/落地目录）均未覆盖此项。修复需服务端握手套接（pending-share 记录 (sharer,receiver,srcPath)，agree 时比对 actor 与登记），可在本分支允许路径内完成、无需改协议。 | `msghandler.cpp:709-710,732`；diff `8a0bb84..c7c4eed` 该条件行未变 | **不阻塞合并**：条件基线即存在（本分支严格改善：目标端由客户端可控改收 actor 根）、攻击面非本分支新增、且与 F-1 同属"既有缺陷另立修复"先例。**合并后必须**在首个群文件/秒传分支接入本门之前修复并补一条负向探针（伪造 sourcePath→拒绝且文件不落地） |
| L-1 | Low | logoff 二次确认密码经 `handleLogoff(actor, userPwd)` 比对失败时（D4 决定保留删目录行为）若密码错但账号被并发删除等 DB 边界，revoked 不置位、连接保留——行为正确，仅提示：删用户目录不可逆属产品高危面，建议后续分支评估 | `msghandler.cpp:133-147` | 记录，不阻塞（D4 批准范围内） |
| F-1 | Info·既有 | `createPublishLink` 去重复用旧记录返回 true 但响应携带未入库新 token → 该链接永不可解析。本 diff 未触碰该函数（`git diff … | grep createPublishLink` 仅 context 行），基线 `8a0bb84` 引入 | `msghandler.cpp:1046-1068` | 确认不在改动面；建议另立 SMALL 分支 |
| I-2 | Info·既有 | verify-then-act 于原串（canonical 校验后 mkdir/remove 用 raw path）——FS-003 复审已接受的同模式延续 | `msghandler.cpp:471-474,496-505` | 不判违规 |
| I-3 | Info·既有 | 认证标志无原子保护，依赖串行事件循环 | `mytcpsocket.h:23` | 观察 |
| I-4 | Info·既有 | DEBUG 级 recvmsg/seedMsg 落原始 PDU（含登录密码哈希字节） | `mytcpsocket.cpp:427-433` | 建议 owner 会话将 DEBUG 生产默认关闭（独立 SMALL，非本分支） |

## 6. 判定汇总

| 验收 | 判定 | 关键证据 |
|---|---|---|
| A1 | **PASS** | mytcpsocket.cpp:53,461-472,475-536；拒绝日志 54 行 |
| A2 | **PASS\***（M-1 残留除外） | copySelfField 8 处 + actor 参数 9 处 + pathInActorRoot 7 入口；msghandler.cpp:732 例外 |
| A3 | **PASS** | mytcpsocket.cpp:184-192,442-458 |
| A4 | **PASS** | e24789d3 逐字节复现 44/44；FS-003 保留；%3 修复落地 :728 |
| A5 | **PASS** | manifest=候选哈希；docs-only 差异经本会话 diff 证实至现 HEAD |

**结论：PASS，允许按 no-ff 合并。** 附带义务：M-1 须在首个消费本授权门的群文件/秒传分支开工前修复（本分支或紧随 SMALL 分支），并补伪造 sourcePath 负向探针；F-1、I-4 建议纳入修复队列。

## 7. 哈希绑定声明

本报告全部判定基于候选提交 `c7c4eed696dc0948ae2eb9db42092aa584b50edc` 的 detached worktree 源码（file:line 见各节）；引用的既有证据：manifest.txt（commit=c7c4eed，dirty no）、pipeline-independent.log（sha256 `ddbf687b18b3…`）、verify-independent.log/verify-output.log（均 `e24789d315a2…`）、reject-lines-independent.log（54 行，`af2251b8eb59…`）——sha256 为本会话独立复算，与独立测试报告声明一致。审计用 worktree `build/wt-audit-sec001` 已于报告写毕后删除（见本会话操作记录）。
