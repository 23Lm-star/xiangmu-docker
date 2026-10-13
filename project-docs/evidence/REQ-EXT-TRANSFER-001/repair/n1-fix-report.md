# REQ-EXT-TRANSFER-001 repair（N-1 处置）报告

- 节点：repair（owner 会话；候选 `be31161`，基线 `84bfb26`）。分支 `feat/req-ext-transfer-001-parallel-transfer`。
- 授权：主人 2026-10-01 指示——读取最新审计结果（`84bfb26` PASS，含非阻塞 N-1/N-2），对仍存在的缺陷进行修正与测试。
- 输入：`project-docs/evidence/REQ-EXT-TRANSFER-001/audit-report-84bfb26.md`（本会话从 `build/evidence/` 归档）+ 交接 `req-ext-transfer-001-audit-followup-2026-10-01.txt`。
- 独立性与限制（照实）：`owner_agent: claude-code`；本机 Codex CLI 不可用，跨 CLI 独立性不成立。本会话为**同一代理的修复会话，不构成独立验证**，须由新会话 independent audit 复审。
- 约束遵守：**未触碰冻结面**（`git diff 84bfb26..be31161 -- Client/protocol.h Server/protocol.h` = 空；累计 `master..be31161` 仍 22 insertions / 0 deletions，合 §9 批准稿）；**未改 D6 与上限**（上传 3 / 下载 3）。

## 1. 逐项处置

| # | 项（审计原文） | 处置 | 证据 |
|---|---|---|---|
| 1 | **N-1**（非阻塞，审计建议修复）：97 纯占位槽 `reapIdleUploads` 要求 `lastProgressMs > 0`，而 97 建槽从不设置该字段 → 残留 `id=0` 暂存槽与 `upload_session` 行**不被空闲回收**，只能等 3600s TTL 或下一次 97 覆盖。 | **已修（code）**：`uploadfilefastcheck` 建槽即写 `ctx.lastProgressMs = now`，纳入既有 D5 回收口径。**连带**：`abortUpload` 释放时按 `sessionId` 精确匹配清 `m_pendingFastcheck.active`——否则占位被回收后，60s 后迟到的 29 会采纳一条已 `expired` 的会话行。 | `Server/msghandler.cpp`（97 建槽段 = `ctx.lastProgressMs = ...`；`abortUpload` = `m_pendingFastcheck` 失效分支）；回归锚点 **T-23** |
| 2 | **N-2**（非阻塞，测试精度）：T-20 的判别力**全部由 T-20b 承担**（T-20a 只断言 108 `ret=true`、T-20c 只断言 30 回带 == 物理大小，二者在未修复代码下同样 PASS）；T-22 为**静态文本断言**，不构成运行时行为证据。 | **不改**（如实保留审计结论）：T-20a/c 的断言在修复前后同为真，属**设计使然**（T-20c 恰是 AUD-4 缺陷的"症状"——`beginStaged` 按物理大小派生续传起点，故回带必然等于物理大小）；强行改造会引入不可判别的断言。T-22 的静态属性已在 `design.md` §6.2 与套件 NOTE 明写。**不作能力提升，不改验收条件。** | `design.md` §6.2 T-22 标注；`parallel_e2e.py` 文件末尾 NOTE |
| 3 | **F-3 动态面**（客户端第 4 个 29 确实未发出）+ **design §6.3 十条 GUI 人工项** | **仍未闭环，NEEDS-HUMAN**——无本仓自动化宿主（`test.ps1` 注册 0 单测；Client 为 GUI 进程）。步骤见 `repair/followup-report.md` §4（同上一轮）。**不计 pass。** | followup-report §4 |

## 2. 验证（本会话实跑，非采信）

| 门 | 结果 | 日志 |
|---|---|---|
| `pipeline/build.ps1` | **GREEN** `hash=be31161`（Client/Server 均 OK） | `build/evidence/current/` |
| `pipeline/test.ps1` | **GREEN-WARN**：`0 automated tests registered`（仓库口径如实，不构成 GREEN 主张） | 同上 |
| `pipeline/audit.ps1` | **RED = 预期冻结面命中**（`Client/protocol.h`+`Server/protocol.h`，本需求即冻结面变更且已批准）；`impact-check: NOT_APPLICABLE` | 同上 |

e2e（串行独占 5000，`py -3.13`）：

| 套件 | 计数 | 说明 |
|---|---|---|
| parallel_e2e | 5 次运行中 **3 次 54/54**（原 53 + **T-23**），另 **2 次服务端静默终止** | 见 §3；`repair/n1/parallel_e2e.log` 保存的是**第 5 次（崩溃）**运行的完整输出（含 traceback） |
| fastupload | **46/46** | 97/98 与 legacy 97→31 直传契约未破：`repair/n1/fastupload.log` |

**新增锚点 T-23 的自证力**：断言 = 97 未命中建槽后 `upload_session.status` 先 `uploading`、静默 >60s 后变 `expired`。判别力真实——**未修复**代码下 `lastProgressMs` 恒 0，除占位外的活跃槽被回收而**该行恒停留 `uploading`**，T-23 必失败。复用 T-13 的 82s 静默窗口，**零额外套件耗时**。
> 本会话未做反向探针（正向断言已足以区分修与未修：`uploading` → `expired` 的差异即判别力本身），故无 worktree/回滚事项。

## 3. 边界与观察（照实，不读作 pass）

- **阻塞级未决观察（⚠ 本候选需在复审中优先裁定）**：`parallel_e2e` 本会话 5 次运行中 **2 次**在服务端出现**静默进程终止**（`ConnectionResetError` + `Server.exe` 不在进程表），**崩溃点不同**：第 1 次在 T-18 首处 `up_init`、第 5 次在 **T-13 的 `time.sleep(82)` 之后**（`parallel_e2e.py:577`）。3 次全 54/54 通过。日志：`repair/n1/parallel_e2e.log`（第 5 次，含 traceback；运行在 T-14 后中断）。
  - **主要假设（未证实）**：候选把 `abortUpload(LEGACY)` 变成**可被空闲回收触发的真实路径**——此前 id=0 占位 `lastProgressMs` 恒 0，从不满足回收条件，故 `abortUpload(0)` 事实上从不执行；本修复后它在 T-13 的 82s 静默窗口内首次被 15s 定时器触发。第 5 次崩溃**恰落在该窗口之后**，与此假设一致。第 1 次崩溃在 T-18（T-13 尚未运行）**与该假设不一致**——故**不能**断言为单一原因。
  - **取证缺口**：`Server.exe` 以 `-Wl,-subsystem,windows` 构建，无控制台；`stdout/stderr` 重定向到文件得到 0 字节，**崩溃现场不可采**。
  - **状态：OPEN**。已撤销此前"一次性、不归因本候选"的措辞——该结论在观察到第 2 次崩溃后**不成立**。**建议复审节点做两件事**：①把 `Server/msghandler.cpp` 回退到 `84bfb26` 重建，在同一套件上重复 N 次以判定崩溃是否候选引入；②若确认引入，回退 N-1 并改以"不清 `m_pendingFastcheck`、仅置 `lastProgressMs`"或"为 97 占位改用独立短 TTL"等更保守形态重做。**在此之前不得读作 pass，不得进入 integration。**
- 本会话**未运行** entry_ops/dedup/cross_tree/sec001/groupfile 五套件（改动面为 97 建槽与 `abortUpload` 释放，fastupload + parallel_e2e 已覆盖这两处契约；五套件未受影响的判断为**推演**，非实测）。未做 GUI；未跑跨 CLI。
- **AUD-2-N1**（回队未区分"上限拒绝"与"重复 id/非法"）仍为历史观察，本轮未改。

## 4. 交下一节点

**independent audit**（新会话、只读、detached worktree @ **`be31161`**）：复核 (a) N-1 修复是否真实且不引入新竞态（97 建槽即计时的副作用：占位提前回收是否影响 97→31 legacy 直传、是否与 `m_pendingFastcheck` 采纳路径一致）；(b) T-23 的判别力（可否独立重做反向——关闭 `lastProgressMs` 赋值应使 T-23 必失败）；(c) N-2 的"不改"判定是否成立（T-20a/c 在未修复代码下确实同真）；(d) 冻结面零改动、D6 未改。**(e) ⚠ 最优先：§3 的服务端静默终止——先按"回退 `msghandler.cpp` 到 `84bfb26` 重建 + 重复运行"判定是否由本候选引入；若确认，本候选须回退重做。** **F-3 动态面与 §6.3 十条人工项仍未闭环**，integration 三事实行暂不成立。
