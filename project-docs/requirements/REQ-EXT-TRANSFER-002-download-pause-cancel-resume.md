# REQ-EXT-TRANSFER-002 下载暂停/取消与断点续传

> 阶段：goal（需求 + 最小方案裁决）｜日期：2026-10-08｜分级：**LARGE**
> 事实源：`Client/transfertask.cpp`、`Server/mytcpsocket.cpp`、`Server/msghandler.cpp`（本文件全部锚点为本次 goal 实测，非文档转述）
> 上游：`requirements/REQ-EXT-TRANSFER-001-multi-file-parallel-transfer.md` §2 非目标「下载任务的暂停/取消（下载仅多选并行）」；`融合式扩展功能路线图.md` P1「下载断点续传」
> 逐字节落位、错误码、测试矩阵在 design 节点冻结（`tasks/REQ-EXT-TRANSFER-002/design.md`）。
> **批准前禁止编辑 `Client/protocol.h` / `Server/protocol.h`**（frozen_paths）。

## 0. 前置事实（本次 goal 核验）

`REQ-EXT-TRANSFER-001`（多文件有限并发，transfer_id 多路复用）**已实现并合入 master**（`d2148bf`，祖先链含于 `d85e39f`）：个人/群 × 上传/下载四路并行、上传暂停恢复取消、下载 round-robin 公平调度、54 条 `parallel_e2e` 断言在册。

本需求**不是**新增并行能力，而是补齐该需求自声明的非目标：**下载侧的控制面**。现状（代码锚点）：

| # | 现状 | 位置 |
|---|---|---|
| G1 | `pause()` 对下载**直接 return**（`!t->isUpload()` 早退）→ 面板「暂停」对下载条目无效 | Client/transfertask.cpp:825 |
| G2 | `cancel()` 对下载只 `closeRecv` + 移除任务，**不发 107** → 服务端仍在推送该文件直至读完，占带宽与并发名额，客户端静默丢弃 | Client/transfertask.cpp:896-910 |
| G3 | `resume()` 对下载以**新 id 重发 40/105**，服务端从 0 重传；客户端 `beginReceiving` 走 (N) 递推**另建新文件**（旧分片成孤儿） | Client/transfertask.cpp:858-869、536-560 |
| G4 | `onDownloadFinish` 失败即 `QFile::remove(savePath)` → 失败重试永远从头 | Client/transfertask.cpp:789-791 |
| G5 | 快照只存下载的 kind/路径/名字/大小，注释明写「下载整文件重传」→ 重启后无法续传 | Client/transfertask.cpp:48-50、64-87 |
| G6 | 服务端 `startDownloadStream(path, id)` 无起点参数，恒定 `sentSize=0` 从头流 | Server/mytcpsocket.cpp:441-466 |
| G7 | 服务端 107 只认上传槽（`findUpload`），下载流从未被 107 释放 | Server/mytcpsocket.cpp:392-415 |
| G8 | 40/105 请求无「起点」字段（40 的 caData[32..47] 空闲；105 的 caData 除 [48..55] 外全空） | Server/msghandler.cpp:1730-1787、msghandler.cpp:2299-2319 |

可复用设施（零新建）：107/108 ABORT 请求-响应对已在线上且回带 transfer_id；下载数据通道 42/43 个人与群共用；出站 256KB 高水位背压与 round-robin 环调度已就位；快照框架与「恢复为 Paused」语义已为上传验证过。

## 1. 需求

下载任务获得与上传对等的控制面：**暂停 / 继续（断点续传，不重传已收部分）/ 取消（立即服务端停发）**，个人下载与群下载一致。

## 2. 范围（用户 2026-10-08 决策）

| 项 | 决策 |
|---|---|
| 传输路径 | 个人下载（40/41/42/43）与群下载（105/106/42/43）**同一套语义** |
| 续传语义 | **服务端从客户端声明的起点开始发送**（真续传：已收前缀不重传）；起点 = 本地分片文件的物理大小 |
| 暂停语义 | 停收 + 发 107 释放服务端槽位 + 保留磁盘分片；「继续」从分片末尾接续 |
| 取消语义 | 发 107 释放槽位 + **删除本地分片** + 移除任务行 |
| 失败重试 | 失败**保留**分片；「重试」从分片末尾续传（不再从头） |
| 重启/断线 | 任务恢复为「已暂停」，分片仍在磁盘则「继续」可续；分片缺失则标失败并从头重下 |
| 非目标 | 单文件分片并行（P5）、动态分片、分片级校验/哈希、限速与配额、下载自动重连续传（需用户点继续，与上传一致） |

## 3. 最小实现方案

### 3.1 协议增量（冻结面，全部追加、零删除、零重编号）

| PDU | 新字段 | 落位 | 语义 |
|---|---|---|---|
| 40 DOWNLOADFILE_REQUEST | `DOWNLOAD_RESUME_OFFSET` | caData[32..39]（u64，空闲） | 客户端声明的起点；**0 = 从头**（旧客户端与既有 e2e 脚本天然写 0） |
| 105 GROUP_FILE_DOWNLOAD_REQUEST | 同上（同一常量同一偏移） | caData[32..39]（u64，空闲） | 同上；caMsg 群信封**零改动** |
| 41 DOWNLOADFILE_RESPOND | `DOWNLOAD_ACCEPTED_OFFSET` | caData[24..31]（u64，空闲） | 服务端**实际采纳**的起点 |
| 106 GROUP_FILE_DOWNLOAD_RESPOND | 同上（同一常量同一偏移） | caData[24..31] | 同上 |

- 回带采纳起点是**兼容护栏**：旧客户端不读该区间 → 零影响；新客户端请求 N>0 而收到 0 → 判定「对端未接受续传」→ **明确失败**，绝不把后缀写进错误的偏移造成文件损坏。
- 冻结面增量：两端 protocol.h 各 +1 个常量块（4 行，逐字节一致），**追加在 REQ-EXT-TRANSFER-001 常量块之后**——不得插入其内部（`parallel_e2e.py` P1-frozen-block 以该块尾行 `MAX_PARALLEL_DOWNLOADS = 3;` 为正则终点，插入即破坏既有断言）。

### 3.2 服务端（2 文件）

| # | 改动 | 位置 |
|---|---|---|
| S1 | 107 泛化：`id≠0` 时先查上传槽（现有行为逐字节不变），否则查 `m_downloads` → `removeDownloadStream(id)`；两者皆无 → `ret=false`（现状）。108 原样回带 id。`id=0` 仍不受理（维持既有断言） | mytcpsocket.cpp:392-415 |
| S2 | 40 与 105 分支读出起点，随 `startDownloadStream` 下传 | mytcpsocket.cpp:213-228、301-315 |
| S3 | `startDownloadStream(path, id, startOffset)`：`seek(startOffset)`、`sentSize=startOffset`；`startOffset > totalSize` → 不建流，41 回 `ret=false` / 106 回续传起点非法码 | mytcpsocket.cpp:441-466 |
| S4 | 41/106 响应写入 `DOWNLOAD_ACCEPTED_OFFSET` = 实际采纳起点 | msghandler.cpp:1777-1786、2212-2229 |

服务端不新增调度、不新增定时器、不改并发上限语义：107 释放后 `m_downloads.size()` 自然回落，槽位即时可复用。

### 3.3 客户端（1 文件）

| # | 改动 | 位置 |
|---|---|---|
| C1 | `pause()` 增下载分支：Running → 发 107 → `closeRecv`（**保留磁盘分片**）→ 归还 `m_activeDownloads` → **摘除旧 id 映射** → Paused；Queued → 移出队列 → Paused | transfertask.cpp:822-851 |
| C2 | `resume()`/`retry()` 下载分支：**摘除旧 id 映射**；起点 = 磁盘分片实际大小（无分片则 0） | transfertask.cpp:854-890、937-954 |
| C3 | 40/105 组包写入起点 `t.doneBytes` | transfertask.cpp:476-501 |
| C4 | 41/106 处理：校验采纳起点 == 声明起点（否则失败）；续传时校验服务端大小与暂停前一致（不一致 → 「服务端文件已变化」失败）；续传走 `resumeReceiving`（重开**同一** `savePath` 并 `seek`），非续传走既有 `beginReceiving`（(N) 递推不变） | transfertask.cpp:701-765、536-571 |
| C5 | `onDownloadFinish` 失败分支：**不再删除**分片（续传前提）；清理由「取消」承担 | transfertask.cpp:781-793 |
| C6 | `cancel()` 下载分支：Running 先发 107；删除分片；摘 id 映射 | transfertask.cpp:896-910 |
| C7 | 快照新增下载的 `savePath`；恢复时按分片大小回填起点，分片缺失则标失败（起点归零可重下）；文件头注释同步（「下载整文件重传」已过期） | transfertask.cpp:48-50、64-130 |
| C8 | 面板提示文案更新（暂停/继续/取消现对下载同样生效） | transfertask.cpp:978、1026 |

**必做的一条防御**：换 id 时摘除旧 id 映射（C1/C2/C6）。否则暂停瞬间仍在途的旧 id 的 42 数据块会命中映射、被写进续传后的文件，造成**静默串写**（G3 的既有隐患，本需求把它变成必发路径）。

### 3.4 失败不再删分片的代价（如实记录）

失败任务会留下与目标同名的分片文件，直到用户「取消」或重试成功。缓解：快照恢复时校验分片存在性；面板状态列可见「已暂停/失败」。不做后台清理（不新增调度器）。

## 4. 验收标准（业务语言）

1. 下载中点「暂停」：任务停住、占用的下载名额释放、已下字节留在磁盘；点「继续」从断点接续，**已收到的前缀不再重传**。
2. 续传完成后落盘文件与源文件**逐字节一致**。
3. 点「取消」：服务端**立即**停发该文件，同连接其他下载/上传不受影响；本地分片删除、任务行移除。
4. 暂停/取消不干扰其他并行任务（其余下载照常完成，round-robin 公平性不变）。
5. 暂停期间服务端文件发生变化（大小不同）→ 明确失败，**不产生损坏文件**。
6. 续传起点非法（超过文件大小）→ 明确失败，连接与其余任务不受影响。
7. 断线/重启后任务为「已暂停」，分片仍在磁盘则「继续」可续传。
8. **既有单文件与多文件并行流程、全部既有自动化回归逐字节不变**（id=0 旧路径与既有 54 条 `parallel_e2e` 断言零改动）。

## 5. 节点计划（LARGE）

goal（本文件 + task.md，本会话）→ design（逐字节落位 + 错误码 + 测试矩阵 → 契约变更批准卡）→ development → independent test（新会话）→ repair（按需）→ integration（三事实行）→ independent audit（新会话、只读）。

## 6. 开放问题（design 节点裁定）

1. 续传起点非法的错误码取值与文案（群侧 `-2107` 已被并发上限占用，建议 `-2108`）。
2. 105 用 caData[32..39] 的最终确认（需复核群信封「尾随字节即拒」不受影响——本方案不动 caMsg，预期无影响）。
3. 暂停后是否等待 108 才允许继续（TCP 顺序已保证 107 先于新 40 到达，倾向不等待）。
4. 106 的响应契约原文写「caData 全空」，本需求在 [24..31] 写入采纳起点，需在 design 明写该区间的归属与新契约措辞。

## 7. 风险

| 风险 | 缓解 |
|---|---|
| 陈旧 42 串写续传文件 | 换 id 即摘映射（C1/C2/C6）+ 专项断言（暂停瞬间持续发块的场景） |
| 新客户端 × 旧服务端静默错位 | 41/106 回带采纳起点，不匹配即失败（3.1） |
| 冻结面双端漂移 | 新块追加在既有块之后、双端逐字节一致、design 出逐字批准稿、审计核对 diff |
| 失败留分片成垃圾 | 「取消」删除；恢复时校验；文档与面板如实呈现 |
| 续传把「服务端已变」的文件拼成损坏件 | 大小不一致即失败（C4） |
| 106 响应契约变更被漏记 | design 明写 [24..31] 归属 + 专项静态断言 |
