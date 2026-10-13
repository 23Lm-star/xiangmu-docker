# REQ-EXT-TRANSFER-003 修复面独立审计报告（`node: audit`，LARGE 强制节点）

| 项 | 值 |
|---|---|
| 审计对象 | `feat/req-ext-transfer-003-upload-session-throughput` @ **`4965738`**（修复提交；父 `b861351`；基线 `master@d85e39f`） |
| 审计工作台 | `build/wt-audit`（`git worktree add --detach 4965738`，已删除） |
| 审计日期 | 2026-10-09 |
| 审计节点写入范围 | 仅本目录（`build/` gitignored）；未提交、未改产品源码/测试口径/git 历史 |
| **独立性披露** | 本会话与 development / independent-test / repair 节点**同为 `claude-code`**：**跨代理独立性未满足**。主人可要求换 CLI（Codex）重审同一 hash，报告结构与结论可原样复用（全部判定可由 §7 命令复现） |

## 结论（一句话）

**PASS-with-findings**——修复面（R-1）在源码逻辑与运行语义上成立、冻结面逐字节合规、S-06 六条新断言非永真、断言数 53→59 自洽；发现 6 条（A-1..A-6：1 条中等的**既有**缺陷、5 条低危/信息级，见 §4），**无一条与开发/测试结论构成事实冲突到需要仲裁的程度**——唯一需要主人裁决的是 §4-A-4（既有 File 页面路径陈旧，是否并入本需求）与 §3c（本 hash 两份审计报告以哪份为主体）。

---

## §1 冻结面边界（自核，不采信测试节点预检）

**判定：符合。** `Server/protocol.h` 的新增块与 design §9 批准稿**逐字节相等**；`Client/protocol.h` 零改动。

| 检查 | 命令 | 结果 | 原始输出 |
|---|---|---|---|
| 块内容逐字节 | 提取 design §9 代码块（5 行），与 `git show 4965738:Server/protocol.h` 第 197–201 行逐字符比较 | **相等**；`sha256(块)=4695f639fdea6426…c7a7`（批准稿与实现同值） | `logs/audit-frozen-bytes.log` |
| 整文件改动量 | `git diff d85e39f..4965738 -- Server/protocol.h` | **+5/−2，单 hunk**；新增行集合 == 批准稿 5 行；删除行 == 原两行常量 | 同上 |
| 客户端冻结面 | `git diff d85e39f..4965738 -- Client/protocol.h` | 输出为空 = **零改动** | 同上 |
| §9(b) 非冻结配检 | `Server/mytcpsocket.cpp:649-651` | `inboundCap = (uiType==31) ? MAX_UPLOAD_DATA_PDU_SIZE : MAX_INBOUND_PDU_SIZE`，与 §9(b) 逐字一致 | 本报告 §7 |

未越权：全分支 diff 中 `Server/protocol.h` 只出现 §9 批准块一处；枚举值、既有偏移常量、`caMsg` 语义、`sizeof(PDU)` 均未动。

**A-2（低，文档面）**：design §9(a) 的括注"（原两行删除，**新增四行**）"与 §9 自检预期"+4/−1"同其**代码块本身**（5 行）及实际 diff（+5/−2）差一行。以"逐字批准稿"为准则实现无误（**FACT**）；错的是括注与自检数字。`suggested fix:` 修 design.md §9 的括注/自检数字（纯文档，需 owner 在非审计会话改）。

---

## §2 修复面深查（`git diff b861351..4965738`）

**范围确认（FACT）**：只动 `Client/{client.cpp, msghandler.cpp, msghandler.h, transfertask.cpp, transfertask.h}` + `project-docs/evidence/REQ-EXT-TRANSFER-003/verify.py` + `project-docs/project-manifest.yaml`（7 文件 +134/−30）。无 Server 改动、无冻结面改动、无既有套件/口径改动。

### 2.1 R-1（根因，`handllogin()` → `QString` + 同步落名）

代码事实（`Client/client.cpp:194-204`）：`handllogin()` 同步返回名字（失败/异常返回空串），调用方 `if (!loginName.isEmpty()) m_LoginName = loginName;` **先于** `setCurrentUser(m_LoginName)`；1.5 s 定时器内**不再**写 `m_LoginName`（`Client/msghandler.cpp:79-103`）。

**FACT — 全仓唯一写点**：`m_LoginName` 的赋值点只有 `client.cpp:199`（`Grep 'm_LoginName\s*=' Client/` 全命中为读点，唯一赋值 1 处）；`handllogin()` 唯一调用点 `client.cpp:198`。因此"删掉定时器写名"不留下任何未被覆盖的写入路径。

**FACT — 失败分支语义未变**：`ret==-1/-2/-3/-4` 与 `else`（状态异常）分支返回 `QString()` → `!isEmpty()` 为假 → `m_LoginName` **保持原值**（与修复前"定时器只在 ret==1 才写"等价）。`QMessageBox::information` 在 -2/-3/-4 是模态嵌套事件循环，属既有重入面，修复未改变其可达性。

**FACT — 所有 `m_LoginName` 读点逐一判定（18 处，提前 1.5 s 就位是否改变行为）**

| # | 读点 | 触发时点 | 提前赋值的影响 |
|---|---|---|---|
| 1 | `client.cpp:201`（`setCurrentUser` 实参） | 登录响应内 | **正是修复目标**：首次登录不再传空串 |
| 2 | `client.cpp:220`（ADDFRIEND_REQUEST） | 服务端推送 | 窗口内自"旧名/空"变"新名"，语义更正确 |
| 3 | `client.cpp:554`（注销请求） | 用户点击（Index 已显示） | 无：该时点新旧一致 |
| 4-5 | `chat.cpp:43`、`friend.cpp:135/161` | 用户操作 | 无（均在 Index 显示后） |
| 6 | `file.cpp:71`（File 构造，`m_strUserPath`） | `Index::getinstance()` 首建 = 1.5 s 定时器内 | **更安全**：修复前同刻写名在 `Index::show()` 之前一行，等价；极端路径（重连 `forceReLogin` 先构造 Index）由"空名建错目录"变为正确名 |
| 7-8 | `file.cpp:614/633` | 用户操作 | 无 |
| 9-11 | `groupchat.cpp:205/316/436`（"我"判定） | 用户操作/回显 | 无（窗口内可能提前正确） |
| 12 | `msghandler.cpp:126`（在线列表过滤自己） | ONLINEUSER_RESPOND 推送 | 窗口内自"旧名/空"变"新名"，**更正确**（不再把新登录的自己列进在线好友） |
| 13-15 | `msghandler.cpp:312`、`onlineuser.cpp:66`、`sharefile.cpp:50`、`userinformation.cpp:13` | 用户操作 | 无 |

**FACT — 定时器剩余职责未被改变**：`resetGroupStates()` 体为 `m_groupStates.clear()`（`client.h:70`），不读 `m_LoginName`；`Index::show()`/`Client::hide()`/延迟 `flushfriend()`/`initializeSession()` 均不依赖用户名（`initializeSession` 的 actor 身份由连接侧决定）。故"提前 1.5 s 就位"**没有任何路径回退到旧值语义**。

**FACT — 与 FINDING-IND-7 根因闭合**：修复前首次登录 `setCurrentUser("")` 早退 ⇒ `m_currentUser` 恒空 ⇒ `saveSnapshot()` 空用户守卫整轮 no-op；同进程再登录 `m_LoginName="A"` ≠ `m_currentUser=""` ⇒ 放行 ⇒ `m_tasks.clear()` + 读空快照 ⇒ 面板空。修复后首次登录即 `m_currentUser="A"`，再登录 `name == m_currentUser` ⇒ 早退 ⇒ 内存任务保留。**这正是主人 2026-10-09 GUI 复验通过的那条链路。**

### 2.2 D-a（`saveSnapshot()` 兜底）

**FACT — 兜底分支不可达（防御为死代码）**：`saveSnapshot()` 取 `user = m_currentUser.isEmpty() ? m_lastKnownUser : m_currentUser`（`transfertask.cpp:67`），`m_lastKnownUser` 的**唯一**赋值点在 `setCurrentUser()` 内（`transfertask.cpp:177`），且其后必然执行 `m_currentUser = name`（`:192`，同名分支在 `:178` 早退时 `m_currentUser` 已等于 `name`）。因此恒有 **`m_lastKnownUser` 非空 ⟺ `m_currentUser` 非空** ⇒ `:72-74` 的"兜底写入"告警分支与 `:67` 的兜底取值**永不触发**；能触发的只有 `:68-71` 的"两者皆空 → 告警跳过"。

**推论（FACT，由穷举得）**：修复前 `saveSnapshot()` 在 `m_currentUser` 空时静默 return，现在改为**告警** return——行为差异仅一条 `qCWarning`（可观测性），**不产生写错用户的可能**：不存在"内存有任务而 `m_currentUser` 为空"的状态（`m_tasks` 三个入口——`restoreSavedTasks`（要求 `m_currentUser` 非空）、`enqueue*`（仅由 `File`/`Group` 页面调用，页面只在登录成功后 `Index::show()` 可见）——都要求已登录）。

**FACT — 跨进程重启语义**：`m_lastKnownUser` 为内存成员，重启即空；重启后首个 `saveSnapshot()`（若有）落在 `user.isEmpty()` 的告警分支，不写任何文件。不存在"把上一进程的落盘写到某个猜测用户"的路径。

**A-1（信息级）**：commit message 与 design 增补把 D-a/D-b 记为"防御"，其**触发条件在当前不变式下不可达**。这不是缺陷（无害且方向正确），但"防御已生效"无法被任何测试证明。`suggested fix:` 二选一——(a) 在报告/提交说明里改述为"可观测性 + 结构约束（不构成可达路径）"；(b) 若要真正兜底，需允许 `m_currentUser` 在注销/断线时清空（那是行为变更，须走新批准）。**不要**为了让兜底可达而改早退分支。

### 2.3 D-b（`setCurrentUser()` 与 `restoreSavedTasks()` 合并）

**FACT — 清场仍然干净（切账号）**：`setCurrentUser(B)` 且 `m_currentUser=="A"` 时，先 `saveSnapshot()`（键 = `m_currentUser` = "A"，写的是 A 的文件）→ 再清 `m_tasks/m_idToKey/三条队列/两个 in-flight 计数/` → `m_currentUser="B"` → `restoreSavedTasks()` 读 B 的文件。**不存在把 A 的任务写进 B 或反之的路径**（键取自赋值前的 `m_currentUser`，与 `m_LoginName` 的提前赋值无关——`client.cpp:199` 只改 `m_LoginName`，不碰 `m_currentUser`）。

**FACT — 合并分支同样不可达**：`restoreSavedTasks()` 只由 `setCurrentUser()` 调用，而到达它时**内存必为空**——(a) `m_currentUser` 原为空（首次登录）：无任务可言（见 2.2）；(b) `m_currentUser` 非空且名字不同（切账号）：上一句刚 `m_tasks.clear()`。故 `containsIdentity(m_tasks, t)` 恒为假，`:148` 的去重 `continue` 永不生效。**行为等价性**：把修复前后的 `setCurrentUser` 在所有可达状态上比对，唯一差异是"`m_currentUser` 空时不再执行一次空 `m_tasks.clear()`"和新增的 `m_lastKnownUser` 赋值——**均为空操作**。⇒ **4965738 相对 b861351 的可达行为变更 = R-1 一处**（这是本次审计最重要的归因结论）。

**FACT — 身份键的取值与唯一性**（若将来可达）：`sameTaskIdentity`（`transfertask.cpp:101-114`）= 个人上传 `(kind, localPath, displayName, dirPath)` / 群上传 `(kind, localPath, displayName, groupId)` / 个人下载 `(kind, serverPath)` / 群下载 `(kind, groupId, groupFileId)`。排除 `key/id/进度/状态` 是有意为之；对同一文件重复入队两次会产生**同身份两行**，合并会把快照里第二条丢掉。今日不可达（见上），且同身份两行在语义上确实指向同一服务端暂存键，故不列为缺陷。

**FACT — `wasRestored`/`m_restoredCount`/`m_restoredUser`/id 分配未被动过**：本次 diff 在 `restoreSavedTasks()` 内**只新增**了 `:146-148` 三行，其余（`Failed` 本地文件缺失分支、`Paused`、`wasRestored=true`、`++m_restoredCount`）是 b861351 原文；`resume()` 对上传重新 `allocId()`、对 `wasRestored` 走 97 预检查的逻辑未改。`m_restoredUser` 守卫与"按当前名比较"的语义未变（A→B→A 每次切换都会重新恢复，正确）。

### 2.4 与 REQ-EXT-SESSION-001（唯一登录）的交互

- **被踢 → 重连 → 同名重登**：`forceReLogin()`（`client.cpp:397-412`）只 `hide()` 索引页/清群读状态，**不清** `m_LoginName`/`m_currentUser`；重登同名 ⇒ `setCurrentUser` 早退 ⇒ 内存任务保留 ⇒ `onConnectionRestored()` 复位 `m_connectionDown` 并 `pump()`。**不会被踢会话"复活"出新任务**：被踢时 `onConnectionLost()` 只把 `Running/Hashing` 置 `Failed`（`transfertask.cpp:419-440`），`Queued/Paused/Done/Failed` 不动，`cancel()` 删行并立即重写快照。
- **被踢 → 换账号重登**：清洁切场（见 2.3）。**不会错并**：跨设备/跨进程无合并面，本地快照按 `<user>` 命名。
- **残余风险（既有，G8 已由主人明确不处理）**：同名重登后对非 `wasRestored` 上传点"重试"走直接 29（`needFastcheck=false`），与另一设备正在进行的同名同尺寸上传可能指向同一暂存键。test-report §3 已注明别名句柄扫描域为**本连接**。属已知延后项，**不计入本次发现**。

### 2.5 新静态断言 S-06a..f：非永真 + 断言数自洽

**FACT — 非永真（独立复现）**：用**你自己的**驱动（`logs/audit-s06-negctl.py`：从 verify.py 抠出 S-06 代码块**原文**，仅把 `read()` 重定向到 `git show <rev>:<path>`）在 `b861351` 旧码上跑：**6/6 FAIL**（逐条失败理由与 repair 节点的 `negctl-static.log` 一致）；在 `4965738` 上 **6/6 PASS**。见 `logs/audit-s06-negctl-old.log` / `-new.log`。

**FACT — 断言数 53→59 与套件实际条数一致**：AST 计数 `check()` 调用点 = 旧 50 / 新 56（+6）；运行期展开：`T3-07` 循环 5 次（+4）、`T3-08-init` 仅在 INIT 失败时触发（绿路径 −1）⇒ 旧 53 / 新 59，与 manifest `asserts` 及 repair 节点 `verify-transfer003.log` 的 **59/59** 三方一致。既有 53 条**未被削弱**：diff 对 verify.py 只做"仅增"（46 行新增，0 行删除）。

**口径提示（信息级）**：断言数只在 `test-gate.py` 里被机械核对，而本仓 `test_gate.status: waived`（在册决定）⇒ 59 这个数是"人工维护 + 运行器不核"。本次审计以 AST 计数与运行日志双向对齐，结论成立；后续任何人改套件仍须手改 manifest。

---

## §3 机器面复跑（audit 工作树内，独立）

| 项 | 结果 | 证据 |
|---|---|---|
| `pipeline/build.ps1` | **GREEN**（`hash=4965738`，双端 OK；`{Client,Server}/connect.config` 由 build.ps1 从主工作树 sha256 同步）。**全量重建 6 条 warning，全部落在修复未触及的文件/行**（`file.cpp:319`/`msghandler.cpp:220`/`client.cpp:559`/`ThreadCache.h:20`×3），修复面自身 0 warning | `logs/audit-build-4965738.log` |
| 重建同一性 | 自建 `dist/Server.exe` 509,440 B 与主 dist 差异 **5 字节**（PE 链接时间戳区）；`Client.exe` 3,924,992 B 差异 105 字节 ⇒ **同源独立重建**（与 test-report §1 对 b861351 的同款观测一致） | `logs/audit-rebuild-sha.txt` |
| verify.py 静态段（S-01..S-06） | **20/20 PASS**（独立跑，非采信） | `logs/audit-verify-static-20of20.log` |
| verify.py 动态段（T3-01..T3-12，59 条） | **未跑（NOT-RUN，非失败）**：`127.0.0.1:5000` 正被**主人的 GUI 复验会话**占用（PID 26708 监听 + PID 18584 已建立连接）。`server_ctl` 设计上**拒绝盲杀**占用者；只读审计不越权杀主人的进程 ⇒ 记 NEEDS-HUMAN/NOT-RUN，不重试循环 | 同日志尾部 `FATAL 端口已被占用 (PID 26708)` |
| 13 套回归 | **未复跑**（同上端口独占；且修复面为 Client-only，`Server/` 零 diff ⇒ 对 13 套驱动的 Server.exe 行为无影响；13 套日志的**出处**见 A-3） | `../../claude-req-ext-transfer-003-repair/suites-summary.log`（修复节点留档） |

### 3b 二进制标记复核（把"主人验的到底是哪份构建"钉死）

**FACT**：对主 `dist/Client.exe`（sha256 前缀 `034707ea…`，**即主人 GUI 复验所用**）做字节扫描，修复面新增的三条 UTF-8 `qCWarning` 文案**各命中 1 次**；对修复前备份 `build/wt-claude/build/backup-b861351/Client.exe`（`aa724f12…`，3,924,480 B）**全部命中 0**。⇒ 主人 2026-10-09 那句"重新登录后获取到了之前是失败的任务"针对的**确实是 4965738 的构建**，不是旧二进制。（此检查与并发审计会话的独立扫描结论一致。）

### 3c 并发审计碰撞（如实披露）

本 hash 上**同时存在两份独立审计报告**——本目录（短 hash 目录 `4965738/`）与 `build/evidence/claude-req-ext-transfer-003-audit/4965738946ff98a09e9e1e0b840d24a36e08f201/audit-report.md`（另一会话，10:01 落盘，早于本报告）。两会话共用同一个 `build/wt-audit` 工作间（对方报告 §0 提到的"09:54 三个助脚本、无报告"即本次会话的在途工作），**本节点收尾时按协议删除了该工作间**，对方报告 §"证据清单"引用的 `build/wt-audit/build/audit-repair.diff` 随之消失——已在 `logs/concurrent-audit-repair.diff.regenerated` 留一份等价重生成的 `b861351..4965738` 全量 diff（333 行）。**待主人处置：两份报告并存，建议以其中一份为主体、另一份作为交叉复核存档。**

交叉复核两条（都不改变任一方结论）：① 对方报告 §1 记 `Server/protocol.h` 为"2 hunk"，**实测 `-U3`/`-U0` 均为 1 个 hunk**（误数来自 `@@ … @@` 头里 `@@` 出现两次）；② 对方报告结论（冻结面逐字节相等、R-1/D-a/D-b 成立、无阻塞）与本报告**一致**——两份由不同会话独立导出，互为佐证。

**基线既有红（不得当新发现，口径未动）**：`group-002` = harness ROOT off-by-one（1 s 内、任何断言之前）；`unique-login` = 单红 E7b（本分支改了 `Server/protocol.h` ⇒ "分支 diff 对两 protocol.h 零命中"结构性红）。两条在修复提交中**均未被改动**（diff 不含任何既有套件文件）。

---

## §4 发现清单（claim / fact 分列）

| # | 级别 | 标签 | 内容 | 定案检查 |
|---|---|---|---|---|
| **A-1** | 信息 | **FACT** | D-a 的 `m_lastKnownUser` 兜底与 D-b 的 `containsIdentity` 合并在当前不变式下**不可达**（§2.2/§2.3 的穷举）；4965738 的可达行为变更 = R-1 一处 | 已由赋值点穷举 + 调用图定案（`grep 'm_currentUser ='`、`enqueue*` 调用点） |
| **A-2** | 低 | **FACT** | design §9(a) 括注"新增四行"与其自检"+4/−1"与实际 5 行/+5−2 差一；实现与**代码块**逐字一致，无越权 | `logs/audit-frozen-bytes.log` |
| **A-3** | 低 | **FACT**（附 claim） | `repair` 证据目录里的 13 套日志**产自 HEAD 仍为 `b861351` 的工作树**：`suite-group004.log` 自戳 `commit=b861351` + 时刻 `2026-10-09 09:36:08`，而修复提交时刻为 `09:39:34`。deploy-note §4 已如实说明（"构建时未提交，故戳 b861351"），且修复面 Client-only ⇒ Server 行为等价、矩阵结论不受影响。**claim**：该目录的矩阵是否跑的正是修复后源码——settle 方法：看 `suite-transfer003.log` 同批含 S-06a..f PASS（**是**），或按 §3 形态在空端口复跑一遍 | 上方两条日志事实 + `git log --format=%ci 4965738` |
| **A-4** | 中（**既有，非本次引入**） | FACT + claim | 进程内切账号后**文件页仍指向上一个账号的目录**：`File::m_strUserPath` 只在构造期赋值一次（`file.cpp:71`，值取当时的 `m_LoginName`），此后无任何刷新点；`flushfile()`（`:185-192`）与 `selectedFileRelativePath()`（`:614`）用的是缓存的 `m_strCurPath`，而 `selectedFileRelativePath` 的前缀剥离用**当前** `m_LoginName` ⇒ 换账号后前缀不匹配、路径不再被裁剪。**claim**：GUI 上表现为"注销→换账号登录→打开文件页，列表/上传/下载落到上一个账号的目录或直接被服务端拒绝"。b861351 与 4965738 两版 `file.cpp` 完全一致（repair diff 不含该文件）⇒ **不是本次修复引入**。`suggested fix:` 在 `flushfile()`/`File::showEvent` 里用 `Client::m_LoginName` 重算 `m_strUserPath`/`m_strCurPath`（或让 `setCurrentUser` 发一个"用户已切换"信号给 File）；**须先由主人裁决是否并入本需求** | NEEDS-HUMAN（GUI 一步：注销 wcy → 用另一账号登录 → 打开"文件"页，看路径框与列表） |
| **A-5** | 信息 | **FACT** | 修复节点的 13 套"手动执行"日志自带两条既有红（group-002 / unique-login E7b），与基线一致，**未放宽**；manifest `asserts 53→59` 为增量、与套件实际一致（§2.5） | `suites-summary.log`、AST 计数 |
| **A-6** | 信息 | **FACT** | deploy-note §4 记 `build-b861351-20261009-092750.log`"0 warning"，而审计工作树**全量**重建同一 hash 报 6 条 warning（均为既有、且不在修复面）。两者不矛盾的解释是该轮为增量构建（只重编改动的客户端源），措辞未注明——不影响任何结论 | `logs/audit-build-4965738.log`（另见 §3） |

**红旗自查（全部为"未发现"）**：无永真断言（S-06 6/6 旧码 FAIL）；断言数一致；无口径放宽（既有套件零改动、只增 6 条）；冻结面未越权（§1）；证据与 hash 不匹配面：主 `dist/{Server,Client}.exe` 的 sha256 与 deploy-note 记录一致（`e0ec1c97…`/`034707ea…`），且自建重建仅 PE 时间戳区差异；**未发现**"写错用户""复活已删任务"的新路径（§2.2/2.3/2.4）。

---

## §5 NEEDS-HUMAN（最多一次尝试，已声明不重试）

1. **A-4 切账号文件页**（见上表）——一步 GUI 即可定案。
2. **人工面第 3 项补记**（修复节点 `human-reverify-2026-10-09.md` 自记 ⏳）："点重试从断点**续传至完成**"与"`transfers_wcy.json` 随会话更新"两句原话确认；这两条不影响本审计结论（判定点是"任务是否还在、可重试"）。
3. **动态复跑（可选）**：端口 5000 空闲时，在任意 4965738 工作台按 §7 命令跑 `verify.py` + `run_declared_suites.py` 即可把 §3 的两条 NOT-RUN 补成实跑。

---

## §6 集成事实线预检（三事实行，供 integration 节点）

| # | 事实行 | 本轮判定 |
|---|---|---|
| ① | 本审计报告存在且可被主人接受 | ✅ 本文件（`build/evidence/claude-req-ext-transfer-003-audit/4965738/audit-report.md`），状态 **pending**（等 owner 处置 + 主人接受；`audit:` 字段暂填 `none`） |
| ② | 冻结面 diff 仅 §9 批准块 | ✅ §1（逐字节 + 单 hunk；`Client/protocol.h` 零改动） |
| ③ | 合并结果 pipeline 绿 | ⏳ **待 integration 在主干复核**；本次可提供的前置：`build.ps1` 在 4965738 上 **GREEN**（独立重建）、静态契约 20/20、transfer003 修复节点 59/59；两条既有红按在册口径不阻合并 |

---

## §7 复现命令（审计所用，全部只读）

```bash
# 工作台
git worktree add build/wt-audit --detach 4965738
cp -r build/wt-claude/pipeline build/wt-audit/pipeline          # pipeline/ 不入库
cp build/wt-claude/build/run_declared_suites.py build/wt-audit/build/

# §1 冻结面逐字节
py -3.13 build/wt-audit/build/audit_frozen_check.py             # => BYTE-EXACT (block a): True; Client/protocol.h 零改动
git diff d85e39f..4965738 -- Server/protocol.h Client/protocol.h

# §2.5 非永真（旧码 6/6 FAIL / 新码 6/6 PASS）
py -3.13 build/wt-audit/build/audit_s06_negctl.py b861351
py -3.13 build/wt-audit/build/audit_s06_negctl.py 4965738

# §3 机器面
cd build/wt-audit && powershell -ExecutionPolicy Bypass -File pipeline/build.ps1     # => build GREEN (4965738)
py -3.13 project-docs/evidence/REQ-EXT-TRANSFER-003/verify.py                        # 静态 20/20；动态需端口 5000 空闲
py -3.13 build/run_declared_suites.py                                                # 13 套（需独占 5000）

# §2 关键事实
grep -rn "m_LoginName\s*=" build/wt-audit/Client/                # 唯一赋值点 client.cpp:199
grep -rn "m_currentUser =" build/wt-audit/Client/transfertask.cpp# 唯一赋值点 :192
grep -rn "enqueuePersonalUpload\|enqueueGroupUpload\|enqueuePersonalDownload\|enqueueGroupDownload" build/wt-audit/Client/*.cpp
grep -n "m_strUserPath\s*=" build/wt-audit/Client/*.cpp          # 唯一赋值点 file.cpp:71
```

## §8 归档原始日志（本目录 `logs/`）

`audit-frozen-bytes.log`（§1 逐字节）· `audit-s06-negctl-old.log` / `-new.log`（§2.5 非永真）· `audit-verify-static-20of20.log`（§3 独立静态复跑 + 动态 NOT-RUN 原因）· `audit-build-4965738.log`（§3 构建）· `audit-rebuild-sha.txt`（§3 重建同一性）· `concurrent-audit-repair.diff.regenerated`（§3c 并发会话工作间被回收后重生成的等价 diff，333 行）· `audit-s06-negctl.py` / `audit_frozen_check.py`（本次审计用的两个只读工具，可重跑）
