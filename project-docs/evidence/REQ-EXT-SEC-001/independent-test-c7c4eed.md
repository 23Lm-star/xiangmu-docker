# REQ-EXT-SEC-001 独立测试报告（Development OS v1.7 · LARGE · independent-test 节点）

- 结论：**PASS**（A1–A5 全部成立；附 1 项非阻塞既有缺陷观察 F-1，不属本候选改动面）
- 候选提交：`c7c4eed696dc0948ae2eb9db42092aa584b50edc`（feat `2e3ff69` + 修复 `c7c4eed`）
- 实测检出：主仓库 `E:/qt/xiangmu`，分支 `codex/req-ext-sec-001-auth-context`，HEAD=`7c2e4dd`
- 与候选差异：`c7c4eed..7c2e4dd` 仅 `project-docs/tasks/REQ-EXT-SEC-001/task.md`（docs-only，产品代码与候选逐字节一致）；三脚本均绑定 HEAD=`7c2e4dd` 运行
- 测试会话：claude-code 独立测试会话（只读产品源码；未修改 Server/、Client/、pipeline/ 任何文件）
- 日期：2026-09-29（环境：Windows 11，MySQL 3306 在线，Qt 5.14.2 MinGW64）

## 1. 流水线复跑（绑定 HEAD=7c2e4dd）

| 步骤 | 结果 |
|---|---|
| `pipeline/build.ps1` | **GREEN**（Client 增量 nothing-to-do；Server 全量重编 + 链接成功） |
| `pipeline/test.ps1` | **honest-0**（0 注册项，如实报告，无伪造） |
| `pipeline/audit.ps1` | **PASS**（`audit PASS (7c2e4dd)`，RC=0） |
| frozen paths | `Client/protocol.h`、`Server/protocol.h` 改动 **0 命中**（`HEAD~1..HEAD` 为 docs 提交，亦 0；候选两提交同样 0） |
| 改动面 | `8a0bb84..c7c4eed` 产品文件 = **仅 Server/ 四文件**：`msghandler.{h,cpp}`、`mytcpsocket.{h,cpp}`；Client/、pipeline/ 零改动 |

证据：`pipeline-independent.log`（sha256 `ddbf687b…`）。前置说明：`qsqlmysql.dll` 原缺失，按交接指示自 `build-Server-…-Debug/debug/` 复制入 `build/build-Server/release/`（gitignored 运行数据，非产品源码）。

## 2. 端到端复跑（实时 Server.exe，CWD=Server/，端口 5000）

- `py -3.13 build/sec001/verify.py`：**44/44 PASS，RC=0**。
  - 输出与 dev 证据 `verify-output.log` **sha256 逐字节相同**（`e24789d3…`）——同一候选哈希、同一环境下结果可复现。
- 控制台日志（GBK→UTF-8）：`server-console-it-utf8.log`（`88803eba…`）；拒绝行提取 `reject-lines-independent.log`（**54 行**，`af2251b8…`）。
- 日志卫生：54 条拒绝/改写行仅含 `conn=/type=/路径/原因`；grep `e10adc|VICTIM-CONTENT|INDEP-MSG|INDEP-UPLOAD|cloud-disk://share|md5` = **0 命中**（无密码哈希、无令牌、无文件内容）。注：dev 证据中 `reject-lines.log` 为 0 字节，本次以独立提取覆盖该缺口。

## 3. 独立探针（不照抄 dev 断言）

`independent-probe.py`（`independent-probe2/3.py` 修正探针自身假设后全绿）：

- **P1 未认证全业务类型扫描（24 类逐一构造真实 PDU）**：5/7/9/11/13/15/17/19/21/23/25/27/29/31/33/36/38/40/46/48/52/54/56/58 全部按预期拒绝形态回包（int=-1 / bool=false / 空列表；UPLOADFILE_DATA(31) 回 ack(false)；CHAT(15) 与 ADDFRIEND_RESPOND(10) 投递型静默拒绝+审计日志），无任何一个呈现成功语义；扫描后他人文件逐字节完好。probe1 首轮 25/27，2 项 FAIL 经复核为**探针假设错误**（见 §5），probe3 修正设计后 8/8 PASS。
- **P2 白名单**：CAPTCHA(44)→45、HEARTBEAT(50)→51、REGIST(1)→2 未认证可达（白名单恰为 REGIST/LOGIN/CAPTCHA/HEARTBEAT，`isPublicRequestType` 读码核实无多无少）。
- **P3 actor 覆盖实证（登录态 A）**：
  - MKDIR 父目录=`./filesys/sec001b` → 拒绝且目录未创建；
  - MOVE `srcRel="../sec001b/secret.txt"` → `isPathInSandbox(src, userRoot)` 拒绝，源文件完好；
  - chatmsg 伪造 self=V → 服务端改写为 actor（probe3 用离线账号 V 的信箱标记做判别性验证：`伪 self=A→改写后 V 命中 B→V 消息`，若伪造生效则查 A 信箱必空）；
  - queryFilePublishLink owner 字段谎报=B → 仍按 actor=A 命中链接（found=1）；
  - 外链全链路（probe3）：A 上传 `indep.bin`→建链→**B 以裸 token 解析成功，文件落地 `filesys/sec001b/分享链接/` 且内容一致**——resolvePublishLink 的请求者目录取自服务端 `m_strLoginName`，读码+实测双确认。
- **R1/R2/G1 撤销面**：verify.py 44 用例覆盖（注销后同连接 FLUSH/DOWN 被拒、注销撤销+会话递增、断线在线列表移除、新连接无残留身份）；读码核实 `logoff(revoked)`→清 `m_strLoginName/m_actorName`+递增 `m_actorSessionVersion`（连接保留，符合 D2）、`clientOffLine()` 撤销顺序正确（先 `handleOffLine(m_strLoginName)` 后清空）。

## 4. 读码复核（候选 diff 全文 276 行逐行）

- 入口门位于 `handlMsg()` switch **之前**（mytcpsocket.cpp:53），白名单外一律要求 `m_actorAuthenticated`——权限入口收敛为单函数，无散落 case 校验（需求 §5 停止条件二满足）。
- `authRejectPdu()` 覆盖 switch 内全部请求类型：有响应语义类型逐类回失败 PDU，投递型（CHAT/ADDFRIEND_RESPOND/未知类型）返回 nullptr 仅落审计——与 §3 实测 24 类一致，无漏网。
- 存量入口身份来源：27 个 case 中所有身份参数均为 `this->m_strLoginName`（服务端派生）；PDU 的"自己/发送人/owner/接收者"字段（chat/addfriend/addfriendFin/deletefriend/chatmsg/sharefile/sharefilemove/queryFilePublishLink/logoff）一律 `copySelfField(m_actorName)` 覆盖或按 actor 生成，伪造时 `qCWarning` 记录改写；文件七入口（MKDIR/deldir/changedir/flushfile/renamefile/uploadfileinit/downloadfile）经 `pathInActorRoot`（`filesPath/<actor>` 根）收敛；movefile/listdir 基线已按用户根校验（读码核实），sharefilemove 目标目录=actor 本人根（`c7c4eed` 修复后格式串完整，无 `/%3` 残留）。
- 登录绑定：`authOk` 仅在 `ret==1` 时置位；登录失败不污染既有身份。

## 5. 发现清单

| 编号 | 级别 | 描述 | 归属 |
|---|---|---|---|
| F-1 | Low·既有 | `MsgHandler::createPublishLink` 经 `OperateDB::createShareLink` 去重：同 owner+path 已有链接时复用旧记录返回 true，但**响应仍携带新生成且未入库的 token**→该次响应的链接永不可解析（probe2 P6 复现：第二次建链→解析 ok=0）。基线 8a0bb84 即存在，不在本候选 diff 面；首次建链+解析链路正常（probe3 Q2 PASS）。 | 非本分支缺陷，建议 owner 会话另立修复项 |
| F-2 | Info·流程 | 任务书引用的 `project-docs/handoffs/req-ext-sec-001-dev-2026-09-29.txt` 不存在（master 与分支均无），dev 决策以 `task.md` 开发完成记录为准。 | 文档 |
| F-3 | Info·既有 | 登录后 `getuserinformation/finduser/onlineuser` 可查询任意用户资料/在线态——基线公开查询语义，非资源授权入口，本分支未改；并发面：`clientOffLine` 撤销与线程池 `handlMsg` 的布尔标志无原子保护，属既有架构特性（单 socket 串行分发下窗口极小）。 | 观察，不阻塞 |

## 6. 验收对照（需求 §4）

- **A1** 未登录拒绝（创建群/群消息/群成员按 D5 留待 GROUP 分支；本分支 24 类现有业务入口实测全拒）✓
- **A2** 伪造用户/上传者/路径字段无效（P1/P3 + X1/X2/X3 实测 + 读码）✓
- **A3** 注销/断线撤销（R1/R2 实测 + 撤销代码路径读码）✓
- **A4** 回归（44/44 + K1 上传/下载/分享/重命名/穿越回归）✓
- **A5** 证据绑定候选哈希（本报告与全部日志位于 `c7c4eed696…/` 目录；pipeline/verify 内嵌短哈希 `7c2e4dd`，与候选差异仅 docs 提交，已声明）✓

## 7. 证据清单（本目录，sha256）

```
c7c4eed696dc0948ae2eb9db42092aa584b50edc  ← 绑定候选
ddbf687b… pipeline-independent.log   e24789d3… verify-independent.log（=dev 逐字节一致）
196ba380… probe-independent.log      5df5e63e… probe2-independent.log
113a36c5… probe3-independent.log     af2251b8… reject-lines-independent.log（54 行）
88803eba… server-console-it-utf8.log 66f54d6e… / 83b28c31… / 58fd21e3… 探针脚本
```

## 8. 下一步

进入 independent audit 节点（新会话、只读）；F-1 建议纳入修复队列但不阻塞本分支合并评估。
