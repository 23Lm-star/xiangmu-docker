# REQ-EXT-TRANSFER-003 上传会话连续性、进度可见与吞吐 — 任务记录

## 档位
- **LARGE**（goal 定档 2026-10-08）。理由（风险面，非行数）：
  1. **冻结面命中**：吞吐目标需要上调 `Server/protocol.h` 的 `MAX_INBOUND_PDU_SIZE` /
     `MAX_PENDING_INBOUND_BYTES`（manifest `frozen_paths`）→ 按 v1.7 规则升 LARGE，改动须主人批准。
  2. **公共契约变更**：上传数据通道（31/32）的单块正文上限由 16308 B 放宽；只放宽不改语义、
     不增删字段与编号，旧客户端字节行为不变。
  3. **跨会话**：服务端（会话行复用 + 进度落库 + 暂存句柄生命周期）与客户端
     （分片策略 + id 映射防御）分属两个独立面，且改的是上传热路径。
  4. **回归面**：`parallel_e2e`(54) / `fastupload`(46) / `groupfile`(45) 直接覆盖上传路径；
     `upload_session` 表语义被 reclaim / sec001 侧读用。
- 需求文档 `project-docs/requirements/REQ-EXT-TRANSFER-003-upload-session-continuity-and-throughput.md`
  自声明 LARGE；本记录与之一致，不再改档。
- 流程版本：Development OS v1.7。分支：`feat/req-ext-transfer-003-upload-session-throughput`，
  基线 `master@d85e39f`（工作树 `build/wt-claude`，gitignored）。
- 节点计划：goal（本记录）→ design（常量终值 + 节流/复用规则 + 测试矩阵 → `design.md`）→
  **主人批准契约变更** → development → independent test（新会话）→ repair（按需）→
  independent audit（新会话、只读）→ integration（三事实行）→ merge no-ff。
- `owner_agent: claude-code`。

## 依赖核查（2026-10-08 实测，非文档转述）
| 依赖 | 状态 |
|---|---|
| REQ-EXT-TRANSFER-001 | ✅ merged（祖先链含于 `d85e39f`）：`TransferManager` + per-transfer 上传槽 + 107/108 + 上传暂停/续传（S3）在线 |
| REQ-EXT-FILE-001 T4 / REQ-EXT-FILE-002 | ✅ merged：`upload_session` 表、确定性暂存键、`restartUploadSession`、97/98 预检查在线 |
| REQ-EXT-TRANSFER-002 | ⏳ 在飞（`feat/req-ext-transfer-002-download-pause-resume@d45ae11`，测试节点已完成、待 audit/merge）。**并行冲突面**：`protocol.h`（其在文件尾追加常量块；本需求改中部常量行——不同 hunk），`msghandler.cpp`/`mytcpsocket.cpp`（下载面 vs 上传面，函数不重叠） |
| 回归运行形态 | 已统一（`E2E-RUNFORM.md`，`_harness/server_ctl.py` 管生命周期）；master 侧 12 套、TRANSFER-002 合并后 13 套 |
| 探针二进制 | `dist/Server.exe` sha256 前缀 `cb87a0fa03a56e98`（15:55 部署件，主人 GUI 测试所用）；三条缺陷的根因路径在 master 上逐一核对，`uploader.cpp`/`uploadwritetask.cpp`/`operatedb.cpp` 未被 TRANSFER-002 分支改动 |

## 用户输入（2026-10-08）
主人 GUI 一轮实测上报（原话）：① 上传中 `upload_session` 偏移量一直是 0；② 暂停后断点续传时
创建了一个新的上传会话；③ 上传太慢（4 KB），希望"按固定块的数目生成、每个分片更大"；
并要求"把上传和下载的完整业务实现逻辑全检查一遍"。**要求先做需求分析**（本节点即此）。

## 契约影响（须主人批准；批准卡随 design.md 提交）
| 面 | 变更 | 性质 |
|---|---|---|
| `Server/protocol.h`（frozen） | `MAX_INBOUND_PDU_SIZE` 16 KB → 终值（design 定，目标支持 64 KiB–1 MiB 级分片）；`MAX_PENDING_INBOUND_BYTES` 同步上调（必须 ≥ 单 PDU 上限 + 余量，否则大 PDU 被"缓存超限"误断开） | 只放宽上限；不改字段/编号/既有语义 |
| `Client/protocol.h`（frozen） | 预计**零改动**（客户端分片策略常量放 `Client/uploader.h`，属客户端本地策略）；若 design 判定需两端同值再单列 | 待 design 裁定 |
| 31/32 语义 | 单块正文上限放宽；offset 连续性/长度边界/resumeMagic 校验语义不变 | 兼容 |
| `upload_session` 表 | **零 schema 变更**；`confirmed_offset` 写入时机与取值口径变化（新语义：暂存物理大小，节流持久化） | 语义澄清 |
| 老客户端 / id=0 旧路径 | 字节行为不变（上限是"允许更大"） | 不变式 |

## 门次与事件（append-only）
- 2026-10-08 goal：分支自 `master@d85e39f` 切出（工作树 `build/wt-claude`）。需求文档 + 取证页落盘。
  取证：探针三支（`probe_upload_review` / `probe_upload_cost` / `probe_file_pattern`）实测确认三条上报缺陷，
  并全链路复核上传/下载实现，产出 G1–G8 / D1–D3（G7 断线任务态、G8 暂存键跨群共撞为**范围外另立**建议）。
  关键归因：慢的主因是服务端**每块一次 open/close**（实测 30.2 ms/块 vs 单句柄 0.35 ms/块），
  块大小只是线性杠杆且被其封顶 → 修复必须"去每块 open/close + 放大分片"两件一起做。
  定档 LARGE（冻结面命中）。契约变更批准卡待 design 节点提交。
  节点提交：`432dcff`（docs/probe-only，探针入库；按 `.gitignore` 合同 requirements/tasks/handoffs 与 evidence 日志/报告为本地件，不入库）。
  同日质询更正：G1 由「记录失真/可观测性」升格为 **FILE-001 T4 设计承诺未实现**——T4 原文要求
  `uploadfileinit/UPLOADFILE_DATA/完成` 三阶段落库（`tasks/REQ-EXT-FILE-001/task.md:33`），
  实际 DATA 阶段从未落库（`updateUploadOffset` 自 `6e023d5` 起无调用者，`35dfce7` 才首次被 abort 路径调用），
  完成路径亦未按 FILE-002 design §2.4 写 `confirmed_offset=size`（仅秒传命中路径实现）。
  影响面：该列无产品逻辑读取（`UploadSessionLookup.confirmedOffset` 无消费者），故属记录/承诺失真而非功能缺陷；
  S-A 相应改述为「补齐 T4 的 DATA 阶段（带节流）」。
- 2026-10-08 第二轮质询更正：G3 归因由「每块 open/close」精确化为「**每块重开写句柄**」——
  三段分离（raw Win32）+ Qt 原生复核：成本全在 `CreateFileW`（12.3–13.5 ms/块），`CloseHandle` 仅 0.3 ms，
  未被写过的文件 open+close 仅 0.5 ms，卷顺序写 112–409 MiB/s；**与线程无关**（`QThreadPool::globalInstance()`
  worker 常驻，三支探针零线程亦复现）→ 不是句柄创建/销毁开销、不是写线程临时开。单句柄形态 0.1–0.2 ms/块（~64–128×），
  且保留每块 `flush()` 不损失收益 → S-C 不需改 flush 语义。提交 `e49e8aa`（probe_open_cost.py 入库）。

- 2026-10-08 design（同会话连续执行，主人"先做B吧，然后继续"）：`design.md` 落盘（D1–D10 裁决 / F1–F7 冻结 /
  S1–S15 服务端 / C1–C4 客户端 / §6 测试矩阵 3 静态 + 12 动态 + 5 人工 + 回归 / §9 逐字批准稿 / §10 批准记录）。
  **主人裁决 S-C 机制 = 方案 B**（保留全局线程池 + 暂存句柄共享所有权 `QSharedPointer<QFile>`；写任务只 seek+write+flush，
  open/close 移到会话生命周期；A=每上传一线程 作慢盘退路、C=主线程内联写 均未采纳）。
  契约变更收敛为最小面：`MAX_INBOUND_PDU_SIZE` **保持 16 KB 不动**，新增 `MAX_UPLOAD_DATA_CHUNK=1 MiB` 与
  `MAX_UPLOAD_DATA_PDU_SIZE`（**仅 31 生效**），`MAX_PENDING_INBOUND_BYTES` 256 KB → 1 MiB+8 KB（≥ 单包上限）。
  `Client/protocol.h` **零改动**（客户端分片常量落 `Client/uploader.h`，非冻结面）。
  新增设计项：D5 超长块显式拒绝（不走非法 PDU 断连路径）、D9 空/已完成文件直接 Done（0 字节文件 GUI 必失败的既有缺陷）、
  C4 上传侧摘旧 id 映射。**批准卡已提交主人，design_status=DRAFT。** design 节点为 goal 的退出条件（handoff 词表无 design 档），
  故本文件即为含 design 产出的 goal 交接。
- 2026-10-08 design 批准：主人对 §9 契约变更批准卡回复「行，没问题了」→ **批准**，`design_status=CONFIRMED`。
  批准范围：仅 `Server/protocol.h` 常量面（`MAX_UPLOAD_DATA_CHUNK/MAX_UPLOAD_DATA_PDU_SIZE` 仅对 31 生效、
  `MAX_PENDING_INBOUND_BYTES` 1 MiB+8 KB、`MAX_INBOUND_PDU_SIZE` 保持 16 KB）+ `mytcpsocket.cpp` 按类型取上限；
  `Client/protocol.h` 零改动。批准稿外改动一律不允许（审计按 §9 逐字比对）。
- 2026-10-08 范围增补（主人指令「G7本次一次性修复了，G8不用管」）：**G7 断线任务态收敛纳入本需求**（S-F / design §4.4 C5–C9），
  **G8 明确不处理（风险接受）**。G7 为纯客户端改动、不涉冻结面 → **不触发新批准**，`design_status` 保持 CONFIRMED（本次为 CONFIRMED 后的设计增补，已记入 design §10）。
  G7 口径：两处触发（`disconnected` 信号 + 90 s 心跳丢失）→ 在途（Running/Hashing）任务摘旧 id 映射后标记 Failed+原因、名额归还、清 97 在途槽；
  Queued/Paused 不动；`pump()` 受 `m_connectionDown` 闸；登录成功调 `onConnectionRestored()`（不依赖 `setCurrentUser` 的提前返回）；
  重试即续传（上传按服务端暂存、下载按分片，零新代码）；SHA 回调与 `submitFastcheck` 补状态守卫防"失败后复活"。
  验收新增第 9 条；测试面 = 2 条源码静态契约（S-04/S-05）+ 1 条 GUI 人工项（客户端无 headless 宿主，沿用既有口径，不计 pass）。
## 下一节点
**development 节点（新会话，owner=claude-code）**，在 dev worktree `build/wt-claude` 执行：
T1 按 §9 逐字改 `Server/protocol.h` + 入站校验按类型取上限（唯一冻结面改动）；T2 服务端 S1–S7（共享句柄 + 四类释放时机 + 超长块拒绝）；
T3 服务端 S8–S15（节流落库 + 会话行复用）；T4 客户端 C1–C4（分片策略/空文件 Done/摘旧映射）+ C5–C9（G7 断线收敛）；
T5 新套件 `verify.py`（§6.1–6.2）+ `project-manifest.yaml` 注册；T6 回归 + 文档同步（interfaces.md 常量表等）。
工作树准备（全新工作树，gitignored 物需自带）：① 从主工作树复制 `pipeline/`（未入库、按工作树各一份）；
② 构建产出落 `dist/`（`DESTDIR=dist`），`dist` 的 Qt 运行库按 `E2E-RUNFORM.md` 备齐（DLL trio + platforms/sqldrivers）；
③ `dist/filesys` 需 priming（共享 mydb 去重）否则下载类断言会假红。
