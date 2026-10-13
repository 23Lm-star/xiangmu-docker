# 附：人工面（GUI 5 项）结果的技术鉴定 — 本轮跑的不是候选构建

日期：2026-10-08 晚（主人 GUI 一轮后）。鉴定人：claude-code（本独立测试节点）。
状态：**人工面结论对候选 b861351 无效**——被测的是一个**早于本需求（pre-003）的部署对**。需要换对构建重跑（§4）。
不改任何产品源码；本附件的全部结论来自二进制标记、共享 DB 与磁盘事实（可复核命令附于每条）。

## 1. 被测构建身份（FACT）

| 对象 | 实测 | 003 特征标记 | 判定 |
|---|---|---|---|
| `E:/qt/xiangmu/dist/Client.exe` | 3,924,992 B，mtime 10-08 **15:58:48**，sha `3f934d39…` | G7 串（UTF-16）**0 次** | pre-003 |
| `E:/qt/xiangmu/dist/Server.exe` | 506,368 B，mtime 10-08 **15:55:24**，sha `cb87a0fa…` | D5 串（`超长上传块拒绝`）**0 次** | pre-003 |
| 003 参照 `build/wt-claude/dist/*` | Client 3,924,480 B / Server 509,440 B | Client **2 次** / Server **1 次** | 候选 |

标记法自证：同一检查在 003 双端上分别命中 2 次 / 1 次（客户端的 G7 文案经 `QStringLiteral` 以 UTF-16 存储，须按 UTF-16 检索；服务端 qCWarning 为 UTF-8）。
`git show master:` 侧基线复核：`findReusableUploadSession`=0、`m_connectionDown`=0、`mytcpsocket.cpp` 内 `updateUploadOffset`=**0**、`UPLOAD_CHUNK_MAX`=0 → 主人观察到的四条现象与"pre-003 引擎"完全对应（§2）。

主人的上传数据落在 `E:/qt/xiangmu/dist/filesys`（DB `staging_path` 全为该绝对前缀）→ 服务端 cwd=主 dist ✓ 与上表同一套 exe。

## 2. 逐条对照（主人观察 → pre-003 代码事实 → 003 的对应修复与既有断言）

| 主人观察 | pre-003 事实（证据） | 003 对应实现 | 003 侧独立证据 |
|---|---|---|---|
| ① 上传提速明显 | 该对为 4 KB 分片的旧客户端 → 无法证明是候选带来的提速 | C1 分片策略（`Client/uploader.h` 三常量 + `Uploader::start` 夹取） | S-02a/b（跨端上限一致）；T3-06 33–36 MiB/s ≥ 8 |
| ② 偏移量不随进度动 | `handleUploadWriteFinished` **从不**写 `confirmed_offset`（master mytcpsocket.cpp 零命中） | D6/S9 节流落库（≥1 MiB 或 ≥2 s）+ S10 完成/失败强制写 | T3-01a/b（单调、末次 ≥ 已发−1 MiB）、T3-01c（完成 == expected） |
| ③ 每次加一行 | 无复用键：`findReusableUploadSession` 在 master 不存在 → 每次 97/29 都新插一行。DB 实证：同一暂存键 `90f34db6…`（三角洲…rar，1.16 GB）有 **3 行**（3953 expired / 3954 uploading(reqId=NULL，29 路径) / 3957 uploading），`e8963de9…`（NewMyRPG.rar）有 **2 行**（3955 uploading / 3956 done） | D7/S12–S15 按 (owner, staging_path) 复用 + 97 改键 | T3-02/03/04 单行贯穿（3 组）、X-5b 跨连接各单行 |
| ④ 空文件没有问题 | 空内容走 sha 去重，与版本无关 | — | T3-07a（0 字节转正）、C3/D9（客户端直接完成） |
| ⑤ 断线后回登录界面、会话/任务找不到、无法续传；重传能复用暂存 | 客户端无 G7（master `m_connectionDown`=0）：断线不收敛任务、重连不恢复调度 | G7/C7/C8 + `onConnectionRestored`（静态契约 S-04a–d） | 仅静态面；GUI 行为仍待人工（本轮作废） |

**"重传能复用原暂存文件"是正向信号**：说明暂存键（owner|parent|display|size → sha1）语义在 pre-003 与 003 一致，候选的续传前提成立（T3-09 已独立复证）。

## 3. object_id 疑问（FACT，非缺陷）

- **小 id = 内容去重命中**：主人上传的"新建文本文档.txt"为 0 字节 → sha256(`""`)=`e3b0c442…`，该对象早在 2026-10-03 就存在（`object_id=781`，refcount **25**）。文件本体在 `dist/filesys/objects/e3/781`（0 B，**存在**，已复核）。
- **大 id = 自增序列的当前位置**：`file_object.object_id` 是自增列，共享 `mydb` 累积了数周 e2e 上传 → 当前位约 100 万。主人 173 MB 文件 `NewMyRPG.rar` 新对象 = `1000421`（正常）。**不是"异常大"**，也**不会**因为后来传了空文件而回退（去重命中不插新行）。
- 复核命令：`SELECT object_id,sha256,size_bytes,storage_path,refcount FROM file_object WHERE object_id IN (781,1000421);`
  → 781: sha `e3b0c442…`, 0 B, `objects/e3/781`;  1000421: 181,732,483 B, `objects/09/1000421`。

## 4. "转正对象的文件在存储路径找不到"（三分）

1. **1.16 GB 那个（"大文件"）从未转正**：`upload_session` 只有 uploading/expired，`user_file`/`file_object` 里都没有它；暂存 `dist/filesys/.staging/90f34db6….part` 物理 **810,033,152 B**（= 65% of 1.16 GB，mtime 22:16）→ 没转正自然没有对象文件。
2. **173 MB 那个已完整落盘**：`objects/09/1000421` 存在（181,732,483 B），且磁盘内容 sha256 前缀与 DB 记录**逐字节一致**（已复核）→ 这个文件没有丢。
3. **共享 DB + 多棵工作树是混淆源**：本轮独立测试在我自己的工作树里跑过 13 套 e2e，那些对象行写进同一个 `mydb`，但**实体文件在各自工作树的 `dist/filesys/`**（例：`object_id=1000420` 属 e2etx3/negctl_alias.bin，文件在 `build/wt-indtest/dist/filesys/objects/98/1000420`，主树查不到是正常的）。**这是我造成的观测噪声，如实披露**；如需我清理这些测试行/对象，请指示（删除操作等你点头）。

## 5. 遗留物（主 dist，未清理）

| 路径 | 大小 | 说明 |
|---|---|---|
| `dist/filesys/.staging/90f34db6….part` | 810 MB | 主人 1.16 GB 上传的断点暂存（保留可续传；两侧 session 行 3954/3957 仍在） |
| `dist/filesys/.staging/5d7c572e….part` | 12 MB | 更早（16:41）的暂存残留 |
| `dist/filesys/.staging/_probe_open.tmp` | 1 MB | 成本探针遗留（goal 节点） |
| `upload_session` 3954/3957（uploading）、3953（expired） | — | pre-003 的"一行一尝试"残留；003 上线后可手工清理或等 TTL/回收 |

## 6. 结论与下一步

- 本轮 GUI 5 项 **①③⑤ 的观测全部由 pre-003 构建产生**（②④ 亦同批），**不构成对候选的验证或反证**；候选侧 ①②③ 已由机器面独立复证（§2 末列），⑤ 仍需人眼。
- **下一步（唯一阻塞项）**：用 **候选 b861351 的双端 + 主 dist 运行树**重跑 GUI 5 项。运行树必须是主 dist（主人的既有文件与 DB 里的 `objects/*` 都指向它）；换树会导致"已上传文件全部 404"的新一轮错觉。
- 重跑时的观测口径：②看 `upload_session.confirmed_offset`（记得每次手动重跑该查询，工具不自动刷新）；③只数**同一 display_name + staging_path 的未完成行**（done 行可能是秒传登记，属设计）；⑤杀服务端后任务应在秒级变"失败"且重启+重连后仍在列表里，点重试从断点续传。
