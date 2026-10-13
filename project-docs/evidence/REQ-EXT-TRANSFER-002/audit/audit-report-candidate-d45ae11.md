# REQ-EXT-TRANSFER-002 独立审计报告（candidate d45ae11）

- 节点：independent audit（Development OS v1.7 / LARGE；新会话、只读、detached 工作树）。
- 对象：候选 `d45ae11`（完整 `d45ae1173bb58a3b8551983d0ac12bdeb6c6c977`）＝分支 `feat/req-ext-transfer-002-download-pause-resume` 上 `d85e39f..d45ae11` 两提交（`f820e61` feat 实现+套件+manifest；`d45ae11` chore S3 签名收紧）；基线 `master@d85e39f`。
- 审计快照：`E:/qt/xiangmu/build/wt-audit`（detached @d45ae11；起始 `git status --porcelain` 为空、零提交）。
- 输入：test-report `claude-req-ext-transfer-002-independent-d45ae11/`（含 frozen-content.log / static-independent.log / suite-sweep / 归因日志）＋ `project-docs/tasks/REQ-EXT-TRANSFER-002/{task,design}.md` ＋ 四节点 handoffs ＋ `git diff d85e39f..d45ae11` ＋ transfer-003 审计先例。**未做全仓扫描**；依赖闭包用 `git grep @d45ae11` 调用点枚举手工给出（快照内未建 codegraph 索引；与 manifest `code_intel.impact_status: not_applicable` 及 003 先例同口径）。承重事实全部由本人从 git 对象与源码重新导出，不采信 development/test 的结论文字。
- **结论：FAIL —— 阻塞项 1 条：FINDING-AUDIT-1（客户端续传失败路径被收尾 43 翻转为 Done，违反 design §4.3 必做防御与不变式 I2，击穿 §11 批准卡承诺）**。其余核验全部通过：冻结面逐字节==§9 批准稿（本人独立导出）、C1-C10/S1-S4 除 §4.3 缺口外逐条成立、I4 双端锚点成立、S3 收紧行为等价、测试报告 5 条非阻塞发现全部维持。

## 0. 只读边界与独立性披露（照实）

- 只读边界：未 `commit/push/merge/reset/branch/stash`；未改审计工作树内任何生产源码、配置、测试、验收条件。写入仅限：`build/evidence/claude-req-ext-transfer-002-audit/<full-hash>/`（本报告＋`verification.log`）与工作树内 gitignored 的一次性工间产物 `build/wt-audit/build/audit002/`（diff 导出、双端 protocol.h blob、§9 提取件、逐字节比对中间件）。无临时反向探针（本节点结论均由静态读码＋git 对象导出，未复跑套件——复跑属 test 节点，本轮其结果已由测试节点独立取得且本报告未推翻其任何一条）。
- 独立性披露（不掩盖）：`owner_agent: claude-code`，development 与 independent test 亦 `claude-code`；本机 Codex CLI 不可用 ⇒ **跨 CLI 不成立，同代理审计**（protocol 要求记录显式理由：无第二可用代理时，同会话上下文的自确认风险由"全新会话＋独立 detached 工作树＋承重事实逐条自行重导"对冲；本报告的冻结面比对、行号裁决、缺口定位全部现场重做，无一转述）。实际独立性＝新会话＋独立工作树＋事实重导，**不等于**跨工具链独立；残余限制在册，若主人要求跨 CLI 审计请在接受本报告前裁决。
- 边界量具缺口（FACT，延续在册）：主工作树 `pipeline/audit.ps1` 为未填骨架（F-5），自动冻结边界判定失真；**本报告 §1 的边界结论由人工逐字节核验产出，不依赖该门禁**。

## 1. 冻结面边界（必查 1）— FACT

| 核验 | 方法（本人现场重导，命令与输出见 `verification.log`） | 结果 |
|---|---|---|
| 冻结面规模 | `git diff --numstat d85e39f..d45ae11 -- 双端 protocol.h` | `4 0` + `4 0`；唯一 hunk |
| 删除行 | `^-[^-]` 计数 | **0**（append-only、零重编号） |
| 落位 | hunk 上下文 | `MAX_PARALLEL_DOWNLOADS = 3;` 之后、`#endif` 之前（§9 要求位置）；S-02 语义成立 |
| 双端逐字节 | `git show` 两 blob 尾部 `cmp`（blob 级，LF 行尾，od -c 复核无 `\r`） | **IDENTICAL** |
| 内容 == §9 批准稿 | `awk` 提取主工作树 design.md §9 栅栏 4 行 vs 双端新增 4 行 `cmp` | **MATCH-RAW-BYTES**；段 sha256 前缀 `5a053019481482f5` 三方同一（Client／Server／批准稿），无需行尾归一化 |
| 影响面 | `git diff --numstat d85e39f..d45ae11` 全量 | 10 文件＝双端 protocol.h、Client/transfertask.{cpp,h}、Server/{msghandler,mytcpsocket}.{cpp,h}、新套件 verify.py、manifest；**无计划外文件，未触碰任何既有 harness** |

- **判定：I6 成立；合并三事实线第 2 条（冻结面 diff 仅 §9 批准块）前置证据成立。**
- 权威文件口径（FACT，配合 N-4 看）：`git show d45ae11:.../design.md` **不存在**——design.md/task.md 被 `.gitignore:84` 排除在库外；§9 批准稿的比对基准是主工作树工作文件。批准事实链在 handoff/§11 文本中，git 对象链不含批准稿本体，合并后需按 LARGE 收口归档（见 §7）。

## 2. 实现层 C1-C10 / S1-S4 逐条裁决（必查 2）— 行号均指 `@d45ae11`

### 2.1 客户端（Client/transfertask.cpp/.h）

| # | 裁决 | 事实锚点 |
|---|---|---|
| C1 | **FACT 成立** | `pause` 下载分支 :925-939：Queued→`m_downloadQueue.removeOne`；Running→`sendAbortIfActive`(:931)→`closeRecv`(:932，flush 后字节留盘)→名额归还(:933)→**摘旧 id 映射(:934)**→Paused→saveSnapshot→pump。Queued 分支不动分片 |
| C2 | **FACT 成立** | `resumeDownload` :970-990：先 `m_idToKey.remove(t.id)`(:972) 再新 id insert(:974)；起点=`QFileInfo(savePath).size()`，缺失→savePath 清空、起点 0(:976-981)；`t.doneBytes`=起点；resume/retry 共用(:997/:1080)。旧 resume 直插映射不摘旧号的 G3 隐患已被同路径消灭 |
| C3 | **FACT 成立** | :494（40）、:511（105）：`memcpy(caData+DOWNLOAD_RESUME_OFFSET, &doneBytes, 8)`，小端 u64 与既有字段同法；`++m_activeDownloads`(:498/:515) |
| C4 | **FACT 成立**（判定逻辑）：①:759-763/:818-822（!ret/code≠0 且 requested>0→「服务端未接受续传起点」）；②:765-768/:833-837（accepted≠requested→同 ①，含旧服务端恒 0）；③:770-776/:839-845（requested>0 且 size≠totalBytes→删分片+起点归零+savePath 清空+明确失败）；④:778-781/:848-851（requested>0→`resumeReceiving`，否则→既有 `beginReceiving`）；⑤:777/:847（统一回写 `totalBytes`）。`checkResumeRespond` :611-616 两判定与 design 逐字一致。**但 ①②③④ 的失败后果面有阻塞缺陷，见 §4** |
| C5 | **FACT 成立** | `resumeReceiving` :590-606：先 `closeRecv`；空 savePath→「无法打开下载分片」；`ReadWrite`+`seek(offset)`；失败同文案；成功 `doneBytes=offset`。(N) 递推仍仅属 `beginReceiving`（:552-587 未动） |
| C6 | **FACT 成立** | `onDownloadFinish` :884-886 已无 `QFile::remove`（diff 删除的 3 行即此），失败保留分片（D7） |
| C7 | **FACT 成立** | `cancel` :1027-1039：Running 先 `sendAbortIfActive`(:1028)；无条件 `QFile::remove(savePath)`(:1035)+清空；摘映射、移任务、saveSnapshot |
| C8 | **FACT 成立** | `sendAbortIfActive` :899-904 条件放宽为 `state==Running`；上传分支调用面不变（upload 路径内 `isUpload` 恒真，行为等价） |
| C9 | **FACT 成立** | saveSnapshot 仅下载写 `savePath`(:81-84)；restore 下载分支 :127-136：`doneBytes`=分片物理大小或 0、Paused。旧快照无字段→空→0→「继续」=今日整文件行为（I3 代码级成立）。`m_nextKey=1`（.h:135），key==0 哨兵有效 |
| C10 | **FACT 成立** | 头注释 :48-51 改写（「下载整文件重传」口径→分片大小续传）；面板文案 :1105、:1153 同步 |

### 2.2 服务端（Server/msghandler.{cpp,h}、Server/mytcpsocket.{cpp,h}）

| # | 裁决 | 事实锚点 |
|---|---|---|
| S1 | **FACT 成立** | `handleTransferAbort` :403-430：`id!=LEGACY` 才进；先 `findUpload`（AUD-4 在途写延迟释放逻辑逐语句原样保留，仅嵌套层级+1）；未命中→`m_downloads.contains`→`removeDownloadStream`(:420-423)；两者皆无→ret=false；**id=0 一律不受理**；108 原样回带 id(:428) |
| S2 | **FACT 成立** | 40 分支 :216-219、105 分支 :308-312 各以 `memcpy(caData+DOWNLOAD_RESUME_OFFSET)` 同读法写入 `m_MsgHandler->m_downloadResumeOffset`（新公有成员，msghandler.h:146-148，风格同 `m_downloadSlotAvailable`）。同连接顺序处理 ⇒ 每次消费点(:1776/:1793/:2333/:2244)前必为本次新鲜值，无陈旧读 |
| S3 | **FACT 成立** | `startDownloadStream` :458-492：open→`totalSize`→防护 `startOffset<0 || >totalSize || seek 失败` → 删条目+`seedDownloadFinish(false)`，**不建流不静默**；通过则 `sentSize=startOffset`(:486)。==size 的 0 字节正文+43 success（D5）由 tick `remaining<=0`(:601-606) 保证，与 T2-03 实测一致 |
| S4 | **FACT 成立** | 41：仅 `ret=true` 写采纳起点(:1791-1795)，槽满早退(:1741-1751)与 >size 拒绝(:1775-1782，canonical 同清空→socket 不建流)均零区间（R4）；ret=true ⇒ canonicalPath 非空 ⇒ 建流，「受理且建流」耦合成立。106：`responseType==106 && !canonical.isEmpty()` 才写(:2240-2246)；拒绝体 `body.clear()`(:2347)，caMsg 信封布局未动（I7）；`-2107` 先于 `-2108`（见 N-3） |

### 2.3 不变式 I1-I7 独立定位

| 不变式 | 裁决 |
|---|---|
| I1 旧客户端逐字节一致 | **FACT**：`mkPDU` 全量 memset 双端在库（基线既有）→旧端 [32..39] 恒 0；新端恒写同值 0（:494/:511，doneBytes=0），字节无差；105 caMsg 零改动。wire 复证＝T2-14（verify.py:366-368 id=0 全流程逐字节）＋既有 6 套旧帧形态全绿（test §5） |
| I2 新客户端×旧服务端明确失败 | **代码级 CLAIM→被 §4 阻塞项击穿**：②判定成立(:765-768)，但失败态随后被在途 43 翻为 Done，「明确失败」不持久 ⇒ I2 不成立，见 FINDING-AUDIT-1 |
| I3 旧快照恢复 | **FACT 代码级**：restore :127-136 空字段→起点 0→beginReceiving＝今日路径。运行态属 GUI 项 6（NEEDS-HUMAN） |
| I4 分片大小不变式 | **FACT 双端锚定**：起点==分片物理大小的代码位＝`resumeDownload` QFileInfo :976-979、restore :131-133；分片只随已收块顺序增长＝`onDownloadData` :868-873（写失败即中止、护栏 :867 recvFile 空即丢帧，暂停/取消/完成处 `closeRecv` flush）⇒ 分片大小 ∈ [0, 服务端已发]。wire 测试锚点＝T2-06（:328-330，首块首字节==源[M]、`prefix+suffix==BIG` 全等）与 T2-11b（:409-410）；客户端行侧（:976-979/:131-133）**无运行时宿主**（N-1），由读码定论 |
| I5 id 唯一与即时失效 | **部分成立**：pause/resume/retry/cancel 四点摘除已实现（:934/:972/:1038/:1063）；**finishTask(下载) 一点未实现**（:521-540 无 remove）⇒ §4.3 清单四项缺第 4 项 → FINDING-AUDIT-1 |
| I6 冻结面仅 §9 | **FACT**（§1，本人逐字节重导） |
| I7 caMsg 逐字节不变 | **FACT**：40/105 caMsg 构造未动（diff 面 :489-513 仅加 caData 写）；41/106 的 caMsg 体零改动，106 仅 caData[24..31] 新增（D4 批准面） |

## 3. d45ae11 的 S3 签名收紧本身（必查 3）— FACT

- `git diff f820e61..d45ae11` **仅动 `Server/mytcpsocket.h` 声明 1 行**（去 `= 0` 默认参＋注释改写），`.cpp` 零改动。
- 调用面：`git grep startDownloadStream d45ae11` 全仓 4 命中＝调用点 :228/:320＋定义 :458＋声明 .h:54；两调用点在 **f820e61 即已显式 3 参**（`git show f820e61:Server/mytcpsocket.cpp` 复核）。
- 无默认参后任何新增漏参调用将编译失败＝收紧收益；现存二进制行为等价声明**成立**（同一代码路径，签名层无运行时差）。d45ae11 提交信息自称的复跑口径与 test 节点 §5 实测一致，不另翻案。

## 4. 阻塞发现

### FINDING-AUDIT-1（**FACT，阻塞**）——续传护栏的失败态被收尾 43 翻转为 Done

**事实链**（逐行读码定位，静态控制流确定）：

1. design §4.3（**必做防御**）明文列举摘除点四项：「pause（旧 id）、resume/retry（旧 id）、cancel（旧 id）、**finishTask（下载）**」。实现只落前三项（:934/:972/:1038/:1063）；`finishTask` :521-540 的下载分支无任何 `m_idToKey.remove`。
2. C4 的 ②（:765-768/:833-837，旧服务端采纳区间恒 0）、③（:770-776/:839-845，服务端换件 size 不符）、④失败（:778-781/:848-851，分片打开失败）三处 `finishTask(*t, Failed, …)` 发生时，**服务端流已在建或即将在建**（41 ret=true／106 code=0 ⇒ socket 层 `startDownloadStream`），且这些失败路径**不发 107**。
3. 失败任务此时：`closeRecv` 已置 recvFile=nullptr ⇒ 后续 42 被 :867 丢弃（不串写，这点成立）；**但 43 收尾帧经 `onDownloadFinish` :880 `taskByTransferId(id)` 仍命中该任务（映射未摘）、:886 无条件 `finishTask(Done)`** —— 无状态合法性检查。
4. 结果：被明确拒绝的续传在毫秒级后被翻转为 **Done**。旧服务端场景：面板显示「完成」而磁盘只有旧前缀分片（**静默损坏件交付**）；服务端换件场景：显示「完成」而磁盘文件已被 ③ 删除（**无文件**）。
5. 违反面：§4.3 必做防御（未完整实现）、不变式 **I2**、design §10 风险表「新客户端×旧服务端静默错位 → D6 护栏」的缓解承诺、§11 主人批准卡的业务语言承诺「新客户端配旧服务端会**明确报错**而非拼坏文件」；验收 5/6 口径连带击穿。
6. 既有面备注：同机制在基线即存在（d85e39f `beginReceiving` 本地打开失败→Failed→在途 43→Done），属既有代码惯性；**本候选把触发面从罕见本地 IO 故障扩大到协议护栏主路径**（旧服务端、服务端换件——恰是 design 自设的两条主防线），故归因本候选。
7. 为何此前未暴露：25 断言是裸 socket 线面，不执行 transfertask.cpp（N-1）；§6.2/§6.3 均无「护栏失败后收尾」条目（F-3/N-2）；GUI 8 项未跑。**定论性人工检查**（供主人修复后验证）：旧服务端 exe × 新客户端，暂停→分片留存→点继续 → 预期行状态停在「失败：服务端未接受续传起点」，不得变「完成」。

**建议修复（不代做）**：在 `finishTask` 的下载分支摘除 `m_idToKey[t.id]`（design §4.3 自身指定的形状，一处补全，同时关掉基线残留的同类翻转）；可选加固＝`onDownloadFinish` 加 `t->state != Running → return` 双保险；②/③/④ 失败时顺带 `sendPause(t.id)` 可提前止流省带宽（design 未要求，非必需——摘映射后帧自然落空，流自然收尾，名额随 43 回落，服务端语义无依赖）。修复后补一条静态契约断言（verify.py S 面：finishTask 下载分支含摘除）以钉住回归。

## 5. 非阻塞发现：测试报告 F-1..F-5 逐条表态（必查 4）＋本审计新增

- **F-1 维持**（FACT，亲验）：`unique_login_e2e.py:202-206` 的 E7b＝`git diff master...HEAD --name-only` 命中 protocol.h 即红，对任何**经批准的**冻结面分支结构性红。处置归 design owner（改口径为「仅 §9 批准块」或移出全分支回归面），本节点与 test 节点同样**未放宽**。
- **F-2 维持**（FACT，既有）：`DIST=os.path.abspath("dist")` 按 cwd 解析的错跑隐患，先于本候选；处置＝owner 随 harness 小修统一为文件位置解析。
- **F-3 维持，并升级为必须处置**：验收 5「暂停期服务端换文件」的矩阵缺口是 **FINDING-AUDIT-1 得以溜过测试节点的直接盲区**（②③④ 三个新失败点的后果全无自动化、全无 GUI 条目）。它是 design 矩阵缺口 ⇒ **处置方向归 design owner**（本人不代裁、不放宽）；但修复 A-1 后该场景至少要有一条目（GUI 项或静态断言）承接，否则同类回归无锚。
- **F-4 维持**（FACT，既有全库模式）：新 transfer002 条目（manifest:160-169）沿袭 `isolation.filesys: "./Server/filesys"` 失真写法；随 scripts SMALL 一并更正。
- **F-5 维持**（在册既有）：audit.ps1 骨架失真与 `-Deploy` 陈旧覆盖缺陷均非本候选；本审计边界结论已按 protocol 以人工逐字节核验独立产出（§1），不依赖该门禁。

新增（本审计）：

- **N-1（非阻塞；FACT）客户端状态机无运行时宿主**：verify.py 自述口径诚实，但 C1-C10 的**动态正确性目前完全押在 8 条 GUI 人工项＋读码**上；这是 A-1 这类「线面绿、状态机错」缺陷的结构性温床。长期建议（归 owner/repo 面）：客户端逻辑抽可测面或加最小 wire-driver 宿主；本任务不阻塞于此。
- **N-2（非阻塞；FACT，缺口在册）I2 的运行时验证无归属**：§6.2/§6.3 均无「新客户端×旧服务端」条目（批准卡的业务承诺恰是它）。与 F-3 同归 design owner 处置（补 GUI/静态条目；旧服务端二进制可自 `d85e39f` 现构，属可做）。
- **N-3（非阻塞；FACT，仅记录）105 拒绝优先级**：`msghandler.cpp:2321-2325` 槽满 `-2107` 先占，`-2108` 仅在 `code==0` 后判（:2333）。§2.3 表未声明优先级；客户端 requested>0 时两码同文案（:818-822），语义无碍。
- **N-4（非阻塞；FACT）批准稿权威在库外**：design.md/task.md 被 `.gitignore:84` 排除、不在任何 git 对象内；§9 逐字节核对只能锚主工作树工作文件（本人已做）。合并收口时把批准卡/§9 文本归档进 `project-docs/evidence/REQ-EXT-TRANSFER-002/`（入库路径），消除「证据文件漂移即结论失效」面。

## 6. 合并三事实线现状

| 事实线 | 现状 |
|---|---|
| ① 独立审计报告在册**且被接受** | 本报告＝在册（pending）。**且因结论 FAIL，此线在阻塞项修复前无意义**；修复+复测+新候选上重出审计后方可谈接受 |
| ② 冻结面 diff 仅 §9 批准块 | **已达成**（§1，本人逐字节独立导出；不依赖 audit.ps1 骨架） |
| ③ 合并结果 pipeline 绿 | 未产出（归 owner 合并时）；口径注意：test 门册 waived→UNTESTED、audit.ps1 失真在册（F-5），「绿」须写明＝build.ps1 GREEN＋13 套驱动串行（11 绿＋2 既有归因红）＋新候选上 transfer002 全绿 |

## 7. 给主人的处置建议（本节点零修复）

1. **owner（claude-code）新 repair 会话**：落 FINDING-AUDIT-1 修复（`finishTask` 下载分支摘映射，按 §4.3 形状；建议带 `onDownloadFinish` 状态守卫双保险），补 1 条静态契约断言钉回归；重跑 build＋13 套＋transfer002（确定性复跑口径）。
2. **design owner**：F-3/N-2 给处置方向——§6.2/§6.3 至少补「护栏失败后收尾不被 43 翻转」与「新客户端×旧服务端」条目；同时裁决 F-1 的 E7b 口径。
3. **主人**：修复后新候选上，test（新会话）→ audit（新会话）两节点重走，再做 GUI 8 项一轮（含 A-1 的旧服务端人工复验一步，§4.7 判据）。
4. **repo-contract SMALL（另立，不关本任务）**：audit.ps1/artifacts.ps1 修复＋F-2/F-4 元数据更正＋N-4 证据归档惯例。

## 8. 证据清单（本目录）

`audit-report.md`（本文件）、`verification.log`（本人现场重导的全部命令与原始输出）。工作间产物：`build/wt-audit/build/audit002/`（impl.diff、manifest.diff、双端 protocol.h blob、§9 提取件、cmp/sha 中间件；gitignored，随工作树销毁）。快照 `git status --porcelain` 起始为空、收尾见节点最终答复。
