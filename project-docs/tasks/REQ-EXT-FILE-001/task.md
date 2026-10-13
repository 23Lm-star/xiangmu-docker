# REQ-EXT-FILE-001 文件资产与内容寻址 — 任务记录

## 档位
- **LARGE**（goal 定档 2026-09-29，owner 同会话批准）：文件存储架构分离（物理对象 vs 逻辑条目）；DB 新增 4 表 + 存量迁移；权限边界收口（对象路径/状态只由服务端生成）；是 FILE-002/GROUP-003 的地基。需求文档自声明 LARGE。
- 硬约束：两 `protocol.h` 零命中（不新增秒传 PDU、不改分块协议——§6）；客户端零改动（D4）。
- 流程版本：Development OS v1.7。分支：`codex/req-ext-file-001-file-assets`，基线 master@`1570744`（已含 SEC-001@`7fc47c1`）。dev worktree：`build/wt-claude`（主工作树由 group-002 占用，不抢占）。
- 节点计划：goal（完成，批准即签核）→ design（`design.md` 随本记录落分支）→ development → independent test（新会话）→ repair → independent audit（新会话、只读）→ integration（三事实行）→ merge no-ff。

## 依赖核查（2026-09-29 实测）
| 依赖 | 状态 |
|---|---|
| REQ-EXT-SEC-001 | ✅ merged master@`7fc47c1`（审计 ACCEPT，48/48 e2e）。`MsgHandler::m_actorName`（连接级）、`actorUserRoot()`=`filesPath/<actor>`、`pathInActorRoot()`、`isPathInSandbox()` 可直接复用；T5 权限挂接以其为唯一身份来源 |
| 并行冲突面 | group-002 仅 goal 记录未开发（依赖 GROUP-001 未启动），源码零重叠；本分支为唯一活动开发区 |

## 决策（owner 批准，2026-09-29，D1–D4 按推荐）
- **D1** 迁移=独立 SQL 脚本（`Server/migrate_ext_file_001.sql`）+ 服务端启动 `schema_version` 版本检查（缺表/低版本 → 拒绝启动并明确日志，不静默建表改数据）。
- **D2** 物理布局=`Server/filesys/objects/{sha256前2位}/{object_id}` 内容寻址；旧路径经 `file_migration_map` 映射保留。
- **D3** 引用归零仅标 `pending_reclaim`（可恢复），本分支不做物理删除。
- **D4** 客户端零改动：所有兼容在旧路径字符串 ↔ `user_file` 逻辑条目之间的服务端双向转换层完成。

## 现状盘点（代码事实，master@1570744）
- 文件入口（`Server/msghandler.cpp`）直接操作**物理目录树**：`MKDIR`/`deldir`/`flushfile`/`renamefile`/`movefile` 全走 `QDir/QFile`，`flushfile` 以物理 `entryInfoList` 渲染 `FileInfo{caName,uiType}` 回显——同内容多副本、无引用概念。
- 上传：`uploadfileinit`→`ResumableUploadSession`（连接内存态，`stagingFilePath` 临时文件）→`UPLOADFILE_DATA` 分块 + `sendUploadAck(offset)`；会话不落库，断连即失（仅靠磁盘 staging 续传）。
- 分享（`sharefile`）：源以 canonical 物理路径登记握手（SEC-001 M-1/B-1 已收口"只能分享本人用户根内文件"）。
- `isPathInSandbox` 要求目标 canonical 存在——迁到逻辑条目后"待建目录不存在"会误拒，T3 需拆出逻辑路径校验（语法归一+actor 根约束），物理校验保留给仍走磁盘的操作（纵深防御）。
- DB：`OperateDB` 单例 + `DatabaseConnectionPool`（RAII）；建表均为手工 `migrate_*.sql`，无版本检查机制——T1 引入 `schema_version`。
- 启动链：`Server::initialize()`→loadconfig→listen；`OperateDB::connect()` 在连接池初始化处调用——版本检查挂在此后。

## 开发任务分解（T1–T8，已批准）
- **T1** 数据模型与迁移：4 表 DDL（`file_object`/`user_file`/`upload_session`/`file_migration_map`）+ `schema_version` 单行表；`migrate_ext_file_001.sql` 幂等（`IF NOT EXISTS`/唯一键）；启动版本检查 fail-fast；回滚=只建新表不动旧数据，DROP 脚本随行。
- **T2** 对象存储层：新模块 `Server/fileobjectstore.{h,cpp}`——服务端生成 `objects/{h2}/{object_id}` 路径；staging 写满→服务端 SHA-256 复算→同卷原子 rename 转正；`(owner,parent,name)` 命中同摘要对象=秒传地基（本分支不开 PDU）；引用计数±与 `pending_reclaim` 状态机。**对象命中+条目创建+计数变更同事务**（连接池取同一连接执行）。
- **T3** 逻辑条目收编：`MKDIR/deldir/flushfile/renamefile/movefile` 改为操作 `user_file`（目录=is_dir 行，object_id NULL）；`flushfile` 从表渲染（`FileInfo` 布局不变，展示名截 32 与现状一致）；同用户同目录同名同对象幂等；同名不同内容按 `(1)(2)` 规则生成；客户端伪造物理路径失效（路径只是逻辑键）。
- **T4** 上传会话落库：`uploadfileinit/UPLOADFILE_DATA/完成` 三阶段把 offset/过期/临时路径持久化 `upload_session`；staging 归 `filesys/tmp/`；重启/断连后可按表恢复续传；分块 PDU 不动。
- **T5** 权限挂接：owner 一律 `m_actorName`；客户端摘要/大小仅预检元数据，服务端复算不一致→不转正、条目不建、拒绝日志（`lcFs/lcAuth`，不落内容）；非本人对象/条目查询全以 `(owner,...)` 谓词收口；`sharefile` 源改查 `user_file` 归属（替代 canonical 物理比对），握手授权语义不回退。
- **T6** 存量迁移与兼容：`migrate_ext_file_001_data`（按 `file_migration_map` 幂等：哈希→建对象→建条目→记映射；重跑跳过；同名不同内容不覆盖，冲突时保留旧文件原样并记录）；旧目录不删；旧路径字符串↔逻辑条目转换覆盖上传/下载/列表/删除/重命名/移动/分享全回归面。
- **T7** 验收测试与证据：验收 1–5 端到端脚本（仿 SEC-001 48/48 模式：Python 构造 PDU + 服务端拒绝日志断言 + DB 状态断言）；pipeline build/test/audit；证据绑定候选哈希。**停止条件**：旧路径与新资产建不出可审计双向映射（映射表有缺/歧义）→ 停，不进 FILE-002/GROUP-003。
- **T8** 收尾：任务记录/证据归档 `project-docs/`；BRANCH-ORDER E 阶段登记；handoff。

## 允许路径
`Server/fileobjectstore.{h,cpp}`、`Server/operatedb.{h,cpp}`、`Server/resumableuploadsession.{h,cpp}`、`Server/uploadwritetask.{h,cpp}`、`Server/msghandler.{h,cpp}`、`Server/mytcpsocket.{h,cpp}`（会话持久化配合）、`Server/server.{h,cpp}`（启动版本检查）、`Server/main.cpp`（T1/T6 挂钩点，见"开发进度 节点2"main.cpp 挂钩叙述；独立审计范围项修正）、`Server/Server.pro`、`Server/migrate_ext_file_001.sql`、`Server/migrate_ext_file_001_rollback.sql`（回滚随行，独立审计范围项修正）、`Client/*`（仅回归必需的最小配合，预期零改动）、`project-docs/tasks|evidence/REQ-EXT-FILE-001/`。
**禁止**：两 `protocol.h`（frozen）、`Server/filesys/` 入库、新外部依赖、顺手实现秒传 PDU/群文件/乱序并发上传、物理删除用户旧文件。

## 运行环境事实（T1 实测，独立测试会话需照此准备）
- 本机 MySQL 5.7：**Antelope+COMPACT，索引键 767B 上限**，`character_set_server=utf8` → 新表统一 utf8+缩窄索引列（已落 DDL）。
- QMYSQL 驱动不在 Qt 发行版：需 `release/sqldrivers/qsqlmysql.dll` + exe 同目录 `libmysql.dll`（从主工作树 `build/build-Server/release/` 复制，非入库物）。
- Server.exe 运行目录必须为 `Server/`（`filesPath=./filesys` 为相对路径）；GUI 子系统，stdout 不可重定向捕获，断言走端口/日志文件/DB。
- 迁移实测幂等：重放 `migrate_ext_file_001.sql` 零报错；fail-fast 实测：版本低→进程秒退、不监听 5000。

## 开发进度（2026-09-29 development 节点，进行中）
- **T1 ✅**（`8dba0f9`）：4 表 + `schema_version`@2026092901（utf8/InnoDB，适配 MySQL5.7 Antelope 767B 键上限）+ 幂等重放实测 + `checkSchemaVersion()` 启动 fail-fast。冒烟：达标监听 5000 / 降级秒退不监听。回滚脚本就位。
- **T2 ✅**（`cd17507`）：`FileObjectStore` 内容寻址核心——`hashStaging`（流式 SHA-256）+ `commitUpload`（对象 find-or-create / 条目建或复活 / 引用计数，同一事务；新对象 staging 原子 rename 到 `objects/{sha2}/{id}`，命中既有对象丢弃 staging）。`releaseEntry/releaseSubtree`（归零→`pending_reclaim`，绝不物理删）。DB 级事务模拟实测验收 1/2/3。Server.build GREEN。
- **T4 🟡 groundwork（本次，additive/未接热路径，零回归）**：`ResumableUploadSession::beginStaged`（staging=`.staging/<sha1(owner|parent|display|size)>.part`，确定性路径支持断点续传）+ `OperateDB::{createUploadSession,updateUploadOffset,finishUploadSession}`（upload_session 表 CRUD）。均未被 handler 调用，现网行为不变。Server.build GREEN。

## 剩余开发（节点2 已全部落地，见"开发进度 2026-09-30 节点2"）
读/写权威切换（T3+T5）+ 迁移（T6）+ e2e（T7）互相依赖，不能分批否则回归：
1. **写侧切换**：`uploadfileinit`→`beginStaged`+`createUploadSession`；`handleUploadWriteFinished` 完成时→`MsgHandler::finalizeUpload()`=`commitUpload`+`finishUploadSession`。
2. **读侧切换（与写侧同批）**：`flushfile`/`listdir`→`FileObjectStore::listEntries`（DB 优先，某 parent 零条目且物理目录存在→回退 QDir，兼容未迁移用户）；`downloadfile`→`entryByPath`→`objectAbsPath`（对象缺失回退 canonical 物理）。
3. **其余 handler**：`deldir`→`releaseEntry/Subtree`（无条目回退物理删）；`MKDIR`→建目录条目（+物理 mkdir）；`renamefile`/`movefile`→`relocateEntry`（无条目回退物理 rename）；`sharefilemove`→源条目 `refObject`+接收者新条目（+物理 copy 回退）。
4. **T6 迁移**：`migrateLegacyFiles()` 遍历 `filesys/<user>` 未映射文件→建对象+条目+`file_migration_map`（幂等、同名不同内容→`(1)`、失败不删旧，满足 §6 双向映射停止条件核查）。
5. **T7**：验收 1-5 端到端脚本（仿 SEC-001 48/48 模式，Python 造 PDU + 服务端日志/DB 断言）+ pipeline 三门 + 证据绑定候选哈希；随后 independent test（新会话）、independent audit（新会话只读）、integration 三事实行、merge。

**D5（owner 批准 2026-09-29）**：对象+逻辑条目为权威——新上传唯一物理副本进 `objects/`，客户端界面无改动靠服务端从 `user_file` 合成列表；旧目录仅作未迁移回退与审计映射。每 handler 采用"先查条目、无则回退物理"的兼容模式，可按 handler 分批落地而不回归。

## 开发进度（2026-09-30 development 节点2，完成）
- 提交链：`11e01de` D5 修复（parentDirInActorRoot）→ `64e3b40` dedup_e2e.py 入证据 → `51f38ef` rename/move/share/MKDIR 条目优先切换（`insertDirEntry`/`attachEntry`/`relocateEntry` 目录级联/`renameEntriesDir`/`releaseSubtreeByPath`；分享=接收者条目引用同一对象物理零复制；握手键 canonical-或-词法绝对，`logicPathInActorRoot` 按 design §6；并修复 `releaseSubtree` 循环中途失败仍 commit 的部分提交隐患→§5 一致性）→ `17af569` T6 存量迁移（`OperateDB::migrateLegacyFiles()` 启动自动一次；`commitUpload copyOnly` 变体：源绝不删除/半成品副本清理；映射 mapped/skipped_name_conflict/failed；重跑跳过=幂等；main.cpp 挂钩沿 T1 先例）。
- 实测（本机 MySQL5.7/mydb + wt-claude Server.exe 实链路）：**验收1/2/3 复跑 13/13**；**验收4/5+操作面 36/36**（`entry_ops_e2e.py`：迁移三态/幂等重跑/`(1)` 安置/旧文件零删除/rename+move+目录回退/条目分享零复制+引用计数/伪造大小→session failed 无条目无对象/跨用户拒绝/B-1 分享他人源拒）；**SEC-001 全量回归 48/48**（`sec001_regression.py`=原 verify.py 的 D5 适配副本，仅 K1c/K1f/K2a/K2b 物理落盘期望→条目+对象等价断言，安全断言零改动，原件不动）。
- §6 停止条件核查：未触发——`file_migration_map` 对测试集构成可审计双向映射（M1-M4/I1-I2 断言）。
- 运行环境补充事实（独立测试节点照此准备）：启动 Server.exe 必须 **PATH 前置 Qt5.14.2/mingw 两个 bin 目录**，否则 Anaconda 等同名 DLL 劫持导致启动崩溃（0xC0000005/0xc0000139）；Python 用 `py -3.13`（Anaconda 3.6 无 capture_output）；SEC 回归需 `SEC001_BASE=<工作树根>` 且预建 `Server/filesys/{sec001a,sec001b,sec001r,sec001v}` 用户根；脚本生命周期参考 `build/start-server.ps1`（gitignore 外，会话自管）。**新工作树首次构建前需从既有树拷贝未入库的 `Client/connect.config` 与 `Server/connect.config`（config.qrc 资源输入，缺失则 build RED: No rule to make target），并拷贝 `libmysql.dll`+`sqldrivers/qsqlmysql.dll` 到 exe 旁**（repair2 记录，独立测试会话 F-3 实测）。`SEC001_BASE` 必须以真实环境变量传递（bash 中 cmd 风格 `set` 无效，会静默回落主树）。
- 遗留/边界（证据定格，非阻塞）：路径 >191 字符的旧文件记 failed 不入库映射（超长列宽，MySQL5.7 utf8 191 上限）；未建目录条目的旧目录改名/移动后，其内已迁移条目仅前缀级联（`renameEntriesDir`），旧目录本身无条目行（flushfile 物理回退显示）；隐藏文件不入迁移扫描（QDirIterator Files 默认过滤器）。

## 开发进度（2026-09-30 repair 节点，独立审计 REJECT 后 owner 修复，本会话=claude 代偿 owner）
- 审计阻塞 F-3/D5 成立并经三层复核确认（代码/契约/独立测试实证）。修复方案=design §2 字面回归：
  - **逻辑键**：`normalizeParent(owner, raw)` 归一为 owner 相对、用户根内、`'/' 结尾` 的机器无关键（`'/'`=用户根，`'/sub/'`=子目录），接受 PDU 相对/本树绝对/既有逻辑键三形态且幂等；不再调 `absolutePath()`（工作树相关前缀是键漂移根因）。物理 IO 全部保留原始路径（isPathInSandbox/objectAbsPath/staging/movefile dstDir 物理检查）。
  - **收口面**：store 10 处键生成/前缀级联改 `'/' 结尾`语义（`logicDirKey`+startsWith）；msghandler 9 入口传 owner；movefile 条目分支"物理存在性检查"与逻辑键解耦（`dstNorm`→逻辑、`QDir(dstDir)`→物理）；迁移 `old_path` 改存 design §2 相对形态 `filesys/<user>/...`（跨树重跑天然去重）。
  - **存量转换**：`OperateDB::normalizeLogicKeys()`（main.cpp 挂钩，先于迁移、幂等可重放）——`user_file`/`upload_session.parent_path` 绝对→逻辑键；`file_migration_map.old_path` 绝对→相对；uk_loc 冲突按 file_id 最早保留、后到 active 行软删+镜像 releaseEntry 减引用（D3 不物理删）。
  - **F-2**：`sec001_regression.py` 增步骤0 DB 清场（owner 行删净+未引用对象回收，对齐 entry_ops 语义，不再以人工清场豁免冒充 PASS）。
  - **范围契约**：允许路径清单补 `Server/main.cpp`/`Server/migrate_ext_file_001_rollback.sql`（`17af569` 起即被使用，审计指出清单漏记）。
  - **新增回归**：`cross_tree_e2e.py`——两棵不同绝对根（`Server/` 与 `build/xtree-file001/`，junction 共享物理 filesys）+ 共享 mydb：同请求跨树同键幂等（不重复行/不加引用）、列表/下载/分享跨树命中、存量绝对键启动转换+重放幂等。K1f 断言改逻辑键字面量（`'/'`）。
  - **附带发现并修复（验收2 缺口，跨树回归暴露）**：`commitUpload`/`attachEntry` 同请求重复命中在 `(1)` 递推**之后**才判幂等→重复请求会建 `(1)` 重复行并 +1 引用。已改为同对象命中优先（`(1)` 仅用于同名不同对象），dedup/sec001 语义不回退。
  - **实测（最终二进制单实例连跑）**：entry_ops **36/36**、dedup **13/13**、sec001_regression **48/48**（新步骤0清场生效）、cross_tree_e2e **31/31**；三门于 `915eeff`：build GREEN / test honest-0 / audit PASS（protocol.h 零命中）。运行事实补充：harness 环境变量须用真实 env 传递（bash 内 `set` 无效会静默回落默认 BASE）；taskkill 后目录句柄可瞬态占用，sec001 预清场已带重试。
  - **证据**：`build/evidence/codex-req-ext-file-001-file-assets/915eeff…/`（四套件日志+gates.txt）；handoff `project-docs/handoffs/req-ext-file-001-repair-2026-09-30.txt`（validate-handoff OK）。

## 开发进度（2026-09-30 repair2 节点，第二轮审计 REJECT 后）
- 轮次记录：repair(`915eeff`) → 独立测试 **PASS**（36/36、13/13、48/48、31/31 全一次通过，F-3/D5 主收口成立；非阻塞 F-1..F-4 见 `build/evidence/…/915eeff…/test-report.md`）→ 独立审计 **REJECT**（新阻塞 F-4/F-5，见 `build/evidence/…/cf4f74f…/audit-report.md`）。
- **F-4**：`releaseSubtree` 在 `'/'` 结尾键下 `LIKE dirPath+"/%"` 变 `//%` 永不命中→深度≥2 后代漏删；改 owner 全行 FOR UPDATE+内存 startsWith（同 renameEntriesDir 模式，免疫通配名）。同函数目录自身行 `objectIds.prepend(value(0))` 错取 file_id 列→deldir 条目优先时减到**无关对象**（`17af569` 既有缺陷，审计误记为本轮引入，一并修复）。夹具 ND0-ND5（entry_ops 36→43）。
- **F-5**：`normalizeLogicKeys` 撞键收敛把 loser `parent_path` 改写为存活行同三重→必 1062，loser 永远 active+旧键、减引用镜像成死代码、每次启动重复告警。新策略：撞键 loser **仅软删不动 parent**（死行旧键永不命中查询）；deleted 撞键行静默跳过（重放稳定）；判别改 canonical 形态（首尾 `'/'`、无 `//`），覆盖 `/filesys/…` 无盘符形态；upload_session 同规则。夹具 CVXA-CVXF（cross_tree 31→37，含重放稳定与 refcount 5→4 镜像一次）。
- **非阻塞在册**：`/filesys/…` 判别盲区已随 F-5 收紧；`claim-check` 仍缺 manifest.yaml（工具性缺口，与首轮审计同姿态，不冒充 PASS）；handoff 校验器 `NODES` 无 `independent-test` 枚举（会话用 `node: test`）。
- 实测（repair2 二进制）：entry_ops **43/43**、dedup **13/13**、sec001 **48/48**、cross_tree **37/37**；三门 `9fbdb70` GREEN/honest-0/PASS；证据 `build/evidence/codex-req-ext-file-001-file-assets/9fbdb70/`。

## 门次结果（2026-09-30）：第二轮独立测试 **PASS** + 第二轮独立审计 **ACCEPT**
- 测试（`9fbdb70` clean worktree，只报不改）：entry_ops 43/43、dedup 13/13、sec001 48/48、cross_tree 37/37（含首轮干扰轮次原样转录，根因=他需求测试节点整机 taskkill，见 F-6）；三门 GREEN/honest-0/PASS；自造 rp2probe 撞键对验证收敛策略 boot1/boot2 逐字节稳定。报告 `build/evidence/…/9fbdb70…/test-report.md`。
- 审计（detached @`5a07316`，纯静态只读）：F-4/F-5 判定 CLOSED（De Morgan 判别分区无缝、loser 无 parent 改写无 1062、deleted 静默跳过重放稳定、dir-self prepend 取列正确、NULL 跳引用、失败全回滚）；夹具判别力对 `915eeff` 逐条核验；诚实性核验（4/4 harness 盘=提交、原始日志 FAIL=0）；F-3/F-2/范围契约重采样均闭合；协议零命中。**无阻塞**。ACCEPT 报告 `build/evidence/…/5a07316…/audit-report.md`。
- 残留非阻塞台账：F-6 并行节点错峰（共享 5000/MySQL 与整机 taskkill 风险）；带盘符+尾部斜杠旧行的告警泄漏（夹具不可触发，FILE-002 收紧）；handoffs 路径清单外先例；validator NODES/`manifest.yaml` 工具缺口；connect.config 未入库构建输入（环境事实已录）。
- **集成三事实行状态**：①独立审计 ACCEPT ✓（本报告+证据在案）②冻结面 diff 空 ✓（protocol.h×2 零命中）③pipeline green on merged result = **待 merge 后主链复跑**。三事实齐即 merge no-ff 进主干；push 单独待主人。

**下一节点**：integration（owner 执行 no-ff 合并 + 合并结果三门复跑）→ merged。

## 集成（2026-09-30，完成）
- **merge**：`codex/req-ext-file-001-file-assets@bf43c58` no-ff 合入 master（主人授权、不 push）→ **`master@9faae6c93eea55674eb1b3bd8f9699232c16a7b7`**，与并行已合 SESSION-001（`cf2103e`）零冲突。
- **三事实行（全部达成）**：①独立审计 ACCEPT（`5a07316…/audit-report.md`，无阻塞）②冻结面 diff 空（protocol.h×2 零命中）③合并结果 pipeline green（`9faae6c`：build GREEN / test honest-0 / audit PASS）。
- **合并态复跑（file-001 面）**：entry_ops 43/43、dedup 13/13、sec001 适配副本 48/48、cross_tree 37/37。
- **合并态跨面复核（SESSION-001 面）**：supervised 对抗探针 **8/8 PASS**（唯一登录特性与 file-001 无互相破坏）；其 e2e **15/16**，唯一 FAIL=E7a 调用**冻结原件** `REQ-EXT-SEC-001/verify.py` 断言"物理落盘"语义，被 D5（owner 批准）按设计失效——权威 post-D5 回归=适配副本（本需求 48/48）。判别实验：清净 DB 复跑原件只剩 K1c/K1f/K2b 三条物理断言（设计预期），此前附加的 K1i 失败=残留条目+原件无清场（F-2 族）非产品回归。
- **台账（非阻塞）**：session-001 侧若在合并后重跑 e2e，E7a 应改指适配副本 `project-docs/evidence/REQ-EXT-FILE-001/sec001_regression.py`（或接受其 16→15 的语义差异并在其记录注明）；原件 `verify.py` 按 file-001 决策保持冻结不动。
- 证据：`build/evidence/codex-req-ext-file-001-file-assets/9faae6c…/`（四套件+三门+跨面 e2e/probes/原件判别实验日志）；push 待主人明示。

**下一节点**：merged（已达成）；push 待主人指令。
1. `flushfile` 从 DB 渲染后，未迁移目录显示为空 → T6 先迁移后切换渲染，或渲染层 DB 优先 + 物理回退（双读过渡，本分支取后者更稳）。
2. 32 字节展示名 + `(1)` 后缀溢出 → 后缀插入时截基名保留扩展名，规则写进 T3 测试。
3. MySQL 事务与连接池：引用计数并发必须同连接 `SELECT...FOR UPDATE` + `COMMIT`；池按线程隔离，单连接对象操作串行化在 socket 线程内（现状 handlMsg 即同线程）。
4. 下载走物理路径处（`downloadfile`/分享回退）需经 `object storage_path` 解析，保持 `pathInActorRoot` 式校验不放松。
