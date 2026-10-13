# REQ-EXT-SEC-001 增量复审报告 — 候选 0efccb9（B-1 修复确认轮）

结论：**接受（B-1 关闭，允许进入 integration）**

- 复审会话：独立审计者（≠开发者），只读。未修改任何已跟踪文件；未 commit/push。
- 候选：`codex/req-ext-sec-001-auth-context` HEAD 产品代码 = `0efccb9efd20678aa2becec8eac7b9b7ed5c6515`
- 方法：claim→fact 逐条独立核验（detached worktree 源码实读 + git diff + 证据核对 + 全新重启服务端 e2e 复跑），不照抄开发者/上轮报告。

## 1. 改动面 ✅

- `git diff eb0ecc1..0efccb9 --stat`：`Server/msghandler.cpp`（10+/8-）+ `project-docs/tasks/REQ-EXT-SEC-001/task.md`（来自中间文档提交 8a552c2；产品代码仅 msghandler.cpp）。
- `git diff eb0ecc1..0efccb9 -- Client/ Server/protocol.h Server/mytcpsocket.* Server/mytcpserver.* Server/operatedb.cpp` = **空**。
- `git diff 8a0bb84..0efccb9 --name-status` = Server/ 六文件（msghandler.{h,cpp}、mytcpsocket.{h,cpp}、mytcpserver.{h,cpp}）+ docs（task.md 新增）。
- `git diff 8a0bb84..0efccb9 -- Client/ Server/protocol.h` = 0 行：两 protocol.h 与 Client/ 相对基线零改动。
- `git diff 0efccb9..HEAD(2ee8e1c) --name-only` = 仅 `project-docs/…/task.md`：主仓库产品代码与候选逐字节一致。

## 2. B-1 关闭核验 ✅（核心）

源码实读（detached worktree @0efccb9，Server/msghandler.cpp）：

1. **门控表达式**（:706）：`const bool sourceOwned = !shareCanonicalSrc.isEmpty() && pathInActorRoot(shareSrcRaw);` — 与声称一致。
2. **登记与转发同源门控**（:710-716）：`for(int i=0;i<revUserCnt && sourceOwned;i++){ addPendingShare(receiver, shareCanonicalSrc); reseed(revName, resendpdu); }` — **登记(:714)与邀请转发(:715)均在同一循环体内被 `sourceOwned` 门控**，不存在"只门控其一"的部分绕过。!sourceOwned 时仅记警告日志（:708），直接落到空响应（:718-720）。
3. **actor 语义**：`pathInActorRoot` → `actorUserRoot()`（:156-162，以 `m_actorName` 为用户根）。`m_actorName` 在 `MyTcpSocket::handlMsg` 分发 switch **之前**由服务端连接身份赋值（mytcpsocket.cpp:63 `m_MsgHandler->m_actorName=this->m_strLoginName`），且:53 未认证门先行拦截。故此处 actor = **分享人本人连接身份**，非目标接收者。攻击者 A 提交 B 的文件为源：`pathInActorRoot` 对 shareSrcRaw 做 canonical 归一（isPathInSandbox :440-458，QFileInfo::canonicalFilePath 解析 `..` 与符号链接）后判是否落在 realpath(filesPath/A)/ 前缀内 → B 的文件/指向 B 文件的 A 根内符号链接均解析到 A 根外 → sourceOwned=false，无法自我登记。**B-1 绕过链（分享他人文件→登记→自我同意）在登记源头切断。**
4. **消费侧**（sharefilemove :723-770）：`recvName=m_actorName`（:730，接收者=本人连接身份，caData 谎报名单仅作日志警告 :739-742）；放行条件 `isPathInSandbox(sourcePath)` && `pathInActorRoot(recvDir)` && 文件名无 `/` `\` && `hasPendingShare(recvName, canonicalSrc)`（:749-758）；复制成功后一次性 `consumePendingShare`（:760-763）。
5. **接收者名单可控性复核**：`addPendingShare` 的 receiver 取客户端 pdu->caMsg 名单（:712-714）——攻击者可把**自己**的文件登记到他人 receiver 名下；受害者同意时复制的源=登记时的 canonical（必在分享人自己根内），属正常分享语义，**不构成他人文件投递路径复活**：要登记他人文件，登记侧 sourceOwned 校验必失败（第 3 点）。reseed 只送达实际以该名登录的连接（mytcpserver.cpp:115-126）。
6. **canonical 同源**：登记 `QFileInfo(shareSrcRaw).canonicalFilePath()`（:703）与消费 `QFileInfo(sourcePath).canonicalFilePath()`（:747）同法、同进程（同 CWD 基准），`..`/符号链接变体归一到同一文件字符串；`hasPendingShare`/`consumePendingShare` 按 (receiver, canonical) 精确匹配（mytcpserver.cpp:146-164）。
7. **运行证据**：K2d e2e 复跑日志（见 §5）中服务端控制台出现解码确认的拒绝行：`[sharefile] 拒绝：分享源不属于分享人，不登记不转发 conn=18 源:"./filesys/sec001b/secret.txt"`，随后该源自同意 `sharefilemove` 被"源无分享握手登记"拒绝，无复制落地。

## 3. 合法链路回归 ✅

- K2b「握手登记后正常同意成功落地」PASS：本人文件→分享他人→他人同意→落在接收者本人根（`filesPath/recvName`，:743-744），sourceOwned 校验未误伤。
- K1f「接收者谎报目标名：文件仍只落在 B 本人根」PASS。
- K2c「登记一次性消费：重放同意被拒」PASS。

## 4. 流水线复算（证据核对）✅

- `manifest.txt` sha256 = `3407792bf56a4b261a1836abe35b031831d816e8464db03842e9cfbe424d4b53`，与要求一致；`commit:0efccb9…`、`dirty: no`。
- 清单内 `verify-output.log` sha256 实算 `f7a7eb83…65cd` = 清单记录 ✓；末尾 **48/48 PASS**。
- `pipeline-run.log`：`build: GREEN (0efccb9…, 20260929-214508)`、`test: 0 automated tests registered`（honest-0）、`audit PASS (0efccb9)`、`e2e: 48/47+K2d PASS`。
- `reject-lines.log`（34 行）：无密码值/哈希；唯一宽匹配行为 `logoff: user/pwd mismatch or user not found: "sec001a"`（仅用户名，无口令/哈希）。
- 注：本轮以既有 0efccb9 构建产物（Server.exe mtime 与提交同时刻）复跑 e2e，未重新编译；产品代码与候选逐字节一致已由 §1 git diff 证实。

## 5. e2e 独立复跑 ✅

全新重启服务端（build-Server/release/Server.exe，CWD=Server/，QT_LOGGING_TO_CONSOLE=1，MySQL 3306），握手登记内存态从零开始：
**48/48 PASS**，含 K2a（伪造同意被拒无落地）、K2b（正常链路）、K2c（重放拒绝）、K2d（B-1 链被拒且 stolen=False）。
日志：`reaudit/verify-output-reaudit.log`、`reaudit/server-reaudit.log`（本目录）。完毕已 taskkill Server.exe（PID 55488）。

## 6. 残余观察（不阻塞 integration）

| 级别 | 观察 | 说明 |
|---|---|---|
| Low-Med | 握手登记表可被认证用户灌满（PENDING_SHARE_MAX=1024，addPendingShare 满则 removeFirst 挤占，mytcpserver.cpp:140-142），致合法待同意登记被提前淘汰 | 需已认证；面自 eb0ecc1 引入以来未因 B-1 修复恶化（B-1 修复反而收窄了登记来源）；建议后续按 actor 配额 |
| Low | `sharefile()` revUserCnt 未与 caMsg 实际容量钳制（基线 8a0bb84 即存在，逐字对照未变）：极端值致 resendpdu 尺寸计算/`pdu->caMsg+32*i` 越界读面 | **既有面，本候选未恶化**（循环现受 sourceOwned 门控，!sourceOwned 时不再读名单）；建议 integration 后单独钳制 0≤revUserCnt≤uiMsgLen/32 |
| Info | consume 失败（copy 返回 false）时登记保留可重试，一次性语义仅在成功后生效（:761-763） | 与 M-1 设计一致，可接受 |
| Info | TTL 30min（PENDING_SHARE_TTL_MS，mytcpserver.h:50） | 合理；revUserCnt 整数溢出面为上述既有项，未恶化 |

## 7. 独立性与只读声明

本会话未参与 M-1/B-1 任何开发；全程只读产品源码与 Git 历史；仅在本证据目录写入 `reaudit-report.md` 与 `reaudit/` 日志；detached worktree（.claude/worktrees/wt-reaudit-0efccb9 @0efccb9）报告写毕后删除。
