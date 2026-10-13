# REQ-EXT-TRANSFER-001 N-1 崩溃观察仲裁报告（A/B 对照 + 时间压缩探针）

- 节点：arbitration（owner 会话，梦梦/WorkBuddy 代理）。分支 `feat/req-ext-transfer-001-parallel-transfer`，head `333bef2`。
- 授权：主人 2026-10-01 指示完成本需求分支的修复收口，并首先验证主人的假设——"修复会话期间服务端静默终止 2/5 次，应为主人并行手动运行导致端口占用所致"。
- 仲裁对象：`n1-fix-report.md` §3 的阻塞级未决观察——候选 `be31161`（N-1：97 占位建槽即起空闲回收计时）是否引入 `Server.exe` 静默终止。
- 结论先行：**候选 `be31161` 未被牵连，N-1 修复维持有效；静默终止 = 服务端被"干净退出"而非崩溃。** 机制（两独立会话证据合流）：`Server/main.cpp` 使用 **QApplication** 且 `Server` 本身是唯一可见 **QWidget**——Qt `quitOnLastWindowClosed` 默认开启，该窗口一旦被关闭（主人并行操作时的手动点 X / 软 `taskkill` 的 WM_CLOSE）即触发 `QEventLoop::exit` → main 正常返回，表现为"无 WER 记录的静默消失"。并行 repair-attribution 会话（turn-cap 关闭，交接 `handoffs/req-ext-transfer-001-repair-attribution-2026-10-01.txt`）在加密回收 + gdb 下命中 `break exit` 于 `QEventLoop::exit(int)`（Qt5Core.dll），与本会话"零 WER 记录"互证。N-1 的嫌疑路径本身经探针实测无害（见 §1）。依据如下 6 条独立证据。

## 1. 证据链

| # | 证据 | 来源 | 指向 |
|---|---|---|---|
| 1 | **Windows 事件日志无崩溃记录**：覆盖修复会话崩溃窗口（2026-10-01 20:00–20:40）的 Application 日志中，Id=1000/1001 且 Message 含 `Server.exe` 的事件为 **0 条**。真实的访问违例（0xC0000005）/未处理异常几乎必然留下 WER 记录。 | `Get-WinEvent`（本会话实测，22:30 前后） | 非代码崩溃；进程被外部终止或干净退出 |
| 2 | **run 1 崩溃点（T-18 首处 up_init）在 N-1 路径的可达域之外**：全量套件含 82s 睡眠也仅 ~102s（本轮实测 102–104s），T-18 位于套件前段，其前**不存在任何空闲 ≥60s 的上传槽**——`reapIdleUploads` 在 T-18 前不可能触发过 `abortUpload`。 | `run_ab.sh` 实测时长 + `parallel_e2e.py` 用例顺序 | N-1 解释不了 run 1 |
| 3 | **静态分析：N-1 的两个 hunk 均为平凡写**（`ctx.lastProgressMs = now`；`m_pendingFastcheck.active = false`）。新解锁路径 `reap → abortUpload(id=0 占位)` 中，`ResumableUploadSession` 为纯值语义（无指针成员；未开始态 `m_filePath` 为空，`abortUpload` 有 `!staging.isEmpty()` 守卫；`reset()` 平凡）。写盘回调 `handleUploadWriteFinished` 对"上下文已被回收"有 `cp==nullptr` 防护，`writeInFlight` 使 reap 跳过在途槽。 | `msghandler.cpp:1270-1341`、`mytcpsocket.cpp:361-415`、`resumableuploadsession.cpp:9-165` | 新路径在构造上内存安全 |
| 4 | **受控环境 A/B 全绿**（一次性 detached worktree，各自独立构建、filesys 自主进程预热、PID 轮询取证）：base `84bfb26`（无 N-1）53/53 PASS、cand `333bef2`（含 N-1）54/54 PASS，服务端全程存活（PID 20020 稳定）。此前 master 回归审计 @53b048b（含 N-1）六套件 236/236 亦绿。 | `repair/n1-ab/summary-old.txt` | 受控条件下不可复现 |
| 5 | **时间压缩定向探针 10/10 通过**：诊断 worktree 内将 `UPLOAD_IDLE_REAP_SEC` 60→3（`mytcpsocket.h`，非冻结面；一次性 worktree，验证后已整体删除），探针每轮复现精确嫌疑条件——3 路空闲 INIT 槽 + 97 未命中建 id=0 占位（已建暂存会话）+ 静默窗口 > 回收阈值 → 15s 定时器回收含 `abortUpload(id=0)` 全路径。**10 轮全部存活**（等价于把该路径的触发频率压缩 20 倍后仍无崩溃）。 | `repair/n1-ab/probe.py`（探针全文已归档） | 嫌疑路径实测无害 |
| 6 | **主人自述并行操作**：主人确认修复会话时段本人在手动运行服务端/客户端（做 §6.3 人工项——其中第 9 项"传输中断开服务端"本身就需要**杀掉服务端**；且任何手动 `Server.exe` 与 e2e 实例按映像名 `taskkill /F /IM` 互相误杀、抢 5000 端口）。两次崩溃点飘忽（T-18 / T-13 窗口内）与环境干扰的特征一致，与确定性代码缺陷的特征（同路径必崩）不符。 | 主人陈述 | 环境干扰为最简解释 |

## 2. 裁定

1. **N-1 修复（`be31161`）维持**，撤销 n1-fix-report §3 的"不得读作 pass"限制（该限制的前提——崩溃可能由候选引入——已被否定：并行会话 gdb 实证事件循环被主动退出而非崩溃；本会话探针实证嫌疑路径无害）。
2. 静默终止机制定性：**QApplication `quitOnLastWindowClosed` × 唯一可见窗口（Server 主窗体）被并行人工操作关闭 → 干净退出**（手动点 X 或软 taskkill 的 WM_CLOSE）。非候选缺陷，不计入；"环境干扰"与"代码可达退出路径"两个要件齐备才会发生。
3. `n1-fix-report.md` §4 交下一节点的复核项 (a)(b)(c)(d)：(a)(b) 由本仲裁探针覆盖（路径安全 + 回归锚点 T-23 在 r1-cand 全量套件中通过）；(c)(d)（N-2 不改判定、冻结面零改动、D6 未改）沿袭 84bfb26 审计与 master 回归审计结论，未被推翻。

## 2.1 遗留建议（合并后另开需求，不阻塞本收口）

- **根因级加固**：`Server/main.cpp` 加 `a.setQuitOnLastWindowClosed(false);`（服务是常驻进程，生命周期不应绑定 GUI 窗口状态）；可选配套 `closeEvent` 忽略。本会话按主人指示收口，未实施。

## 3. 实验过程记录（照实）

- 一次性 worktree：`build/wt-n1-ab-base`（@84bfb26）、`build/wt-n1-ab-cand`（@333bef2），各自 `pipeline/build.ps1 -Target Server` GREEN，dist DLL/filesys 自主工作区预热，**验证后已整体删除**（`git worktree prune` + rmdir，零提交、零残留）。
- 诊断性改动仅一处：cand worktree 内 `UPLOAD_IDLE_REAP_SEC = 60 → 3`（`Server/mytcpsocket.h`，**非冻结面**；该 worktree 已删除，分支代码零改动）。
- 全量 A/B 只完成各 1 轮（后续轮次被主人的 safe-delete 批量删除确认钩子拦截——套件 `shutil.rmtree(.staging)` 面对累积 >50 文件的暂存目录被强制终止；该钩子与"连续复跑套件"结构性冲突，见 §4 建议 3）。
- PID 轮询器（2s 间隔）记录每次运行服务端存活与 PID 唯一性；受控期间无 PID 异常/第二实例。

## 4. 建议（不阻塞，供主人裁定）

1. **e2e 运行期间禁止并行人工操作服务端**（含 GUI 人工项、手动启停 `Server.exe`）——§6.3 人工清单应安排在 e2e 会话之外单独执行，避免再次出现"环境干扰伪装成崩溃"。
2. `parallel_e2e.py` 等套件的 `start_server()` 可加固：Popen 记录 PID、启动后轮询进程存活、死亡即 FATAL——本次未改（套件为冻结证据工件，改动需主人批准）。
3. safe-delete 钩子阈值（>50 文件需确认）与套件复跑的 `.staging` 清理冲突；建议对 `dist/filesys/.staging`（纯 e2e 暂存产物）设白名单，或复跑前人工清理。

## 5. 交下一节点

- **integration 三事实行**仍差两项 NEEDS-HUMAN：①F-3 动态面（客户端第 4 个 29 未发出——无自动化宿主）；②`design.md` §6.3 十条 GUI 人工项（步骤见 `repair/followup-report.md` §4，一轮完成、不重试）。由主人在 GUI 内人工执行并回填 `human-verified by <owner>, <date>` 后，本需求即可进 integration。
- 独立性限制（照实）：本仲裁为 owner 会话（梦梦/WorkBuddy），非跨 CLI 独立验证；但仲裁结论不依赖"候选代码正确"的自我声明，而依赖事件日志、时序推演、受控复现与探针四类客观证据。
