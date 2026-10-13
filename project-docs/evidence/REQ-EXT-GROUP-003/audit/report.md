# REQ-EXT-GROUP-003 独立审计报告（independent audit）

- 节点：independent audit（Development OS v1.7 / LARGE，新会话、只读）。
- 审计对象（test 候选）：`0b83aab`（`feat(group-003): 群文件目录与访问控制（PDU 99-106 + group_file + 服务端权限判定）`），完整哈希 `0b83aabe6bff45dc224dce88eb9ef87690d81ac8`。
- 审计快照：`git worktree add --detach build/wt-audit-group003 0b83aab`；本节点**零提交、零生产源码改动**（工作树只读使用；唯一写盘为 gitignored 的 `build/audit/`）。
- 审计者：**claude-code**。任务记录 `owner_agent: claude-code`，development/test 亦为 claude-code。**跨代理未满足**（同一代理名，仅"独立新会话"成立）——按纪律显式记录，不掩盖；若主人要求跨 CLI 审计，请在 integration 前裁决。
- 审计边界：只读复核 差异 / 三门报告 / 独立测试证据 / 影响面；未改生产源码、测试标准、验收条件、分支历史；未推送、未合并、未启动服务端、未写 DB。
- 结论：**PASS** —— 候选范围内 **0 条阻塞缺陷**；test 节点"PASS，0 条候选归因缺陷"的结论**未被推翻**，未发现越界或伪造证据。非阻塞观察 6 条见下（供 owner/architect 择机处理，**不阻断 integration**）。

## 输入证据

- test 交接：`project-docs/handoffs/req-ext-group-003-independent-test-2026-10-01.txt`（`node: test`、`audit: none`、`blockers: none`）。
- test 报告与日志：`project-docs/evidence/REQ-EXT-GROUP-003/independent-test/`（`report.md`、`groupfile_e2e-run.log`、既有回归 5 件 + 群生命周期、`pipeline-{build,test,audit}.log`、`INDEX.md`）。
- development 证据：`project-docs/evidence/REQ-EXT-GROUP-003/groupfile_e2e.py`（45 用例 harness）。
- 任务与设计：`project-docs/tasks/REQ-EXT-GROUP-003/task.md`（档位/依赖/契约卡/门次）、`design.md`（§1 协议、§2 权限判定点、§3 数据模型、§4 负向面、§6 验收映射、§8 允许路径）。

## 逐项核验

### 1. 冻结路径：两份 `protocol.h` 仅追加 99–106 — FACT，独立复证

- `git diff master...0b83aab -- Client/protocol.h Server/protocol.h | grep -E '^-[^-]'` = **空**（零删除行，append-only）。
- 独立提取两端"群文件 PDU 布局常量"块并逐字节/哈希比对：`len=1870`，两侧 **sha256 前 16 位同为 `86007a7f2971bd34…`，`c==s` 为 True**。
- 新增枚举恒为 99–106 四对；`ENUM_MSG_TYPE_MAX=99999` 与原 1–98 未重编号（diff 只含 `+` 行，插入点位于原 98 之后）。
- 与 design §1.1/§1.2 偏移表逐字段相符（LIST offset@20/limit@24、LIST_RESP count@24/total@28/records@32、INIT_RESP hit@24/file_id@28/gfid@36/session@44/next@52/name_len@60/name@64、DELETE id@20、DOWNLOAD_RESP size@24/name_len@32/name@36）。
- 原始输出：`independent-verification.log` 第 1 节。

**判定：符合已批准的契约变更范围（只追加、两端一致、无重编号）。** 冻结面命中是**预期**，不是缺陷。

### 2. 差异范围与路径边界 — FACT，符合 design §8 允许清单

`git diff --stat master...0b83aab` = 23 文件 / +1940 −26（其中 26 处删除全部在 `Client/msghandler.cpp` 的下载落盘函数**重构搬移**，无删除生产语义）。生产侧改动全部落在 design §8 允许面：

`Client/protocol.h`(frozen,批准)、`Client/client.cpp`、`Client/group.{h,cpp}`、新增 `Client/groupfile.{h,cpp}`、`Client/msghandler.{h,cpp}`、`Client/Client.pro`、`Server/protocol.h`(frozen,批准)、`Server/msghandler.{h,cpp}`、`Server/mytcpsocket.cpp`、`Server/operatedb.{h,cpp}`、`Server/fileobjectstore.{h,cpp}`、`Server/migrate_ext_group_003.sql`(+`_rollback.sql`)、`project-docs/tasks|evidence/REQ-EXT-GROUP-003/`、`project-docs/handoffs/`。

未触碰禁项：既有 PDU 编号/字段/语义、既有表列定义、`Server/filesys/`、外部依赖、`Makefile`/`.pro.user`（audit.ps1 卫生检查静默）。`Server/Server.pro` 无新服务端源文件，符合"仅新增客户端文件"。

### 3. 三门与证据对账 — FACT

| 门 | 证据 | 结果 |
|---|---|---|
| build（test 工作树独立构建 `0b83aab`） | `pipeline-build.log`：`build GREEN: hash=0b83aab config=20261001-140112` | GREEN |
| test（`pipeline/test.ps1`） | `pipeline-test.log`：`test GREEN-WARN: 0 automated tests registered` | honest-0（0 注册测试，未冒充 GREEN 依据） |
| audit（`pipeline/audit.ps1`） | `pipeline-audit.log`：`FROZEN-PATH HIT: Client/protocol.h`、`Server/protocol.h`；`impact-check: NOT_APPLICABLE` | RED = **预期冻结面命中**（本任务即冻结面变更） |
| 影响面（manifest 声明） | `project-manifest.yaml`：`impact_status: not_applicable`（原因：0 静态可达自动化测试，回归由 e2e socket harness 承担） | NOT_APPLICABLE（已声明，非静默跳过） |

**独立复跑 `impact-check.py --from-diff branch.diff --path E:/qt/xiangmu` → `verdict: NOT_CHECKED`（工具自述"NOT_CHECKED is not a pass"）**，与 manifest 声明一致：仓库无静态可达测试，且变更符号不在 master 索引中（新符号仅存在于本分支）。影响面因此**不构成 PASS 依据，也不构成 FAIL**——诚实口径。调用路径改由源码直读确立（见 §7）。

### 4. 独立测试结论核验（未越界、未伪造）— FACT

- `groupfile_e2e-run.log` 尾 = **45/45 passed**；与 dev 交接声称一致。
- 既有回归日志逐一存在且与声称一致：`sec001_regression.log` **48/48**、`entry_ops` **43/43**、`cross_tree` **37/37**、`dedup` **13/13**、`fastupload` **46/46**、群生命周期 **15/15**。
- harness **非恒真**：`groupfile_e2e.py` 含 DB 级不变式断言（`A2b-refcount`：`refcount == COUNT(user_file active) + COUNT(group_file active)`）、负向面（伪造 id → -2103；跨群 group_id → -2103；`/` 名与 300B 名与非法摘要 → -2002；尾随字节 → -2002；limit 越界 → -2002）、未认证分派（→ -1001 且响应类型正确）、以及静态追加性检查。**未发现放宽验收阈值**。
- 越界检查：test 未改任何验收标准；GUI 冒烟明确声明"未执行"；`test.ps1` honest-0 未当 GREEN 用。与 v1.7 纪律一致。
- 候选之上仅文档提交：`git diff --name-only 0b83aab..9129900 -- Client Server pipeline tools tests src include` = **0**（15 个文件全部在 `project-docs/`）→ test 证据锚定 `0b83aab` 成立，无"docs 追哈希"冗余。

### 5. 数据模型 / 迁移 — FACT

- `migrate_ext_group_003.sql`：`CREATE TABLE IF NOT EXISTS group_file`（附加式，不动既有表）；唯一键 `uk_group_file_name(group_id, display_name)`（8 + 70×3 = 218B < 767B 上限）✓；外键 `fk_gf_obj → file_object(object_id)`；索引 `(group_id,status)`、`(group_id,uploader_name)`。
- 版本号：`INSERT IGNORE INTO schema_version VALUES (2026100103)`，`kGroupFileSchemaVersion = 2026100103`（独立于 2026100101/2026100102，避免"未跑迁移的库通过启动检查"）并入 `checkSchemaVersion()` 的 `qMax` required 计算；低版本启动 fail-fast，文案补 `migrate_ext_group_003.sql`。**独立读源码确认**（`operatedb.h:28`、`operatedb.cpp` checkSchemaVersion 改动）。
- 回滚脚本只删本需求对象。

### 6. 代码级核验（读源码，只读）— FACT + CLAIM

- **权限判定 P0–P7**（`OperateDB::handleGroupFileOperation`）：P0 actor 空 → -1001；P1 `group_info` 无行 → -2102；P2 `groupRole()`（`SELECT role … WHERE status='active'`）空 → -2101（**每请求重查 = 退出/被移除即时失权**，老/新连接一致）；P3 写操作（UPLOAD_INIT/DELETE）要求群 `active`，读操作（LIST/DOWNLOAD）成员可读历史（D5）；P4 归属 `group_file_id` 必须与信封 `group_id` 同群且 active；P5 删除授权 `role='owner'` 或 `uploader_name==actor`；P7 对象存在性。**逐条与 design §2.2 相符**。
- **引用计数语义扩展**：`attachGroupFile`（+1）与 `releaseGroupFile`（−1）与既有 `commitUpload`/`releaseEntry` 共用同一 `file_object.refcount` 计数（`CREATE` 时=1；既有对象 `+1`；删除 `GREATEST(refcount-1,0)` 且 `IF(refcount<=0,'pending_reclaim','ready')`）。逐分支推演（首次/幂等命中/同名不同对象 (N) 递推/软删后复活/上传者个人条目已存在）**计数均正确，无重复计数或漏计**；`pending_reclaim` 判定依赖 MySQL 单表 UPDATE 左到右求值语义（5.7 下成立）。
- **绝不物理删**：`grep` 全 `Server/` 无对 `file_object` 的 DELETE、无后台回收器 → `pending_reclaim` 对象始终留在盘上，"命中复活"不可能引用到已回收字节（**FACT**，排除悬空引用）。
- **路径安全**：群请求**不接受任何客户端路径**——入口只有 offset/limit、size/sha/name、`group_file_id`；下载路径由服务端 `group_file.object_id → file_object.storage_path → objectAbsPath()` 解析（`objects/<sha前2位>/<id>`），客户端输入永不出现在路径中（`A5-no-path-input` 静态断言 + 源码确认）。
- **返回/回滚一致性**：`finalizeUploadOnComplete` 在群引用建立失败时 `releaseEntry` 撤销个人条目并整体判失败（不留半成品）；`attachGroupFile` 全程单事务，失败即 rollback。
- **分派接线**：99/101/103 落在 `handlMsg` 群 case 段（actor 门之后），105 单独 case 先发 106 再 `startDownloadStream(服务端解析路径)`；四类均登记 `authRejectPdu` request→respond 映射；未加入 `isPublicRequestType` 白名单（**独立读 `isPublicRequestType` 确认仅 regist/login/captcha/heartbeat 为公开**）。
- **CLAIM（未运行验证）**：`clampUtf8` 的 `(N)` 递推在极端并发同名下的稳定性未做运行级压测（设计以 `uk_group_file_name` 兜底，推断成立，未实证）。

### 7. 影响闭包（源码直读，替代不可用索引）

调用路径（直读 `0b83aab` 源）：

`MyTcpSocket::handlMsg`（`mytcpsocket.cpp:53` actor 门 → case 99/101/103 → `:255` 一带）
→ `MsgHandler::groupFileRequest`（`msghandler.cpp:1953`）
→ `OperateDB::handleGroupFileOperation`（`operatedb.cpp:1829`）
→ `FileObjectStore::attachGroupFile` / `releaseGroupFile`（`fileobjectstore.cpp:919/1089`）。

上传数据面：`MyTcpSocket::handleUploadData` → `handleUploadWriteFinished` → `MsgHandler::finalizeUploadOnComplete`（`msghandler.cpp:1448`）→ `commitUpload` + `attachGroupFile`（群作用域由 `m_groupUploadGroupId` 携带，个人上传路径与秒传路径均显式清空该上下文，**无上下文泄漏**）。

既有分支（注册/登录/聊天/群 60–96/文件 29–32·40–43·97/98）在 diff 中零改写；回归由 e2e 证据互证。

## 发现（Findings）

### 可归因于候选 `0b83aab` 的**阻塞**缺陷：0 条

### 非阻塞观察（不进 repair，供 owner/architect 归档择机处理）

1. **权限门非事务/非行锁（FACT，设计口径偏差 + 并发硬化，低-中）**：design §2.2 明写"同一事务"与"P1/P2 行锁 + FOR UPDATE"，但 `handleGroupFileOperation` 的 P1/P2/P4 是**普通 SELECT、无事务**，仅 `attachGroupFile`/`releaseGroupFile` 自开事务。后果：成员退出/被移除与**单条在飞请求**的竞态可放行该请求（TOCTOU）；群上传的数据分块阶段（31/32）本身不再复检群成员资格，故"上传期间被移除"仍可完成。**顺序撤销（验收 3 实测项）不受影响**——每请求重查 `group_member.status='active'`，后续 list/download/delete/upload 立即 -2101。窗口不可由客户端操纵。建议：把 P1–P5 与变更并入同一事务并对 `group_info`/`group_member` 取 `FOR UPDATE`，或在 `releaseGroupFile`/`attachGroupFile` 事务内复检角色。
2. **群下载失败静默（FACT，客户端 UX，低）**：`Client::handleGroupFileDownloadRespond` 先判 `uiMsgLen < 36` 即 return；而失败 105 的服务端响应仅 20B 信封 + 4B code = **24B**，故 `code!=0` 分支永不触达 → 用户对失败的群下载**无任何提示**。建议先解信封+code（`>=24`），仅当 code==0 时才要求 ≥36。
3. **102 展示名语义偏差（CLAIM，低）**：design §1.2 称 102 的 display_name 为"服务端最终展示名（含 (N) 去重结果）"；未命中路径下 102 回的是**请求名**，最终群名在 finalize 的 `attachGroupFile` 阶段才可能加 `(N)`。功能影响为零（客户端完成后重列）。需运行级探针确认 (N) 场景端到端。
4. **全局内容去重可"无实体入群"（观察，低，**既有模型非本候选引入**）**：群 UPLOAD_INIT 命中按 `sha256+size` **全局**匹配（不限定 actor），知道某文件摘要的群成员可把他人物理对象挂入本群。与 FILE-002 个人秒传同源（同一 `findObjectBySha` 全局口径），属内容寻址仓的既定属性。建议 architect 侧评估是否需按 actor 限定或加审计。
5. **名称钳制为 70 字节而非 70 列（观察，低）**：`clampUtf8(name, 70)` 按 **UTF-8 字节**截断，design §2.2 写"钳到 70 列（字符）"；对中文名明显更严（≈23 个汉字）。保守无碍（且是 767B 索引键所需），属措辞/行为轻微出入。
6. **客户端上传单例假设（观察，低，**既有模式**）**：`GroupFile::s_uploading` 为静态单指针，`Client::handlMsg` 把所有 32 确认路由到 `uploadingInstance()`；两个群文件窗口并发上传会误路由。与个人上传既有"连接内单上传"假设一致，非本候选新引入。

> 既有基线遗留 `FINDING-PRE1`（`getchatMsg` 离线消息重复下发）不在本需求回归面，本候选未触及该路径，不阻断本节点（沿用 test/前序审计结论）。

## 未执行项 / 边界

- 未在审计快照中重新编译或运行服务端（LARGE 审计输入 = 管道报告 + 差异 + 影响闭包；只读不运行）。被测二进制与三门日志按 §3 对账。
- 未重跑独立 harness（只读复核其日志/源码/哈希一致性）。
- 未做 GUI 冒烟（无法自动化，test 已声明未尝试，维持 NEEDS-HUMAN 遗留）。
- 影响面未能用 codegraph 索引计算（索引在 master，新符号不在其中；工具自述 NOT_CHECKED），以源码直读替代（§7）。

## 结论

**PASS**：候选 `0b83aab` 满足 design §6 验收映射；冻结路径为已批准的 99–106 **纯追加且两端逐字节一致**；三门与证据自洽、哈希绑定成立（候选之上无代码提交）；独立测试"0 条候选归因缺陷"经独立复证成立；路径边界与卫生禁项无越界；无伪造证据。**可进入 integration**（三事实行：本审计报告已存在且被接受、frozen-path diff 为空、合并结果 pipeline GREEN）。6 条非阻塞观察不影响放行。

## 交接行（建议写入分支 handoff 的 `audit:` 字段，由 owner 执行）

`audit: pending — build/evidence/codex-req-ext-group-003-group-files/0b83aabe6bff45dc224dce88eb9ef87690d81ac8/audit-report.md (PASS, 0 blocking, 6 non-blocking)`

## cleanup_status

- 审计工作树 `build/wt-audit-group003`（detached @ `0b83aab`）：报告落盘后删除（`git worktree remove --force` + `git worktree prune`）；删除前工作树内产品源码零改动（唯一新增为 gitignored `build/audit/`）。
- 本节点**零提交**（分支 head 仍为 `9129900`；工作树只读）。
- 未启动服务端进程、未占用 5000 端口、未写数据库。
- 证据落盘：`build/evidence/codex-req-ext-group-003-group-files/0b83aabe6bff45dc224dce88eb9ef87690d81ac8/`（本报告、`independent-verification.log`）；`build/audit/branch.diff`（工作树内临时，随工作树删除）。
