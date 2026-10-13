# 交接词：SMALL 遗留收尾（T3 + T4）

> 用法：新会话首条消息整段粘贴本文。基线 `master@db04bd8`（以 `state.ps1` 实测为准）。
> **rev2（2026-10-01，GROUP-003 合入后）**：rev1 写于 `master@60a5fc9`，当时 GROUP-003 未合入。差异见下节。

## rev1 → rev2 的差异（务必先看）
- master 已前进：`495e552 merge: REQ-EXT-GROUP-003 群文件目录与访问控制 (LARGE)` 合入，HEAD 现为 `db04bd8`。
- 契约面现状：`Client/protocol.h` + `Server/protocol.h` 已按批准追加 **PDU 99–106**（只追加不重编号）；新增表 `group_file`；`schema_version` 新增 **`2026100103`**（现值集合：2026092901 / 2026093001 / 2026100101 / 2026100102 / 2026100103）。
- 新增 e2e（T3 的回归面变大）：
  - `project-docs/evidence/REQ-EXT-GROUP-003/groupfile_e2e.py`（45/45，**自管理** Server，统一形态）
  - `project-docs/evidence/REQ-EXT-GROUP-002/independent-test/group_e2e_regress.py`（**不自管理**，需先起服，且依赖 `pymysql`）
- 残留 worktree 的待裁决项现已定位清楚：`build/wt-test-group001-repair`（detached `514d150`）里存在 `project-docs/handoffs/req-ext-group-001-independent-test-2026-09-30.txt`，**不在 master 上**——归档或丢弃，另开一个 SMALL 处理，**不要塞进 T3**。

## 开场（按序）
1. 用 Skill 工具加载 `development-os-v1-7`（治理层）。
2. 跑 `powershell -ExecutionPolicy Bypass -File pipeline/state.ps1`，确认 HEAD 与基线一致。
3. 读 `project-docs/handoffs/req-ext-small-backlog-dispatch-2026-10-01.md` 的「执行台账」（T1、T2 已完成）+ 本文件。

## 本批次已完成（不要重做、不要回退）
- **T1** `small/pipeline-audit-frozen-parse` → 合并 `master@488f964`：F1 **FIXED**（`audit.ps1` 改按 raw UTF-8 读 manifest；主工作树冻结解析 1→2）、F2 **NOT_APPLICABLE**（manifest 声明 `code_intel.impact_status`，未配 test glob 以免造假 PASS）。
- **T2** `small/unify-e2e-runform` → 合并 `master@0d92607`：FINDING-DEV-2 **FIXED**（五套件统一运行形态，重跑 13/43/37/48/46 全绿，规范见 `project-docs/evidence/E2E-RUNFORM.md`）。
- **环境变更（需知悉）**：未跟踪文件 `Server/connect.config` 第 3 行 = `./filesys`，**必须 CRLF**（`server.cpp:25` 按 `\r\n` 切分，LF 会让 `dist/Server.exe` 启动即 segfault）；`dist/Server.exe` 已按此重建。

## 任务（每项一个独立 SMALL 分支：一需求一分支、一提交、pipeline 绿即 no-ff 合并 master、**不 push**、不跨分支合并、不碰冻结面）

### T3 — `small/normalize-logic-keys-predicate`（FINDING-DEV-3）
- **事实（代码指针，已按 `db04bd8` 复核）**：`Server/operatedb.cpp:208 OperateDB::normalizeLogicKeys()` 用两条 SQL 选择谓词把"canonical 形态"定义为
  `parent_path LIKE '/%' AND parent_path LIKE '%/' AND parent_path NOT LIKE '%//%'`（`:229-230`，冲突基准预种子）与其余集的转换选择（`:237-238`）。
  该定义**把 `/filesys/<owner>/…` 也判为 canonical**，与 `isLogicKeyForm`（`Server/fileobjectstore.cpp`，fs 根 basename 判别）不一致 → 存量该形态行不会被转换（**数据面未收口**）。
- **实测口径**：`SELECT COUNT(*) FROM user_file WHERE parent_path LIKE '/filesys/%'`——当前库为 **0**（潜伏未触发）。GROUP-003 之后请**重测一次**，并顺带核查 `group_file` 关联的 `user_file` 行是否有该形态。
- **目标**：谓词与 `isLogicKeyForm` 的判别对齐；补回归夹具覆盖该形态（含幂等重放）。
- **边界**：这是**生产源码**改动，提交前必须 pipeline 绿（`build.ps1` / `test.ps1` / `audit.ps1`）。若实测证明修复超出函数局部（如需改表/改语义）→ **停止并回报**，按任务记录既定预案降级处理，**不要就地扩大范围**。
- **允许的另一结论**：若判定为「当前无触发面、不值得改」，可给 `WONT_FIX` + 理由，并在任务记录记一行；**不强制修**。
- **回归面（新增要求）**：改动落在迁移/规范化路径，除三件套外至少要跑
  `cross_tree_e2e.py`（37/37，存量键转换 + 幂等重放）与 `REQ-EXT-GROUP-003/groupfile_e2e.py`（45/45，群文件条目复用 `user_file` 键）。
- **收口证据**：修复前/后 `LIKE '/filesys/%'` 计数、夹具重放幂等的 DB 输出、上述套件日志、分支 `audit.ps1` PASS 日志。

### T4 — 批次收口
全部完成后给出一页汇总：**分支名 / 合并哈希 / 每项结论（FIXED / WONT_FIX / NOT_APPLICABLE + 理由）/ 复跑计数**；并把残留 worktree 的归档/丢弃决定记一行（决定本身可交主人，记录必须有）。

## 环境事实（实测，直接用，别重新踩坑）
- 构建：`pipeline/build.ps1`（Qt 5.14.2 mingw73_64 + mingw730_64；qmake `DESTDIR=dist`，产物直接落 `dist/`）。`build/build-Server/release/Server.exe` 是陈旧残留，勿用。
- Python 用 **`py -3.13`**；PATH 上的 `python` 是旧 Anaconda，勿用。MySQL CLI `C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe`，库 `mydb`，迁移脚本幂等可重放。
- e2e 统一运行形态：`project-docs/evidence/E2E-RUNFORM.md`（cwd=`<repo>/dist`、`dist/Server.exe`、`filesPath=./filesys`、脚本自管理 Server 生命周期、夹具自建）。
- **新 worktree 的 `dist/` 只有 exe**，没有 `Qt5*.dll` / `libmysql.dll` / `sqldrivers` / `platforms`（未入库）。自管理套件会在 `PATH` 前置 `C:/Qt/5.14.2/mingw73_64/bin` 所以能跑；**不自管理的 harness（如 `group_e2e_regress.py`）必须先由设置 PATH 的包装器起服**，裸跑会连接被拒。
- **合并后补证**：merge 提交落 master 后，在该提交上跑一次 `pipeline/test.ps1`（诚实 0，约 1 秒），否则后续 docs-only 提交的 `audit.ps1` 会因找不到 `test-<mergehash>.log` 报 `audit RED: evidence`。
- 遇到无法执行的验证（GUI／人工观察／凭据／设备）：**最多尝试一次**，输出 `NEEDS-HUMAN` + 确切步骤并停下，不要重试循环。

## 收口要求
- 每个分支：≤10 行 handoff（`py -3.13 C:/Users/王晨扬/.claude/skills/development-os-v1-7/scripts/validate-handoff.py <file>` 校验通过）+ 任务记录追加行 → 合并 `master`（no-ff，零冲突）。
- 全程禁止：跨分支合并、改动冻结面（`Client/protocol.h`/`Server/protocol.h`）、`push`。
- 并行开发面：若有其它需求会话在跑，遵守 F-6 错峰纪律（共享 5000 端口 / MySQL / 整机 `taskkill Server.exe`），同一时刻单一活动开发区。

## 会话预算
- 软上限 60 请求/会话。只剩 T3 + T4，预计一个会话可完成；若余量不足，先交回 handoff，不要在验证上压缩。
