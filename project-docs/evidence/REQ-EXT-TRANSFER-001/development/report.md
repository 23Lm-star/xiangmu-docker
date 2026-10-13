# REQ-EXT-TRANSFER-001 development 节点报告

- 节点：development（Development OS v1.7 / LARGE），分支 `feat/req-ext-transfer-001-parallel-transfer`，候选 `520c714`。
- 上游：`tasks/REQ-EXT-TRANSFER-001/design.md`（§9 逐字批准稿、§11 批准记录：主人 2026-10-01 同意）。
- `owner_agent: claude-code`。

## 1. 交付物（T1–T8）

| # | 任务 | 结果 |
|---|---|---|
| T1 | 两端 `protocol.h` 按 §9 逐字写入 | 各 +11 行，零删除；`git diff --stat master...HEAD` 与批准稿一致 |
| T2 | 服务端 per-transfer 上传上下文 | `MsgHandler::UploadCtx` 表；29/101 建槽（D7 上限 + T-17 重复 id）；31 逐 transfer 校验；107/108；D5 60s 回收；断线清理 |
| T3 | 服务端下载 ring 调度 | 单环 + 单定时器 round-robin；每 tick 一块；与 256KB 高水位联动；控制消息不被饿死 |
| T4 | 客户端 TransferManager + 路由 | 30/32/41/42/43/98/102/106/108 全部按 id 路由；删除 `File::m_uploader`/`GroupFile::m_uploader`/`GroupFile::s_uploading` |
| T5 | 客户端 UI | 多选上传/下载 + `TransferTaskPanel`（文件名/方向/进度/状态/失败原因 + 暂停/继续/取消/重试） |
| T6 | SHA-256 移出 UI 线程（M5） | `QtConcurrent::run` 后台计算；算法与 1MiB 分段不变 |
| T7 | 新套件 `parallel_e2e.py` | 43 用例，全绿 |
| T8 | 六套件回归 + 文档同步 | 见下 |

## 2. 门与回归

| 门 | 结果 |
|---|---|
| `pipeline/build.ps1` | **GREEN**（520c714） |
| `pipeline/test.ps1` | GREEN-WARN：0 注册单测（仓库口径，非伪造） |
| `pipeline/audit.ps1` | **RED = 预期冻结面命中**（本任务即冻结面变更）；`impact-check: NOT_APPLICABLE`（manifest 声明） |
| 六套既有 e2e | entry_ops 43/43、dedup 13/13、cross_tree 41/41、sec001 48/48、fastupload 46/46、groupfile 45/45 |
| 新套件 | `parallel_e2e.py` **43/43** |

- 冻结面自检（integration 事实线 #2 的前置）：`git diff master...HEAD -- Client/protocol.h Server/protocol.h`
  = 22 insertions / 0 deletions，与 §9 批准稿逐字一致，**无批准外改动**。
- 日志：本目录 `*.log`（六套件 + 新套件）。

## 3. 实现期裁定（需 independent test / audit 复核）

1. **个人上传握手 = 97 → 29 → 31/32**（design §5.6 与 §3.2/§5.3 的调和）：
   `fastupload` 套件的 P4/P5 为**静态源码断言**，要求 `Client/file.cpp` 保留 1MiB 流式 SHA-256 且**只**发
   FASTCHECK、不得出现 `UPLOADFILE_INIT_REQUEST`。而 97/98 按 P2 **不带 transfer_id**，无法按 id 建上下文。
   故：`file.cpp` 仍算 SHA 并发 97（单飞，服务端旧槽单份）；未命中后由 `TransferManager`（`transfertask.cpp`）
   发 29 建槽，服务端以 `m_pendingFastcheck` **采纳** 97 建立的暂存会话（不重复建会话、保留摘要对照）。
2. **T-06 流数 = 1 大 + 2 小（3 条）**：design 写 "8MB×1 + 64KB×3"（4 条下载流）与主人裁定的 D6
   （下载上限 3）冲突，4 条中第 4 条必被 40 拒绝。本套件按 3 条断言同一公平性性质（小文件 43 早于大文件 43）。
3. 97 命中路径（秒传）在并行下仍零正文完成；未命中才建槽。

## 4. 已知缺口（design §10 已列）

- 下载不可 abort（R1，非目标）；下载任务无暂停/取消（非目标）。
- 任务不持久化（R3，进程退出即丢）。

## 5. 交下一节点

independent test（新会话、独立干净工作树、候选 `520c714`）：独立复跑 `pipeline/*` 与七套 e2e；
纯客户端行为按 design §6.3 人工清单（多选/任务列表/暂停继续取消/大文件摘要不卡 UI）。
