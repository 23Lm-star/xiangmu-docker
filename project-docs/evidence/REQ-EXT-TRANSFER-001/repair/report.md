# REQ-EXT-TRANSFER-001 修复报告（repair 节点）

- 节点：repair（Development OS v1.7 / LARGE；owner 会话；主人 2026-10-01 仲裁"接受审计结论、进入修复"已授权开工）。
- 候选（新）：`119e6d3`（`fix(transfer-001): 修复 AUD-1 + AUD-2`）。上一候选 `520c714`（audit FAIL 对象）。
- 基线：`master`。分支：`feat/req-ext-transfer-001-parallel-transfer`。
- 授权范围（仲裁记录，`task.md` 2026-10-01 仲裁条）：(1) 修 AUD-1；(2) 修 AUD-2；(3) F-1 处置 = 修订 design §6.2 T-06 文本为 3 条流（**不改已批准的 D6**）；(4) 补客户端级 3 文件并发上传用例；(5) 重跑 pipeline + 七套件；(6) 更正 test 报告 §8 证据清单（EV-1）。
- **结论：6 项全部执行完毕；三门已跑（build GREEN / test GREEN-WARN / audit RED=预期冻结面命中）；七套件 43/13/41/48/46/45/46 全绿。** AUD-3/AUD-4（非阻塞）与 design §6.3 人工项**未在本次范围内**，如实列于 §6。

---

## 1. AUD-1 根因（审计结论复核）

审计对 `520c714` 的结论**成立且可复现**：97 秒传预检查在 `m_uploads[0]`（旧单流槽）建立暂存上下文后**从不释放**，而 `activeUploadCount()` 等于 `m_uploads.size()`，于是并行客户端"每文件先 97 再 29"的第 3 路 INIT 被上限误拒、且被客户端标记为**失败**。

新增的 T-19 用例（§4）在**关闭修复**的同一源码上复现出的正是审计推演的那一行：

```
[FAIL] T-19b 3 路 29→30 均 ret=true（AUD-1 回归：97 占位不得吞上传名额） :: [True, True, False]
[FAIL] T-19c 3 条 entry 内容与源逐字节一致 :: lens=[915, 1315, 0]
```

## 2. AUD-1 首版修复被"六套件零脚本改动"否决（重要过程证据）

首版修复思路 = 审计建议二"采纳后立即移除 id=0 槽"的加强版：**在 `uploadfilefastcheck()` 所有返回路径上清掉 id=0 槽**（RAII `LegacySlotGuard`）。

结果：`parallel_e2e` T-19 全绿，但 **`fastupload` 套件 24/46 —— 回归被捕获**。

根因（README 级事实）：**旧客户端（REQ-EXT-FILE-002 形态）未命中后不发 29，直接发 31 正文**（`fastupload_e2e.py:133-145`，`upload_full = send_chunks`），服务端正是靠那个 `m_uploads[0]` 上下文接收正文并落定。清掉它即破坏 **I1/I2（旧客户端零改动）** 与 design §6.1 的"六套件零脚本改动"。

> 该回归**不是**测试脚本的问题，而是首版修复越过了兼容面。这条正是 audit 报告 §E"settling check / 建议修复"未覆盖到的边界，也是本轮修复最有价值的发现。最终方案据此改写（§3）。

## 3. AUD-1 定稿修复（保留兼容面 + 名额不再被占）

三处改动，语义为"**97 暂存占位参与兼容，但不参与名额**"：

| 位置 | 改动 |
|---|---|
| `Server/msghandler.h` `UploadCtx` | 新增 `bool fastcheckPlaceholder = false`（97 纯暂存槽标记） |
| `Server/msghandler.h` `activeUploadCount()` | 由 `int(m_uploads.size())` 改为**遍历计数、排除 `fastcheckPlaceholder` 条目** |
| `Server/msghandler.cpp` `uploadfilefastcheck()` | 沿用原"remove(0)+currentUpload()"取得暂存槽后置 `ctx.fastcheckPlaceholder = true`（**不删槽**，旧客户端 97→31 仍可用） |
| `Server/msghandler.cpp` `uploadfileinit()` | 29 采纳 97 暂存（`adopt` 命中）且成功建槽后，释放 id=0 占位（该槽对并行客户端已无用途，并消除与真实槽重复引用同一 `upload_session` 行的隐患）；置位变量 `adoptedFastcheck`，`remove` 放在所有 `ctx` 引用之后（避免 QHash 重排导致的悬垂引用） |
| `Server/mytcpsocket.cpp` `handleUploadWriteFinished()` | 该槽首次成功接收正文（旧客户端 97→31 直传路径）时 `fastcheckPlaceholder = false` —— 占位转为普通活跃槽，**重新计入名额**（D7 语义不被稀释） |

**名额推演（并行客户端 3 新文件，97→29 交错）**：`97#1→{0*}` 计 0；`29#1→{0*,1}` 计 1，采纳后 `{1}`；`97#2→{1,0*}` 计 1；`29#2` 计 2 → `{1,2}`；`97#3→{1,2,0*}` 计 2；`29#3` 计 3 → **全部受理**（`*` = 占位，不计数）。

两处日志（`uploadfileinit` / `[GROUPFILE]` 的 `active=`）同步改用 `activeUploadCount()`，避免日志数字与实际判定口径不一致。

## 4. 新增用例 T-19（客户端握手保真；AUD-1 回归锚点）

`project-docs/evidence/REQ-EXT-TRANSFER-001/parallel_e2e.py` 追加 T-19：**逐字节复现真实客户端的发送序列**（`97#1,29#1, 97#2,29#2, 97#3,29#3`），断言 3 路 98 `ret=true 且 hit=false`、3 路 30 `ret=true`、3 条 entry 内容与源逐字节一致。用例数 43 → **46**。

**反向验证（no-forged-pass）**：临时把 `activeUploadCount()` 改回 `m_uploads.size()`（占位计回名额）并重建，同一套件复跑 → **T-19b/T-19c 精确复现 AUD-1**（`[True, True, False]`，`lens=[915, 1315, 0]`，44/46）。探针已完全回滚（`git diff | grep NEG-PROBE` = 0）。

## 5. AUD-2 修复（`Client/transfertask.cpp` `onUploadInitRespond`）

原实现：30 的 `ret=false` → `finishTask(Failed)`（违背 design §3.3 "把被拒当继续排队"）。现实现：

1. 迟到响应保护：`t->state != Running` 直接忽略（任务已被暂停/取消）；
2. 归还在 `startTask` 计入的本地名额；
3. 若仍有在途上传 → 任务回 `Queued`、重新入队尾、`needFastcheck=false`（首次 97 已判未命中，重试直接走 29，与 `resume()` 同语义），**不立即重发**——重试由"槽位释放事件"（`finishTask→pump`）驱动，不轮询（design §3.3 原文）；
4. 若归还后已无在途上传（该拒绝不会自愈）→ 收口为 `Failed`，避免任务永久静默停在"排队中"。

第 4 条是对 design §3.3 的一处**实现级补充**（原文只写"不报错、不丢文件"）：它只在"服务端拒绝但本连接没有任何在途上传可释放名额"这一自相矛盾态下触发，作用是给一个不可自愈的拒绝一个有用户可见出口（可重试），而不是让它无限排队。

## 6. F-1 处置（主人仲裁项，已执行）

`design.md` §6.2 T-06 由"8MB × 1 + 64KB × 3"（4 条流，与批准的 D6 上限 3 冲突、按原文不可能通过）修订为：

> `T-06 | 公平性：512KB × 1 + 1KB × 2 同时下载（**3 条流**，与 D6 上限一致） | 2 个小文件的 43 均早于大文件的 43 | 6`

**D6（上传 3 + 下载 3）与 `MAX_PARALLEL_DOWNLOADS=3` 未改**。并新增 T-19 行（标注"repair 追加，AUD-1 回归锚点"）。§3.1 "id=0 槽计入上传数"一句补加了 AUD-1 的确切适用条件（仅"旧客户端真做一次单流上传"时计入；97 预检查不占名额）。套件头部原 D-a 差异说明同步改为"已收口"。

## 7. EV-1 更正（test 报告 §8）

`independent-test/test-report.md` §8 删除了**不存在的** `manifest-build-*` / `manifest-test-*`，并加注：证据 manifest 由 `Stamp-Evidence` 写入 **`build/evidence/current/`**（gitignored 传输区），不经本目录归档。

## 8. 未修复 / 未覆盖（如实列出，不得读作 pass）

- **AUD-3（迟到 98 按序号误归属新任务）**、**AUD-4（107 与在途写并发、暂存一致性未证明）**：非阻塞，**未在本轮授权范围**，保持原状；两者的 settling check（客户端级/GUI 级）仍未执行。建议在 integration 前由主人决定是否单独开修复。
- **test 报告 F-2..F-5**（T-14 代理断言、T-02/F-3 客户端队列自动化缺口、T-08 阈值、F-5 人工清单补项）：F-3 的"客户端本地队列"仍无自动化覆盖（本轮只补了 wire 级握手保真 T-19，不是客户端队列断言）。
- **design §6.3 五条人工清单（NEEDS-HUMAN）**：本轮**未执行**（需 GUI 人工目视），**不计 pass**。其中第 2/3/5 条（任务列表并发刷新、暂停/继续/取消按钮、暂停后进度不回退）与 AUD-2 的新回队语义直接相关，请人工节点一并复核"超限排队中 → 槽位释放后自动开始"。

## 9. 验证（全部在本候选 `119e6d3` 上执行）

### 9.1 三门

| 门 | 结果 | 日志 |
|---|---|---|
| `pipeline/build.ps1` | **GREEN**（Client/Server 均 OK，无新增告警；`-Wall -Wextra` 下本次改动零告警） | `build-119e6d3.log` |
| `pipeline/test.ps1` | GREEN-WARN：`0 automated tests registered`（仓库口径：回归由 e2e 套件承担） | `test-119e6d3.log` |
| `pipeline/audit.ps1` | **RED**：`FROZEN-PATH HIT: Client/protocol.h` + `Server/protocol.h`；`impact-check: NOT_APPLICABLE`；**无 evidence 缺失告警**（`build/evidence/current/test-119e6d3-*.log` 已由 test.ps1 落盘） | `pipeline-audit.log` |

audit RED = **预期冻结面命中**（本任务的冻结面变更系主人批准；本轮修复**未触碰** `protocol.h`）。

### 9.2 七套件（本轮全量复跑）

| 套件 | 基线 | 本轮 | 日志 |
|---|---|---|---|
| entry_ops | 43 | **43/43** | `entry_ops.log` |
| dedup | 13 | **13/13** | `dedup.log` |
| cross_tree | 41 | **41/41** | `cross_tree.log` |
| sec001 | 48 | **48/48** | `sec001.log` |
| fastupload | 46 | **46/46** | `fastupload.log` |
| groupfile | 45 | **45/45** | `groupfile.log` |
| parallel_e2e | 43 → **46** | **46/46**（含 T-19） | `parallel_e2e.log` |

- 六套既有套件（id=0 帧驱动）全绿 → **I1/I2/I3（旧客户端/旧语义零改动）在本候选上仍成立**，且其中一个（fastupload）在首版修复下曾 24/46，正是本轮的兼容面守卫。
- `parallel_e2e` 关键行：`T-19a/b/c` 全 PASS；`T-03a [True,True,True,False]`（硬上限未被稀释）；`T-13a/b`（60s 回收）PASS；`T-06a order=[212,213,211]`。
- 执行形态遵循 `E2E-RUNFORM.md`（cwd 任意、`py -3.13`、串行独占 5000）。七套件日志与 `119e6d3` 为**同一源码内容**（提交未改动文件字节；提交后重建仍为 build GREEN）。

### 9.3 反向探针

见 §4：关闭 `fastcheckPlaceholder` 排除 → T-19b/T-19c 精确复现 AUD-1；探针代码零残留。

## 10. 证据清单（本目录）

`report.md`（本文件）、七套件日志 `entry_ops.log` `dedup.log` `cross_tree.log` `sec001.log` `fastupload.log` `groupfile.log` `parallel_e2e.log`、三门日志 `build-119e6d3.log` `test-119e6d3.log` `pipeline-audit.log`。
三门脚本原始输出另按仓库约定落 `build/evidence/current/`（gitignored）。

## 11. cleanup_status

- 无遗留 worktree：本轮全程在主线工作树 `E:/qt/xiangmu`（分支 `feat/req-ext-transfer-001-parallel-transfer`）内进行，未新建/未删除任何 worktree。
- 反向探针（`NEG-PROBE`）两次均已回滚并重建，`git diff` 中零残留（已核）。
- 运行期清理：删除了 `dist/filesys/.staging/` 下**一个孤儿 .part**（`c260be8c….part`，属上一轮探针遗留、无在途传输引用）；`dist/` 与数据库均为本地运行数据，不入库。
- 本节点提交：`119e6d3`（代码 + design/harness/test-report + 七套件日志）；随后的收口提交为 docs-only（本报告 + 交接 + 任务记录事件）。

## 12. 交下一节点

**independent audit（新会话、只读、detached worktree @ `119e6d3`）**，输入 = 本报告 + `task.md` 事件链 + 上一轮 audit 报告 + 七套件与三门日志。建议审阅重点：
1. AUD-1 定稿修复是否真正覆盖"97 占位不再吞名额"且**未**破坏旧客户端 97→31 直传（fastupload 46/46 是证据，非断言）；
2. `fastcheckPlaceholder` 转正时机（收到正文）与 `adoptedFastcheck` 释放路径是否存在新的竞态或悬垂引用；
3. AUD-2 的"无在途上传即失败"分支是否可接受为 design §3.3 的实现级补充；
4. T-19 是否为**真实**回归锚点（§4 反向探针可独立重做）。
**AUD-3/AUD-4 与 design §6.3 人工清单仍未闭环**，不得被 integration 三事实行掩盖。
