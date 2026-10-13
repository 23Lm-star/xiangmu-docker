# Development OS v1.7 在 E:/qt/xiangmu 的执行缺陷清单

审计对象：`~/.claude/projects/E--qt-xiangmu*` 下全部 **55 个会话**（2026-09-29 03:04 ~ 2026-10-02 13:47，约 35MB jsonl）
基线：`~/.claude/skills/development-os-v1-7`（SKILL.md rev v1.7.1 + references/）
交叉验证：磁盘实物 `pipeline/` 四脚本、`project-docs/project-manifest.yaml`、73 份 handoff、git 全量提交历史
方法：全量解析 jsonl（工具调用、bash 命令、用户/助手文本、token 用量）+ 磁盘实物核对，不抽样。

---

## 总览

| 维度 | 数据 |
|---|---|
| 会话数 | 55（其中 8 个为空壳/挂载会话） |
| 需求分支数 | 6（transfer-001 一个需求独占 13 个会话） |
| handoff 文件 | 73 份，其中 tier:LARGE **47 份**（64%） |
| turn_guard 拦截 | 38 次，命中 30/55 会话，平均 **216 请求/会话**，最高 **649**（软上限 60） |
| 用户手动"发交接词" | **10 次** |
| 用户催促/抱怨耗时 | 50 次命中 |
| 用户强行跳门 | 2 次（跳审计、强行合并） |

**一句话结论**：v1.7 的**治理条文**在这半个多月里被反复违反，但**违反几乎全部由"机制缺失"而非"AI 不听话"造成**——最核心的问题是三个门（test/audit/integration fact-gate）里有两个在本项目**结构性空转**，逼着人和 AI 反复用嘴补门。

---

## P0 — 结构性缺陷（门禁空转，流程形同虚设）

### 1. `pipeline/test.ps1` 永远跑 0 个测试，三门之一是空门

**事实**：`test.ps1` 里 `$TESTS = @()`，脚本自己打印 `test GREEN-WARN: 0 automated tests registered` 后 `exit 0`（绿灯）。
全项目真实的 8 套 e2e 回归脚本（`parallel_e2e.py` 54 项、`sec001` 48 项、`fastupload` 46 项、`groupfile` 45 项、`entry_ops` 43 项、`cross_tree` 41 项、`dedup` 13 项、`group_e2e` 15 项，共 **236 断言**）全部散落在 `project-docs/evidence/*/` 下，**从未注册进 test 门**。

**后果**：
- 「pipeline 三门全绿」在本项目**不构成任何质量证据**。多个会话的收尾话术正是这么写的：
  > `Merged result pipeline is green (build GREEN, test GREEN-WARN 0 tests, audit PASS on abe2e5d)`（ccdbd599）
  > `test GREEN-WARN（仓库 0 个自动化测试，不主张绿灯）`（cbd0acc8）
- 违反 invariant 1「no forged pass」的精神：脚本诚实降级了，但**下游节点把 GREEN-WARN 当 GREEN 引用**，语义在传播中丢失。
- `audit.ps1` 的 test 对账面（`impact_status: not_applicable`）直接写死在 manifest 里，理由是「0 registered tests」，于是**影响面分析被永久关闭**，30 次 codegraph/impact-check 调用里绝大多数是探测 `.codegraph` 是否存在，没一次产出真实的依赖闭包结论。

**根因**：v1.7 假设"项目会自己长出自动化测试"，本项目是 GUI + 手写 socket harness 的 C++，测试不会自动进 `$TESTS`；manifest 用 `not_applicable` 把影响面关掉，属于**用配置绕过门禁**。
**该做**：把 8 套 e2e 注册进 `test.ps1`（`$TESTS` 填 py 脚本 + 退出码），把 `impact_status` 从 `not_applicable` 改成真实运行。

### 2. 独立审计的"独立性"被结构性地阉割：审计者 == 开发者

**事实**：73 份 handoff 中 `owner: claude-code` **59 份**、`owner: codex` 6 份。审计节点的 handoff 自己写着：
> `审计者 claude-code 新会话，与 development owner 同名——报告内已如实记录`（req-ext-group-002-independent-audit）

承载全部节点的是 **qwen3.8-flash / deepseek-flash 两个模型**（`qwen3.8-flash` 15 会话、`deepseek-flash` 27 会话）——所谓"不同代理"只是**同一个便宜模型换个新会话**。

**后果**：
- multi-agent.md 的核心论证是「cross-CLI audit kills self-confirmation structurally」，这里退化成了「同模型新上下文」，独立性只剩 prompt 层面。
- 29c38d14 一会话烧掉 **167.4M cache_read / 649 请求**，全部集中在 development 节点；审计节点（e3f45a65、480293a2）的 cache_read 反而只有 14M/21M。**审查比开发便宜得多，却把预算全砸在开发侧。**
- 16 次 `Agent` 子代理调用里，7 次集中在 750e1796（master 全量回归审计），其余会话为 0 —— 大部分审计是"新开会话"而非"换代理"。

### 3. 强制节点被口头跳过，且 AI 服从了

**两处硬证据**：
- `e315829f`（09-30 06:33）：用户「**我不想再接着审计了，你就这样吧，你现在能确保没有问题就行了，直接跳过审计下一步吧**」→ 随后 `U[06:36] 确认合并master`。该会话此前已明确记录审计结论为 **REJECT**（`P4 返回 ret=-99` 非确定性失败，无法归因，`不能升级为稳定 ACCEPT`）。
- `480293a2`（10-01 13:06）：用户「**你直接合并到master分支吧，我决定了**」。

**AI 的表现**：e315829f 里 AI **没有**接受第一句，权限层拦住了它两次，理由是「跳过审计≠授权合并进主干」，并要求 30 秒决策卡 —— 这是**正确行为**。但最终主人一句"我决定了"就合并了。

**这是 v1.7 自身的设计缺陷，不是 AI 缺陷**：approvals.md 第 6 条规定「只有用户的明确决定能填这个缺口」，等于把「防止跳过的强制门」的可绕过头交给了单句自然语言。**主人有合法的最终权限，但流程没有留下"这次跳过了什么"的强制留痕**——handoff 里只写 `audit: pending`，没有任何 waiver 记录。

**该做**：跳门必须落成任务记录里的 waiver 条目（跳过了什么、为什么、谁批的、风险是什么），否则事后无法区分"合规合并"和"口头放行"。

---

## P1 — 高频、正在持续造成浪费的缺陷

### 4. 60 请求软上限 = 完全失效，turn_guard 拦了 38 次全被无视

**数据**：30/55 会话触发 turn_guard，**平均 216 请求（3.6×）**，最高 649（10.8×）。d57ad93c 一个会话内被拦 3 次（78→176→282→327），三次全都继续干。

**根因是结构性的**：
- turn_guard 是 **Stop hook**，只在"模型准备结束这一轮"时才触发——它拦的是"收工"，不是"继续干"。所以它只能在会话末尾喊一句，模型接着干下一件事，hook 再喊。**38 次拦截的语义全是噪音。**
- hook 源码里 `if data.get("stop_hook_active"): return 0  # already continuing from our own warning: never loop` —— 它自己主动放弃了连续警告，只在 +20 增量时再喊一次。
- 用户侧的 22 次「[Request interrupted by user]」+ 5 次「ran out of context」才是真实边界。

**后果**：主工作流被切成 13 个会话的 transfer-001 需求（v1.7 大模式骨架是 6 节点），多出来的 7 个全是 turn-cap 强切造成的碎片。

**该做**：换 Stop hook 为 **PreToolUse hook**，在 60 之后直接阻断写类工具，让"超限"表现为"开不了工"而不是"收不了工"。

### 5. 交接词（8KB 级别）成了事实上的会话启动协议，handoff 文件形同虚设

**事实**：用户 **10 次**主动要求"给我下一个节点的交接词"，最长的一个 prompt 达 **16,244 字符**。而 v1.7 明确规定：
> `There is no "generate the standard handoff prompt" ritual: the next session is opened by any agent that runs state.ps1 and reads the handoff.`（session-lifecycle.md）

**但实际上**：每次"生成交接词"都产出了 1.5–16KB 的自然语言指令，用户粘贴到新会话。这套仪式**比读 handoff 贵一个量级**，而且把本该由 state.ps1 承载的上下文（分支/节点/证据路径/环境事实）搬进了 prompt。

**为什么 handoff 不够用**（这是真正的缺陷）：
- `state.ps1` 只输出 ≤20 行，**没有环境事实**。而项目最贵的隐性知识全在环境里：`connect.config 第 3 行必须是 CRLF（LF 会让 dist/Server.exe 启动 segfault）`、`filesPath` 相对/绝对路径决定沙箱判定、`dist/` 部署形态、`py -3.13`、MySQL `mydb`。这些只能靠人写进交接词。
- handoff 格式**已经膨胀**：12 份文件的 `next:` 或 `blockers:` 超过 200 字节，最极端的 `req-ext-transfer-001-repair-attribution` 的 `next:` 有 **1,400+ 字符**、`blockers:` 784 字节。**"≤10 行"的形式满足了，实质退化成小作文。**
- **`owner_agent` 那一行形同虚设**：59/73 都是 `claude-code`，写下来不等于有锁。

### 6. worktree 放置违规，且制造了"我跑的代码不是最新代码"这个坑

v1.7 规定 worktree 一律在 `build/wt-<agent>/`。实测 **23 次 worktree add**：

| 位置 | 次数 |
|---|---|
| `build/wt-*`（合规） | 20 |
| `.claude/worktrees/`（违规） | 1（e315829f 的 `s001-itest`） |
| `E:/qt/wt-audit-req-fs-003`（**仓库外**） | 1 |
| `../repo-chrome-wt`（**仓库外**） | 1 |
| `../wt-reaudit-0efccb9`（**仓库外，相对父目录**） | 1 |

**直接后果**：`E:/qt/xiangmu` 主工作区长期停在 `feat/req-ext-transfer-001-parallel-transfer@333bef2`，而 master 的修复在 `build/wt-fix-ux001/dist/`。于是主人 10-01 21:03 在主工作区构建，把**没有修复的旧 exe 写回 `dist/`**，然后质问：
> 「为什么master构建部署到dist就会去掉并行传输功能？」
> 「但是你当前分支的我运行之后显示的窗口尺寸大小也没有改变啊，你是不是根本没修好」
> 「你没有将你的修复分支merge到master分支吗」

AI 花了一整轮解释「分支 ≠ 工作区目录」。**这不是用户理解力问题，是流程没有让"当前运行的是哪个 commit 的产物"成为开局第一屏的事实。** `state.ps1` 输出的 `worktrees:` 只列路径，不提示"dist/ 里那两个 exe 是哪个分支编的"。

### 7. `dist/` 被 gitignore，但它是唯一的运行入口——构建与部署彻底脱钩

`.gitignore` 同时忽略 `/build/` 和 `/dist/`，而 `build.ps1` 只管 `build/build-*`，**没有任何脚本负责把产物部署到 `dist/`**。于是 `dist/Client.exe` 的实际来源（哪个分支、哪个 hash）成了不可追溯的隐式状态，多个会话都在打补丁式处理这件事（`7173e812` 专门做了 `small/build-env-and-dist-deploy` 分支解决 `DESTDIR`，`95039b1c`/`cbd0acc8` 反复手工 sync 二进制）。

**这是 P0-6 的根因**，也是唯一真正会"骗过主人眼睛"的缺陷——二进制里编的是哪个分支的源码，从来没有一个可执行的判据。

---

## P2 — 治理一致性缺陷

### 8. tier 分布严重失真：64% 是 LARGE

73 份 handoff：`LARGE 47` / `MEDIUM 12` / `SMALL 11`。

tiers.md 说 LARGE 只用于「公共契约/架构变更、跨会话、多方审批」，且
> `never default upward for safety-theater`

实际执行中 LARGE 成了默认档。连带后果：LARGE 强制 independent test + independent audit 两个节点，于是 13 会话的 transfer-001 成了必然。**档位通胀 → 节点膨胀 → 会话膨胀**，而膨胀出来的会话全是 turn-cap 强切的碎片（第 4 点）。

### 9. 违规报告几乎不存在

invariant 16 要求「观察到状态与预期节点序矛盾时，一句话向用户报告并提最小恢复方案」。全库搜索 assistant 文本，命中违规自报**仅 5 条**，且其中 3 条是 e315829f 对"跳过审计"的正确拒绝、1 条是 e3647ef9 的正常集成三事实行、1 条是 7a81c68e 的正常审计结论。

**真正该报未报的是**：`test: 0 registered` 这个事实贯穿全程、每个会话都写了 `GREEN-WARN`，却**没有一个会话把它报成流程缺陷并提出修法**——它被当成环境属性接受了 55 个会话。

### 10. SKILL.md 全文被注入 48 次（408 KB），违反 invariant 11

invariant 11 明文「Skill files never read wholesale in-session」。实测每个会话首条用户消息都带 `Base directory for this skill: .../SKILL.md` + 全文（8,695 字节），**48 次，累计 408 KB**。这是 Skill 工具的注入机制（每次调用都重新注入全文），CLAUDE.md 第 3 条「只读 INDEX.md 路由到的文件」被完全抵消——治理层每会话全文进上下文，参考层才按需读。**净效果是把设计成"薄"的治理层做成了"厚"。**

### 11. 审计报告积压 14 份，`state.ps1` 永远显示 pending

`build/evidence/` 下 14 份 `audit-report*.md`，`audit: pending` 的 handoff 有 **15 份**，只有 6 份标了 accepted/resolved。state.ps1 的 pending 过滤按最新 handoff 的 `audit:` 字段判定，**历史审计报告永远不会被清理**，所以每次开局都在提示"有 N 份待处理审计报告"（含已被后续审计推翻的旧报告，如 `5a07316` / `cf4f74f` / `9cf701c` 三份 FILE-001 报告并存）。**噪音源永久化，主人学会了无视这个信号。**

### 12. 提交历史里的 docs-only  ceremony 与 handoff 编码反复

git log 里有大量纯文档提交挂在需求分支上（`docs(transfer-001): ...` × 8），虽然 branching.md rule 3 允许 docs-only 走 trunk 直提，但实际都走了分支 + no-ff merge。另外 `8f1c3d5`（交接文件改 ASCII 行尾）和 `84bfb26`（交接文件转 UTF-8 BOM）两次提交，是同一个坑在两个方向上来回踩：**PS 5.1 的 ACP936 解码要求**。这类"机器适配摩擦"本该由 pipeline 脚本一次性吃掉（`state.ps1` 自己加 `-Encoding UTF8` 即可），却各自烧了一次提交和一次构建。

---

## 附：可直接落地的修复优先级

| 优先 | 修复 | 一句话理由 |
|---|---|---|
| 1 | 把 8 套 e2e 注册进 `test.ps1` 的 `$TESTS`，`impact_status` 改真实运行 | 三门里两门空转，其余所有门禁都是纸糊的 |
| 2 | `state.ps1` 增开一屏：`dist/` 产物来源（分支 + hash + 构建时间） | 直接消灭 P0-6 的"我跑的代码不对"整类问题 |
| 3 | turn_guard 从 Stop hook 改 PreToolUse hook | 软上限已经 38 次全废，早失败比晚收工便宜 |
| 4 | 环境事实（CRLF/绝对路径/部署形态/py 版本/MySQL）落进一个 `project-docs/ENVIRONMENT.md`，state.ps1 每次引用 | 交接词从 16KB 降到 1KB 的前提 |
| 5 | 跳门/waiver 强制留痕（skip 了什么 / 谁批 / 风险） | 不留痕则事后无法区分合规与放行 |
| 6 | 审计节点改用**不同 CLI**（Claude Code 审计 + Codex 开发） | 现同模型换会话，独立性只剩 prompt |
| 7 | tier 重判：把非契约类需求压回 MEDIUM | 档位通胀是会话膨胀的上游 |
| 8 | pending audit 报告在 `audit: accepted` 后自动归档清理 | 永久噪音源，主人已学会无视 |