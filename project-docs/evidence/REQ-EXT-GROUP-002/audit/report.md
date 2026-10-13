# REQ-EXT-GROUP-002 独立审计报告（independent audit）

- 节点：independent audit（Development OS v1.7 / LARGE，新会话、不同代理、只读）。
- 审计对象（test 候选）：`03dc01d`（`fix(req-ext-group-002): correct pull cursor page boundary on both sides`），完整哈希 `03dc01d86a38216f06c5c307cedaef718294601b`。
- 审计快照：`git worktree add --detach build/wt-audit-group002 03dc01d`；本节点**零提交、零生产源码改动**（`git status --porcelain` = 0 条），工作树用完删除（见 cleanup_status）。
- 审计者：claude-code（与 development owner 同一代理名，但**独立新会话**；test 节点亦为新会话。若流程要求跨代理，请主人裁决——本报告不掩盖该事实）。
- 审计边界：只读检查差异、三门报告、独立测试证据与 CodeGraph 影响闭包；未修改生产源码、测试标准、验收条件、分支历史；未推送、未合并。
- 结论：**PASS** —— 候选范围内 0 条阻塞缺陷；test 节点结论未被推翻，且未发现越界或伪造证据。

## 输入证据

- test 交接：`project-docs/handoffs/req-ext-group-002-independent-test-2026-10-01.txt`（`node: test`，`audit: none`、`blockers: none`、结论 PASS 0 条归因缺陷）。
- test 报告：`project-docs/evidence/REQ-EXT-GROUP-002/independent-test/report.md` + 17 件证据（自研 harness、part{1,2,3} 运行日志、机器可读 JSON、GROUP-001 回归日志、服务端控制台、pipeline 日志、record-evidence manifest）。
- development 证据：`project-docs/evidence/REQ-EXT-GROUP-002/`（verify.py、verify-output.log 38/38、evidence.json、server-console.log、manifest.txt）。
- 任务与设计：`project-docs/tasks/REQ-EXT-GROUP-002/task.md`、`design.md`（§11 允许路径、§12 验证矩阵、D2 编号表）。

## 逐项核验

### 1. 冻结路径：两份 `protocol.h` 仅追加 88–96（D2 批准范围）— FACT，独立复证

test 节点已人工复核；本次用**独立方法**（枚举 `name→value` 全表比对，非读 diff 文本）复证：

- `Client/protocol.h`、`Server/protocol.h` 两侧：`<=87` 的既有枚举 name→value 映射**完全一致**（无删除、无重排、无改值）；
- 新增恰为 88–96 九个枚举（`GROUP_SEND_MESSAGE_REQUEST` … `GROUP_UNREAD_SUMMARY_RESPOND`），两份文件同值同名；
- Server 侧唯一删除行是旧注释 `// 88-95 预留给 REQ-EXT-GROUP-002 群消息，不在本需求实现`（占位注释，非协议事实）。
- 原始输出：`independent-verification.log` 第 1 节。

**判定：符合 D2 批准范围（只追加、不重排 1–87）。**

### 2. 差异范围与路径边界 — FACT

`git diff master...03dc01d --stat` = 21 文件 / +1440 −8。生产侧全部落在 design §11 允许面内：
`Server/operatedb.{h,cpp}`、`msghandler.cpp`、`mytcpserver.{h,cpp}`、`mytcpsocket.{h,cpp}`、两份 `protocol.h`、`migrate_ext_group_002.sql`(+rollback)、`Client/client.cpp`、`Client/Client.pro`、新增 `Client/groupchat.{h,cpp}`。
未触碰：`message_info` 一对一语义（`seedchatMsg`/`getchatMsg` 行区间零 diff）、`Server/filesys/`、第三方目录、新增外部依赖、`Makefile`/`.pro.user` 等卫生禁项（audit.ps1 卫生检查静默通过）。

### 3. 三门与证据对账 — FACT

| 门 | 证据 | 结果 |
|---|---|---|
| build（dev 主树 `03dc01d`） | `build/evidence/current/build-03dc01d-20261001-102704.log` | GREEN |
| build（test 工作树独立构建 `03dc01d`） | `build-03dc01d-20261001-104114.log` + manifest（`commit: 03dc01d…`、`dirty: no`） | GREEN |
| test（`pipeline/test.ps1`） | `test-03dc01d-20261001-105033.log`（54B）+ manifest（`03dc01d…`、`dirty: no`） | honest-0（0 注册测试，test 未将其冒充 GREEN 依据，正确） |
| audit（`pipeline/audit.ps1`，本节点执行） | `audit-run-fresh.txt` / `audit-run-with-evidence.txt` | 见下 |

`pipeline/audit.ps1` 两次运行：
1. 全新 detached 工作树直接运行 → `audit RED: evidence`（干净工作树没有 `build/evidence/current/test-<hash>*.log`，属证据运输缺失，**不是**候选缺陷）；
2. 将 test 节点自己的测试日志（sha256 `4fe556dc9e6c4bed…`，与归档副本逐字节一致）运输进工作树后重跑 → **`audit PASS (03dc01d)`**。
   frozen/hygiene 两项全程静默（无命中）。

另核对：被测二进制 `build/wt-test-group002/dist/Server.exe` sha256 = `4a5aca0f6c0aba1c72eccefbce0405d3aee21ba3b6a274cb19b45f2eb0dd1bfd`，与 test 报告声明一致；evidence 归档副本与源文件逐字节一致（`diff` 为空）。

### 4. 独立测试结论核验（未越界、未伪造）— FACT

- 数字复算：`itest_results_part1/2/3.json` = 58/58、36/37、13/13（合计 108 中 107 通过），与 `part{1,2,3}-run.log` 的 `[PASS]/[FAIL]` 行数逐项一致，与报告声明一致；唯一 FAIL 标签即 `FINDING-PRE1_offline_msg_redelivered_and_flag_stuck`。
- harness 独立性：`wire.py`+`itest_group002.py/part2/part3` 与 development 的 `verify.py`（sha256 `08bb1107…`）结构、接口、文件均不同，非复用（抽查并发序号探针：4 线程真实发送 → 校验 code/唯一性/连续性/DB 终态，非恒真断言）。
- GROUP-001 回归 harness `group_e2e_regress.py` sha256 = `6eb6f367920a35d5575aa1aa0c9e01a7f9716d2c2d1b0891cde99fa36af69d01`，与 GROUP-001 独立测试原始 harness `build/evidence/req-ext-group-001-independent-8757ad6/group_e2e.py` **逐字节一致**（"原独立 harness 复跑"属实），日志 `RESULT 15/15 PASS`。
- 有界广播探针以 GBK 字节锚点匹配真实服务端日志 `[GROUP] 广播队列满…user= "it2slow"  drops= 1`，并记录慢连接实时 419/800、事后拉取补齐 802/802、快连接 800/800 —— 观测非构造。
- 越界检查：test 未改任何验收标准/阈值；GUI 冒烟明确声明"未尝试"；`test.ps1` honest-0 未被当作 GREEN 依据。均与 v1.7 纪律一致。

### 5. `FINDING-PRE1` 属基线既有（**不得计入候选缺陷**）— FACT，独立验证

- `getchatMsg` 不出现在 `git diff master...03dc01d -- Server/operatedb.cpp`（大小写不敏感匹配数 = 0）；分支对 `Server/msghandler.cpp`、`Server/mytcpsocket.cpp` 的 diff 仅涉及群消息分支，`GETCHATMSG` 请求处理与调用点零改动。
- 函数体独立提取（signature → 配对右括号，含尾随换行）计算 sha256：master 与 `03dc01d` 均为
  `690cbe79f52c5304279dccffbb762e6e3e8730d66d8799bb881f1f4a50dc8091`（len=1175），**逐字节一致**，且**复现了 test 报告所引用的 `690cbe79…` 前缀**（test 未伪造该哈希；此前审计者按不含尾随换行的规范化曾得 `74773eea…`，两种取法下 master 与候选均一致，不影响结论）。
- 结论：该缺陷在 master 上同源同行为，**非本候选引入**；test 将其单列并建议另开 SMALL 分支，处置正确。

### 6. 代码级核验（读源码，非运行）— FACT + 1 条 CLAIM

- **先提交后广播**：`operatedb.cpp:1579` `db.commit()` 成功后 `result.broadcastMessage=true`（:1589）；广播仅由 `msghandler.cpp:1656` 在 `code==0 && broadcastMessage && !notifyUsers.isEmpty()` 时触发。幂等重放路径 `broadcastMessage=false`（:1551），DB 失败路径不置位。**不变式成立**。
- **序号原子分配**：事务内 `SELECT … FOR UPDATE`（群行）→ `UPDATE … LAST_INSERT_ID(last_seq+1)`（:1559）→ `SELECT LAST_INSERT_ID()`（:1562），连接级语义由 `PooledSqlConnection` 保证；`uk_group_seq` 兜底。4 连接 × 50 对打证据（CC_*）与 DB 终态一致。
- **幂等**：`uk_group_client_msg (group_id, client_msg_id)` + 事务内 `FOR UPDATE` 短路（:1533-1557）；同键异内容返回原消息；跨发送者同键 `-2104`。
- **权限三断**：发送/拉取/确认三路均先查 `group_info.status='active'` 再查 `group_member.status='active'`，非成员/已退出/被移除即时失效；未认证连接由 `mytcpsocket` actor 门拒绝（`-1001`）。
- **有界广播**：`broadcastGroupMessage` 持全局锁仅做连接快照，锁外 `enqueueOutbound`（条数/字节双上限），满则丢弃该连接并 `drops` 计数；`pumpOutboundQueue` 在高水位下停写，断线 `clientOffLine` 清空队列。**不持全局锁写 socket** 成立。
- **分页边界（03dc01d 修复本体）**：`next_cursor` 在裁掉多取行**之后**由实际返回边界解码（:1632-1643），mode0=最大、mode1=最小；客户端 `groupchat.cpp:163` 以 `m_firstSeq - 1` 续翻，与 `seq<=cursor` 含边界语义自洽（test 的 `mode1` 观察属实，属契约阅读约定，非缺陷）。
- **迁移**：`migrate_ext_group_002.sql` 幂等（列存在走 `SELECT 1` 分支）、只增不改；`checkSchemaVersion` 取三版本 `qMax` 并保留 fail-fast；回滚脚本只删本需求对象。
- **CLAIM（未运行验证，标注为推断）**：FINDING-PRE1 的根因"同一 `QSqlQuery` 遍历 SELECT 后直接 `prepare()` UPDATE 未生效"是 test/develop 文档中的**疑似**表述；本次审计同样只做只读推断，未做运行级根因确认（该确认属修复分支的测试范围）。

### 7. CodeGraph 影响闭包 + `impact-check.py`

- `impact-check.py`（skill 脚本）以 `--from-diff` + 16 个变更文件运行：44 个符号解析成功，`affected tests = 0`（`test_glob` 已配置，工具未匹配到注册测试），影响文件 42 个，denied（`Server/filesys`、`pipeline`）零命中 → **PASS**（`impact-check.json` / `impact-check-denied.txt`）。
- `codegraph_explore` 调用路径：`MyTcpSocket::handlMsg`（`mytcpsocket.cpp:44`，actor 门）→ `MsgHandler::groupRequest`（`msghandler.cpp:1566`）→ `MyTcpServer::broadcastGroupMessage`（`mytcpserver.cpp:154`）→ `MyTcpSocket::enqueueOutbound`（`mytcpsocket.cpp:372`）；`handleGroupOperation`（`operatedb.cpp:1249-1251`）将四类消息操作分派到 `handleGroupMessageOperation`（:1470），权限与事务均在 DB 层。
- 影响面横跨 `Client/*`、`Server/*`，但接线改动全部为**新增 case/新增函数**；既有分支（注册/登录/聊天/文件/发布链接/GROUP-001）在 diff 中零改写，与回归证据（一对一在线+离线、发布链接、GROUP-001 15/15）互证。

## 观察（非阻塞，不进 repair；供 owner/architect 归档时择机处理）

1. **`pipeline/audit.ps1` 冻结点检查是"最后一次提交"级**（`git diff --name-only HEAD~1..HEAD`），无法发现跨提交的分支级 frozen 变更；本次分支级复核靠人工/独立脚本完成。建议后续把该检查扩为 `git merge-base master HEAD` 起的区间（工具/流程改进，非本候选缺陷）。
2. **`Client/group.{h,cpp}` 出现在 diff 中**，design §11 允许清单未逐字列出（§10 有"或并入 `Client/group.*`"的表述）。非 frozen 路径、无他人所有权，属设计文字与实现的轻微出入；建议在 task/design 记录中一句话追认。
3. **归档 manifest 的 commit 字段易误读**：`independent-test/manifest/manifest.txt` 记 `commit: b0fba52, dirty: yes`，这是**归档动作发生时主工作树 HEAD**（当时证据文件尚未提交），而非被测候选；运行级绑定在 test 工作树内的两份 manifest（`commit: 03dc01d…, dirty: no`）与全部文件名/日志中。建议 INDEX.md 补一行说明避免后人误读。
4. **`690cbe79…` 哈希此前未记录取法**：本次已复现并写明（函数体文本含尾随换行的 SHA-256）。建议后续证据脚本内联该命令。
5. **错误码存在性侧信道（既有模式，非本候选引入）**：非成员探测"存在且 active"的群得 `-2101`、"不存在/已解散"得 `-2102`，可区分群是否存在；design §9 "编码不泄露他群存在性" 的表述与实现不完全一致。GROUP-001 基线（`-1017` vs `-1026`）已有同类性质，本候选沿用同一模式。建议 architect 侧修正措辞或统一策略，非阻塞。

## 未执行项 / 边界

- 未在审计快照中重新编译或运行服务端（LARGE 审计节点输入 = 管道报告 + 差异 + 影响闭包，只读不运行）；被测二进制哈希与三门日志已按上述核验。
- 未做 GUI 冒烟（无法自动化，test 已声明未尝试，维持 NEEDS-HUMAN 遗留）。
- 未重跑独立 harness（只读复核其日志/JSON/源码与哈希一致性）。

## 结论

**PASS**：候选 `03dc01d` 满足 design §12 的验证要求；冻结路径为批准的 88–96 纯追加；三门与证据自洽、哈希绑定成立；独立测试结论（0 条归因缺陷 + 1 条基线既有 FINDING-PRE1）经复算与源码级复证成立；未发现伪造、越界或阻塞缺陷。可进入 integration（三事实行：本审计报告已存在且被接受、frozen-path diff 为空、合并结果 pipeline GREEN）。

## cleanup_status

- 审计工作树 `build/wt-audit-group002`（detached @ `03dc01d`）：**已删除**（`git worktree remove --force` + `git worktree prune`）；删除前 `git status --porcelain` = 0（仅 gitignored 构建/证据运输文件）。
- 本节点**零提交**（分支 head 仍为 `4bed127`）；主工作树与 test 工作树生产源码零改动（`git status --porcelain` 均为 0，归档证据除外）。
- 审计未启动任何服务端进程、未占用 5000 端口、未写数据库。
- 证据落盘：`project-docs/evidence/REQ-EXT-GROUP-002/audit/`（本报告、`independent-verification.log`、`branch.diff`、`audit-run-fresh.txt`、`audit-run-with-evidence.txt`、`impact-check.json`、`impact-check-denied.txt`、`manifest/`）；运输副本按协议写入 `build/evidence/codex-req-ext-group-002-group-messaging/03dc01d86a38216f06c5c307cedaef718294601b/audit-report.md`。
- 本节点 manifest 的 `commit:` 字段同样是**归档时主工作树 HEAD**（`4bed127`）、`dirty: yes`（证据文件尚未提交所致），与观察 3 同一性质；审计对象的绑定哈希为 `03dc01d…`（见报告抬头与全部复核输出）。
