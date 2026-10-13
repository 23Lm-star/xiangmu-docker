# REQ-EXT-TRANSFER-003 独立审计报告（candidate 4965738）

- 节点：independent audit（Development OS v1.7 / LARGE；新会话、只读、detached 工作树）。
- 对象：`4965738`（完整 `4965738946ff98a09e9e1e0b840d24a36e08f201`，`fix(transfer): … G7 修复 R-1/D-a/D-b (FINDING-IND-7)`）；基线 `master@d85e39f`（merge-base 同）；分支 `feat/req-ext-transfer-003-upload-session-throughput`。
- 审计快照：`E:/qt/xiangmu/build/wt-audit`（detached @4965738；起始 `git status --porcelain` 为空、HEAD 锁定、零提交）。
- 输入：test-report `b861351` + `finding-g7-task-vanishes.md` + repair 证据（`build/wt-claude/build/evidence/claude-req-ext-transfer-003-repair/`）+ 修复 diff `b861351..4965738` + 冻结面 diff `d85e39f..4965738` + 人工复验记录。**未做全仓扫描，未复跑套件**（独立复跑属 test 节点；审计是推理层）。
- **结论：PASS —— 候选 4965738 无阻塞项。** FINDING-IND-7 的 R-1/D-a/D-b 三条修复在源码层逐条成立；冻结面与 design §9 批准稿**逐字节相等**；无新增阻塞缺陷。非阻塞发现 4 条（§4）。合并前另有 3 项未闭环，属主人/owner 的决定，不属本节点裁决（§5）。

## 0. 只读边界与独立性披露（照实）

- 只读边界：未 `commit/push/merge/reset`；未改任何生产源码、配置、测试、验收条件与 Git 历史。全程写入仅限：gitignored 的 `build/wt-audit/build/`（一次性工作间：复用的几何校验脚本 + 本次生成的 `audit-repair.diff`）、gitignored 的 `build/evidence/`（本报告）、gitignored 的 `project-docs/handoffs/`（节点 handoff，`.gitignore:84`）。快照 `git status --porcelain` 起止均为空。
- 独立性披露（不掩盖）：任务/development/test/repair **均为 `claude-code`**，本节点同系。本机跨 CLI 不成立（与 TRANSFER-001 审计、001/003 test 报告同口径）。实际独立性 = **新会话 + 独立 detached 工作树 + 所有承重事实由我自己从 git 对象与源码重新导出（不采信 repair 的结论文字）**，**不等于**跨工具链/跨厂商独立。此为残余限制，不计为满足完全独立；若主人要求跨 CLI 审计，请在接受本报告前裁决。
- 工作间残留披露：审计工作树内已有 3 个 **09:54 的助脚本**（`audit_frozen_check.py` / `negctl-static.py` / `run_declared_suites.py`），无 `audit-report.md`——即本次审计之前有一次**未产出报告的审计尝试**。本节点只把它们当**计算器**用（复用 `audit_frozen_check.py`），其两个输入我已独立读过原件（design §9 代码块 + `git show 4965738:Server/protocol.h`），输出与我的手工比对一致。工作间零提交，无 git 侧残留需收口。

## 1. 冻结面边界（必查 1）— FACT

| 核验 | 方法 | 结果 |
|---|---|---|
| 冻结面内容 == design §9(a) 批准稿 | §9 栅栏 5 行 vs `Server/protocol.h:197-201@4965738` | **逐字节相等**；`sha256(批准稿) == sha256(实际) == 4695f639fdea64269e14f70b327b05f349bb1f3965a0dfb125ee932bec79c7a7` |
| 分支级冻结面 diff | `git diff d85e39f..4965738 -- Server/protocol.h Client/protocol.h` | `Server/protocol.h` **+5/−2、2 hunk**；新增集合 == 批准稿 5 行；删除集合 == 原两行常量；`Client/protocol.h` **零改动** |
| 批准稿之外无第二处改动 | 同一 diff 的全量 hunk 检查 | 成立（枚举/偏移/`caMsg`/`sizeof(PDU)` 均未动） |
| **修复提交**是否碰冻结面 | `git diff --name-only b861351..4965738 -- Server/` | **空**（repair 只动 `Client/`）→ 冻结面改动全部来自 development 提交 b861351，且等于批准块 |

- design §9 末尾自检写"预期 +4/−1"，实际 diff 为 **+5/−2**（批准稿代码块实为 5 行）。按 §9 自身"**或等价的批准稿行数**"口径与逐字节比对，**判定以逐字节相等为准**（与 test 报告 IND-6 独立同结论）。仅文档算术笔误，非阻塞。
- **量具缺口（FACT，需主人知情）**：本项本应由 `pipeline/audit.ps1` 产出，但主工作树的 `pipeline/audit.ps1` 是**未填的骨架**——`$frozen = @()` 为空、脚本自报 `frozen_paths EMPTY: boundary check NOT enforced`，且 docs-only 判定用的代码根正则 `^(src|include|tests|tools|pipeline)/` **不匹配本仓的 `Client/`、`Server/`**（⇒ 任何提交都被判为 docs-only、证据回退到上一个"代码"提交）。审计工作树的副本另有一份**硬编码兜底列表**（`Client/protocol.h, Server/protocol.h`）。结论：**边界事实由本次人工逐字节比对产出，不是门禁跑出来的**。这是既有工程债（与本候选无关），但直接决定合并三事实线第 2 条的口径，故在册。

## 2. FINDING-IND-7 三条修复逐条核验（必查 2）— FACT

根因（test 节点定案、我复读源码确认）：首次登录时 `setCurrentUser(m_LoginName)` 收到空名（名字要等 1.5 s 定时器才写入）⇒ ①快照整轮静默不落盘 ②同进程再登录时名已就位 ⇒ 清表重建把内存里的失败任务整表清掉 ⇒ 面板空、续传不可达。

| 项 | 修复事实（`@4965738`，逐行读源码） |
|---|---|
| **R-1** 登录名同步化 | `Client/msghandler.h:17` → `QString handllogin()`；`msghandler.cpp:58` 在 `ret==1` 分支内 `return loginName`（失败/异常路径返回空串）；1.5 s 定时器内**已无** `m_LoginName=` 赋值。`client.cpp:198-201`：`const QString loginName = m_mhptr->handllogin(); if (!loginName.isEmpty()) m_LoginName = loginName;` **先**落 `m_LoginName` 再 `setCurrentUser(...)`。**唯一调用点**（`git grep handllogin` 全仓仅 `client.cpp:198`）→ 无遗漏调用方。 |
| **D-a** 快照不再静默丢写 | `transfertask.cpp:64-77`：`user = m_currentUser.isEmpty() ? m_lastKnownUser : m_currentUser`；两者皆空 → `qCWarning` 后返回；`m_currentUser` 为空走兜底时也 `qCWarning`；打开失败 `qCWarning`。每条 `return` 前均有告警（S-06c 静态断言 + 我逐行复核一致）。`m_lastKnownUser` 在 `setCurrentUser` 的**非空**分支首行更新（`:176`）。 |
| **D-b** 不清表重建 + 按键合并 | `transfertask.cpp:174-194`：空名早退；同名早退（**不再清表**）；清表块**嵌在** `!m_currentUser.isEmpty()`（"确实是另一个账号"）之内，且清的是上一个账号的内存侧；`restoreSavedTasks()` 在**所有**路径末尾调用。`restoreSavedTasks` 新增身份去重 `if (containsIdentity(m_tasks, t)) continue;`（内存为准、快照补缺），身份键不含 key/id/进度/状态（`sameTaskIdentity`）。 |

- **切账号语义未被破坏**：`setCurrentUser` 内 `m_lastKnownUser = name`（新名）先于 `saveSnapshot()`（把**上一个**账号的任务落盘）——但 `saveSnapshot` 取的是 `m_currentUser`（此刻仍是旧名、且非空），**写的是旧账号文件**，顺序正确。
- **不会复活已取消任务**：`saveSnapshot` 排除 `Done/Cancelled`（`:68`），且 cancel/pause/resume/finish 等跃迁点均调 `saveSnapshot`；`restoreSavedTasks` 有 `m_restoredUser == m_currentUser` 早退护栏。
- **判据链自洽**：修复后"首登→上传→杀服务端（任务 Failed 且留在内存/面板）→重登（同名早退、内存保留）→onConnectionRestored→pump"成立；新进程首登还会从快照恢复该 Failed 任务（Failed 不入终态排除集）。
- **影响面（codegraph 闭包的人工等价物）**：`git grep @4965738` 显示改动符号的调用点**全部在 `Client/` 内**（`setCurrentUser` 唯一外部调用点 `client.cpp:201`；`restoreSavedTasks` 仅内部；`saveSnapshot` 调用点 10 处全在 `transfertask.cpp`）。快照**路径、文件名 `transfers_<user>.json`、字段集合均未变** → 主人既有数据向后兼容。
- `plans/impact-check.py` 在本仓返回 `NOT_CHECKED`（"no changed paths"，其改动路径识别不认 `Client/`、`Server/` 根，与 §1 同源的口径问题）；manifest 已声明 `code_intel.impact_status: not_applicable` 并给出理由（本仓无进程内单测、无可静态到达的测试路径，回归由 socket harness 承担）。故**影响面由上述调用点枚举手工给出**，非工具产出。

## 3. 证据与断言复核（必查 3）— FACT

| 核验 | 原始输出 | 判定 |
|---|---|---|
| 新套件 | `verify-transfer003.log` = **59/59 passed**；`static-contract.log` = **STATIC 20/20** | 与 `manifest asserts: 59` 一致（53→59，**只增不减**，manifest note 同步） |
| 新增断言非永真 | `negctl-static.log`：用 `git show b861351:` 的旧码重跑，新增 S-06a..S-06f **6/6 FAIL** | **事实级**：新断言在修复前代码上确实失败 → 不是形式断言 |
| 回归锚点随缺陷落位 | repair 提交向 `verify.py` 增加 6 条静态契约（R-1/D-a/D-b 各 2 条） | 满足"缺陷修复 + 命名回归断言"的背板（命名见 §4 N-4） |
| 13 套矩阵 | `suites-summary.log` + `suite-*.log`：**11 绿 / 2 红** | 2 红逐条归因见下 |
| 红① `group-002` | `verify.py:32 ROOT = parents[2]` ⇒ 解析到 `<wt>/project-docs`，而 `:243/:579` 取 `ROOT/'Server'/'connect.config'`（真实在仓库根）→ **在任何断言之前崩**（rc=1、1 s）。分支 diff 对该 harness **零改动**；dev@e49e8aa 同样红、输出除 commit 戳外逐字节相同 | **既有 harness 缺陷（ROOT off-by-one），非本候选所致** — FACT（我复读了 32/243/579 与归因日志） |
| 红② `unique-login` E7b | `unique_login_e2e.py:202-205`：断言 `git diff master...HEAD --name-only -- Server/protocol.h Client/protocol.h` 内**不得出现 protocol.h**；本分支**经批准的 §9 冻结面变更必然命中** → 对任何合法改协议头的分支**结构性红** | **既有门禁口径错位，非本候选缺陷，未放宽任何判据** — FACT |
| **已证伪的假设（记入）** | 我怀疑"扫尾漏跑了一条在册套件"（002 分支 manifest 有 14 套含 `transfer002`） | `git show 4965738:project-docs/project-manifest.yaml` 的在册套件恰为 **13 条**（`transfer002` 属 002 分支，且 `d45ae11` **不是** 4965738 的祖先）→ **无门禁缺口**。假设作废。 |
| 人工证据的**对象正确性**（我自做） | 对部署二进制做字节扫描：`E:/qt/xiangmu/dist/Client.exe`（sha256 前缀 `034707ea`，即主人复验所用）含修复独有的三条 UTF-8 `qCWarning` 文案（各命中 1）；修复前备份 `build/wt-claude/build/backup-b861351/Client.exe` 命中 **0** | **主人 ⑤ 的确认针对的确实是 4965738 的构建**，不是旧二进制 — FACT（负对照成立） |

诚实边界：本节点**未复跑**任何套件、未重建二进制、未在 master 上做 A/B。以上判定来自"读原始证据 + 读候选源码 + 自做字节扫描"，不来自采信 repair 的结论文字。两个 skill 量具在本仓不可用，如实记录：`impact-check.py` → `NOT_CHECKED`（不认 `Client/`/`Server/` 代码根，见 §2 末）；`claim-check.py --task REQ-EXT-TRANSFER-003` → "manifest.yaml not found in task dir"（本仓任务目录是 `design.md + task.md`，非 skill 默认布局）⇒ claim/fact 分级由本报告手工执行（每条发现均标注 CLAIM 或 FACT 并给出支撑）。

## 4. 发现（claim / fact 分级）

- **N-1（非阻塞；FACT）`group-002` 在本候选上贡献 0 条断言** → 群消息（`G2 *`）这一面**当前没有活的回归覆盖**。本候选改了 `Server/msghandler.cpp`（群路径函数虽不同，但同文件）。其余群面覆盖仍在：`groupfile 45/45`、`group004 23/23` 绿。**建议**：一行改 `parents[2]→parents[3]`（另立 SMALL 修 harness），修后重跑该套。**风险接受与否属主人决定**（历史多次合并已在同样归因下放行）。
- **N-2（非阻塞；FACT）`unique-login` E7b 对任何改协议头的分支结构性红**。合并时**不得**写成"13 套全绿"，必须写成"11 绿 + 2 条已归因红"。口径修复（把 E7b 限定为"仅 §9 批准块"或移出回归面）归 design owner，**不得靠放宽断言解决**。
- **N-3（非阻塞；CLAIM，未做动态复现）`LOGIN_RESPOND` 无条件调 `onConnectionRestored()`**（`client.cpp:203`），**登录失败（ret≠1）也走**：此时 `handllogin()` 返回空串 → `m_LoginName` 保留旧名 → `setCurrentUser` 早退，**不丢任务**；但 `m_connectionDown` 被清、`pump()` 会把 `Queued` 任务在**未认证**连接上启动一次（服务端拒绝 → 回到 Queued/Failed）。影响有界（不删任何分片/暂存、无数据损失），且该行来自 development 提交 b861351（G7），**非 repair 引入**。**判定该 claim 的实验**：登录界面下留一条 Queued 任务、故意输错验证码，抓包看是否发出 29/31。附带事实：`client.cpp:204 m_loggedIn = true` 同样在登录失败时被置真。
- **N-4（非阻塞；FACT，轻微）** 新增 6 条断言命名为 `S-06a..S-06f`（R-1/D-a/D-b），**未按缺陷号 `FINDING-IND-7` 命名**；契约偏好"以缺陷 ID 命名回归测试"。映射已写入 manifest note，不影响可追溯性。

## 5. 未闭环项（交主人 / owner，均不属本节点裁决）

1. **需求人工面 ⑤ 只完成了一半**：主人已确认"断连→重登→失败任务仍在→可重试"（`human-reverify-2026-10-09.md`，`human-verified by 主人(wcy), 2026-10-09`）；**"点重试→从断点续传至完成"与"`transfers_wcy.json` 随会话更新"尚未补记** ⇒ 合并时**不得**声称 §6.3 人工面 5/5。
2. **本报告需被主人显式接受**：`state.ps1` 只有在 handoff 写 `audit: accepted by 主人, <date>` 后才停止把它列为 pending。我方 handoff 记为 `audit: pending`。
3. **合并三事实线**：① 审计报告在册**且被接受**（未达成，待 1/2）；② 冻结面 diff **仅 §9 批准块**（**已达成**，§1）；③ 合并结果 pipeline 绿（**由 owner 在合并时产出**；注意主工作树的 pipeline 四脚本是骨架版，`test.ps1` 因 `test_gate.status: waived` 报 UNTESTED，故"绿"必须指明其来源口径，见 §1 量具缺口）。

## 6. 结论

**PASS（候选 4965738）**：R-1/D-a/D-b 三条修复在源码层逐条成立、影响面限于 `Client/`、冻结面逐字节等于批准稿、无新增阻塞缺陷、新增断言经负对照证明非永真、主人 ⑤ 的确认对象经字节扫描证实为本候选构建。

**但"可合并"仍缺 3 项**（§5），均非审计可代劳：主人接受本报告、主人补 ⑤ 完成确认、owner 在合并结果上出绿。

### 证据清单（本目录）

`audit-report.md`（本文件）；工作间脚本 `build/wt-audit/build/audit_frozen_check.py`（复用）与 `build/wt-audit/build/audit-repair.diff`（本次生成的 `b861351..4965738` 全量 diff，333 行）。
