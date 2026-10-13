# 需求文档分支处理顺序（Development OS v1.7）

> 生成时间：2026-09-29
> 基线 commit：`b5d7bb3`
> 文档位置：`project-docs/requirements/`

---

## 一、总览：10 个分支，5 个阶段

| 阶段 | 顺序 | 需求 | 分支 | 等级 | 依赖 |
|---|---|---|---|---|---|
| **A 安全** | 1 | REQ-AUTH-002 | `fix/auth-security-baseline` | MEDIUM | 无 |
| **A 安全** | 2 | REQ-FS-003 | `fix/path-traversal-and-ownership` | MEDIUM | 无 |
| **B 卫生** | 3 | REQ-REPO-004 | `chore/repo-sanitize` | SMALL | 无 |
| **B 卫生** | 4 | REQ-DOC-005 | `docs/repo-chrome` | SMALL | 无（但建议在 004 之后）✅ 已完成（merge 08265d5 + closeout 69f2d66，2026-09-29） |
| **B 卫生** | 5 | REQ-REPO-006 | `chore/history-purge` | SMALL ⚠️不可逆 | 必须在 004 之后 |
| **——** | — | **🚦 推送门（第一次 push 到 GitHub）** | — | — | 阶段 A+B 全绿后 |
| **C 质量** | 6 | REQ-LOG-007 | `refactor/logging-levels` | MEDIUM | **必须在 AUTH-002 之后**（改同一批行）✅ 已完成（merge 01ccf89，2026-09-29） |
| **C 质量** | 7 | REQ-TEST-008A | `test/upload-session` | MEDIUM | 建议在 FS-003 之后 |
| **C 质量** | 8 | REQ-TEST-008B | `chore/register-tests-in-pipeline` | SMALL | **必须在 8A 之后** |
| **C 质量** | 9 | REQ-CI-009 | `chore/ci-static-check` | SMALL | 必须在 AUTH-002、LOG-007 之后（否则规则会报已修的缺陷） |
| **D 架构** | 10 | REQ-CFG-010 | `feat/config-externalization` | MEDIUM | 必须在 REPO-004 之后 |

> ⚠️ **2026-09-29 更正**：① REQ-AUTH-002 此前被误标为"已完成 (a621309 + a9d06a7)"——这两个提交实为 `fix-db-credentials-out-of-index`（取消跟踪 `Server/connect.config`）的产物；加盐与清日志在 master 上并不存在，实际状态为**待执行**（分支 `fix/auth-security-baseline` 于 2026-09-29 从 master `24967df` 切出）。
>
> ✅ **2026-09-29 当日完成**：`fix/auth-security-baseline` → master no-ff @`2fe0a48`；pipeline build GREEN / test 0 注册项 / audit PASS；A1–A5 人工验证 11 项 PASS（见 `project-docs/evidence/REQ-AUTH-002/`）。**A6 记为部分满足**：7 处 DIAG 已清零，但 `Server/mytcpsocket.cpp:390-396` 传输层报文转储仍打印 `caData`（含密码哈希）/`caMsg`，经主人决策移交 **REQ-LOG-007**（⑥ 的范围应包含此点；该文件不在 ① 的允许清单内）。
>
> ✅ **2026-09-29 当日完成（REQ-REPO-004）**：`chore/repo-sanitize` → master no-ff @`2304f23`；三门 GREEN/GREEN/PASS（分支 a7be845 与合并后 2304f23 各跑一轮）。A1–A7 全过。**合并事件**：merge 应用 rename 时删盘 `Client/connect.config`，步骤 1 备份按 §九 预案秒级恢复——同型需求（rm --cached + 同内容 template）合并后必须确认/恢复本地实文件（见 `project-docs/evidence/REQ-REPO-004/post-merge-verify.log`）。
>
> ✅ **2026-09-29 当日完成（REQ-DOC-005）**：`docs/repo-chrome` → master no-ff @`08265d5`（README+LICENSE(MIT，主人选定)+task.md+证据；三门在合并结果上实测 GREEN/honest-0/PASS）。**.gitattributes 主人批准不加**（core.autocrlf=true，行尾归一另开需求）。closeout（handoff+post-merge 补记）因并行会话抢占工作区误落 `refactor/logging-levels`@`ab99e6b`，随 ⑥ 合并 `01ccf89` 先进 master，本会话再以空 diff 合并 `69f2d66` 补全分支拓扑；handoff 校验 `OK (10 lines)`（注意：validate-handoff.py 需 `py -3.13`，默认 python 3.6.5 会 TypeError）。遗留分支处置建议与零删除/零推送记录见 `project-docs/evidence/REQ-DOC-005/`。

---

## 二、为什么是这个顺序（每条都有理由）

### 阶段 A（安全）先做 —— 因为它们是"推上去就晚了"

| 需求 | 理由 |
|---|---|
| AUTH-002 | 密码无盐 + 凭据进日志。**推到 public 仓库 = 永久公开**。已在阶段 A 第一位 |
| FS-003 | 上传路径穿越 = 可写任意路径。**PoC 级的洞**，推上去别人一眼能看到 |

> ⚠️ **AUTH-002 与 FS-003 无依赖关系，但都必须在 push 之前完成。**

### 阶段 B（仓库卫生）—— 为 push 做准备

| 需求 | 理由 |
|---|---|
| REPO-004 | 取消跟踪 `Client/connect.config`。**push 前必须清**，否则本地配置进库 |
| DOC-005 | README + LICENSE。**push 前的"门面"**，也是 004 的补充说明 |
| REPO-006 | 历史清理。**必须在 push 之前**——因为**推送后再重写历史要 force-push**，而 push 前重写只需改本地 |

> ⚠️ **REPO-006 是唯一不可逆的操作，且必须在 push 之前。**
> ⚠️ **REPO-004 必须先于 REPO-006**：先让"以后不再跟踪"生效，再做"历史里也清掉"，否则重写完又会被重新加进去。

### 🚦 推送门 —— 阶段 A+B 完成后

**这是第一次 push 到 GitHub 的时机。** 理由：

- 所有 P0（凭据、路径穿越）已修
- 历史已清（不需要日后 force-push）
- 仓库门面已就绪
- 质量阶段（C/D）的改动**推不推都不影响安全**，可以慢慢做

> 💡 **这是本序列中最关键的一个决策点。** 把"安全 + 卫生"集中在 push 前，"质量 + 架构"放在 push 后，可以：
> - 避免 push 后重写历史（force-push 很痛）
> - 让 push 的版本是"安全的"
> - 后续改动走正常 PR 流程即可

### 阶段 C（质量）—— push 之后做

| 需求 | 为什么在 push 之后 | 依赖约束 |
|---|---|---|
| LOG-007 | 日志分级是**改善**不是**修复**。且它改 213 处，**必然与 AUTH-002 冲突** | ⚠️ 必须 AUTH-002 先完成 |
| TEST-008A | 加测试是增量能力 | 建议 FS-003 先完成（测的就是被它改的调用方） |
| TEST-008B | 把测试接进流水线 | ⚠️ **必须 8A 先完成**（没测试可登记） |
| CI-009 | 静态规则会**报出**已有缺陷。若 AUTH-002/LOG-007 未做，规则一跑全是红的 | ⚠️ 必须 AUTH-002、LOG-007 先完成 |

> ⚠️ **LOG-007 与 AUTH-002 强依赖**：AUTH-002 删了 7 处凭据日志，LOG-007 要给剩余日志分级。**两者都改 `operatedb.cpp` / `msghandler.cpp` 的同一批行**。顺序反了会冲突。

> ⚠️ **CI-009 的规则 1（日志凭据）只有在 AUTH-002 完成后才有意义**——否则规则会一直报出那 7 处。

### 阶段 D（架构）—— 最后

| 需求 | 理由 |
|---|---|
| CFG-010 | 配置外部化**改变启动路径**，影响面最广。且它碰两个 `connect.config`，**建议在 REPO-004 稳定后再做** |

---

## 三、执行序列图（可直接照抄）

```
① fix/auth-security-baseline              MEDIUM ✅ 已完成（merge 2fe0a48，2026-09-29）
       ↓
② fix/path-traversal-and-ownership        MEDIUM
       ↓
③ chore/repo-sanitize                     SMALL
       ↓
④ docs/repo-chrome                        SMALL ✅ 已完成（merge 08265d5，closeout 69f2d66，2026-09-29）
       ↓
⑤ chore/history-purge                     SMALL ⚠️ 需主人批准确认 Q1/Q2/Q3
       ↓
   🚦🚦🚦  推送到 GitHub（需主人批准）  🚦🚦🚦
       ↓
⑥ refactor/logging-levels                 MEDIUM ✅ 已完成（merge 01ccf89，2026-09-29；必须先于 ⑨）
       ↓
⑦ test/upload-session                     MEDIUM
       ↓
⑧ chore/register-tests-in-pipeline        SMALL    (必须紧接 ⑦)
       ↓
⑨ chore/ci-static-check                   SMALL    (必须先于...不，是必须在 ⑥ 之后)
       ↓
⑩ feat/config-externalization             MEDIUM
```

---

## 四、并行可行性

**可以并行（互不冲突）**：

| 组合 | 说明 |
|---|---|
| ③ REPO-004 与 ④ DOC-005 | 改的文件不重叠（`.gitignore`/`connect.config` vs `README`/`LICENSE`） |
| ⑦ TEST-008A 与 ⑥ LOG-007 | 8A 只加 `tests/`，LOG-007 改源码。**但都要跑 pipeline，串行更省事** |

**不能并行（有依赖或冲突）**：

| 组合 | 原因 |
|---|---|
| ① AUTH-002 ↔ ⑥ LOG-007 | 改同一批行 |
| ② FS-003 ↔ ⑦ TEST-008A | 8A 测的是 FS-003 改的调用方 |
| ⑦ 8A ↔ ⑧ 8B | 8B 登记 8A 的产物 |
| ③ REPO-004 ↔ ⑤ REPO-006 | 004 必须先让"不再跟踪"生效 |
| ① AUTH-002 ↔ ⑨ CI-009 | 规则会报 AUTH-002 未修的缺陷 |

> **建议：串行执行。** 每个分支都要跑三项 pipeline（build/test/audit），并行跑会争抢 `build/` 目录。

---

## 五、每个分支的"一句话交接"（给 Claude Code）

按顺序逐个发给 Claude Code，**一次只发一个**：

| # | 发这个文件 | 预期产出 |
|---|---|---|
| ② | `project-docs/requirements/REQ-FS-003-path-traversal-and-ownership.md` | 分支 + 4 处校验 + 越权测试证据 |
| ③ | `project-docs/requirements/REQ-REPO-004-repo-sanitize.md` | 取消跟踪 + template + gitignore |
| ④ | `project-docs/requirements/REQ-DOC-005-repo-chrome.md` | README + LICENSE |
| ⑤ | `project-docs/requirements/REQ-REPO-006-history-purge.md` | 历史重写（**需先批准**） |
| ⑥ | `project-docs/requirements/REQ-LOG-007-logging-levels.md` | QLoggingCategory 分级 |
| ⑦ | `project-docs/requirements/REQ-TEST-008A-upload-session-tests.md` | `tests/uploadsession/` |
| ⑧ | `project-docs/requirements/REQ-TEST-008B-register-tests-in-pipeline.md` | `test.ps1` 登记 |
| ⑨ | `project-docs/requirements/REQ-CI-009-ci-static-check.md` | semgrep 规则 + staticcheck.ps1 |
| ⑩ | `project-docs/requirements/REQ-CFG-010-config-externalization.md` | 磁盘读配置 |

---

## 六、需要主人决策的 3 个点

| 时机 | 决策 | 在哪份文档 |
|---|---|---|
| 做 ⑤ 之前 | **历史清理的三个问题（Q1 清什么 / Q2 是否重写 / Q3 用哪个工具）** | REQ-REPO-006 §3 |
| 做 ④ 时 | **LICENSE 类型**（默认 MIT） | REQ-DOC-005 §3.2 |
| push 之前 | **是否 push、什么时候 push** | 所有文档的禁止事项第 N 条 |

---

## 七、验证：这套顺序是否自洽（自查）

| 检查项 | 结果 |
|---|---|
| 每个需求的分支都从 master 切出？ | ✅ 全部 |
| 每个需求的依赖都在它之前？ | ✅ 已按依赖排序 |
| AUTH-002 与 LOG-007 的顺序对吗？ | ✅ LOG-007 在后 |
| 8A 与 8B 的顺序对吗？ | ✅ 8B 在后 |
| 不可逆操作是否都在 push 前？ | ✅ REPO-006 在 push 前 |
| 是否有循环依赖？ | ✅ 无 |
| 是否有两个需求改同一文件但无依赖声明？ | ⚠️ **REPO-004 与 CFG-010 都碰 config 文件** → 已标注顺序（004 先） |
| 是否有需求会互相冲突？ | ⚠️ **AUTH-002 与 LOG-007** → 已标注 |

---

## 八、如果只想做最小集合（赶时间的话）

**最低限度**（只为"能安全地推上去"）：

```
① AUTH-002  →  ✅ 已完成（merge 2fe0a48）
② FS-003    →  必须（路径穿越是 P0）
③ REPO-004  →  必须（配置进库）
⑤ REPO-006  →  必须（历史里有密码）
🚦 push
```

**这 4 个做完就可以 push。** 其余（⑥⑦⑧⑨⑩）都是 push 之后的改善，不影响安全性。

> **但 ④ DOC-005 强烈建议一起做**——README 是"别人能不能用"的门槛，成本很低。

---

## 附：文档清单

| 文件 | 行数 | 等级 |
|---|---|---|
| `REQ-AUTH-002-password-salt.md` | 316 | MEDIUM ✅ |
| `REQ-FS-003-path-traversal-and-ownership.md` | 373 | MEDIUM |
| `REQ-REPO-004-repo-sanitize.md` | 267 | SMALL ✅ |
| `REQ-DOC-005-repo-chrome.md` | 286 | SMALL |
| `REQ-REPO-006-history-purge.md` | 277 | SMALL ⚠️ |
| `REQ-LOG-007-logging-levels.md` | 327 | MEDIUM ✅ |
| `REQ-TEST-008A-upload-session-tests.md` | 606 | MEDIUM |
| `REQ-TEST-008B-register-tests-in-pipeline.md` | 331 | SMALL |
| `REQ-CI-009-ci-static-check.md` | 398 | SMALL |
| `REQ-CFG-010-config-externalization.md` | 451 | MEDIUM |
| `BRANCH-ORDER.md`（本文档） | — | — |

## 阶段 E（扩展功能，2026-09-29 起）

| 顺序 | 需求 | 分支 | 等级 | 状态 |
|---|---|---|---|---|
| E1 | REQ-EXT-SEC-001 服务端身份与授权上下文 | `codex/req-ext-sec-001-auth-context` | LARGE（前置门） | ✅ 已完成（merge `7fc47c1`，审计链 c7c4eed PASS*→B-1 REJECT→0efccb9 ACCEPT，e2e 48/48，证据 `project-docs/evidence/REQ-EXT-SEC-001/`，2026-09-29） |
| E2 | REQ-EXT-GROUP-001 群成员生命周期 | `codex/req-ext-group-001-membership` | LARGE | ✅ 已完成（merge `ee75441`，PDU 60–87，证据 `project-docs/evidence/REQ-EXT-GROUP-001/`，2026-09-30） |
| E3 | REQ-EXT-GROUP-002 可靠群文本消息 | `codex/req-ext-group-002-group-messaging` | LARGE | ✅ 已完成（merge `ffaaacf`，PDU 88–96，独立测试 PASS + 独立审计 PASS，e2e 全绿，2026-10-01） |
| E4 | REQ-EXT-FILE-001 文件资产与内容寻址 | `codex/req-ext-file-001-file-assets` | LARGE | ✅ 已完成（merge `9faae6c`，对象仓 + 五表 + 逻辑键，四套件 43/13/48/37 全绿，protocol.h 零碰，2026-09-30） |
| E5 | REQ-EXT-FILE-002 顺序上传秒传与去重 | `codex/req-ext-file-002-fast-upload` | LARGE | ✅ 已完成（merge `abe2e5d`，PDU 97/98 追加 + 秒传预检查/去重，独立测试 46/46 与独立审计 ACCEPT，2026-10-01） |
| E6 | REQ-EXT-GROUP-003 群文件目录与访问控制 | `codex/req-ext-group-003-group-files` | LARGE | ✅ 已完成（merge `495e552`，PDU 99–106 + `group_file` 表 + schema_version 2026100103，groupfile 45/45 + 六套回归全绿，独立审计 PASS，2026-10-01） |
| UX1 | REQ-UX-001 客户端可用性与视觉统一 | `fix/req-ux-001-client-usability` | MEDIUM | ✅ 已完成（merge `8cf7e3b`，2026-10-01；五套 e2e 195/195 + A2 定向探针 5/5，证据 `project-docs/evidence/REQ-UX-001/`；GUI 目视 NEEDS-HUMAN） |
| E7 | REQ-EXT-TRANSFER-001 多文件并行传输（有限并发） | `feat/req-ext-transfer-001-parallel-transfer` | LARGE | ✅ 已完成（2026-10-01 merge 本次；parallel_e2e 54/54 + 七套件全绿；N-1 仲裁收口=干净退出非崩溃，N-1 修复维持；F-3 动态面 + §6.3 GUI 十条 NEEDS-HUMAN；遗留建议 setQuitOnLastWindowClosed(false)） |
