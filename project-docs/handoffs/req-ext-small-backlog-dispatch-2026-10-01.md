# 交接词：SMALL 遗留修复批次（REQ-EXT-FILE-002 集成后）

> 用法：新会话首条消息整段粘贴本文。节点 = 会话。基线 `master@368063b`。

## 开场（按序）
1. 用 Skill 工具加载 `development-os-v1-7`（治理层）。
2. 跑 `powershell -ExecutionPolicy Bypass -File pipeline/state.ps1`，确认 HEAD 与基线一致。
3. 读 `project-docs/tasks/REQ-EXT-FILE-002/task.md` 的「下一节点」节 + 本文件。

## 任务（每项一个独立 SMALL 分支：一需求一分支、一提交、pipeline 绿即合并 no-ff 到 master、**不 push**）
顺序按 T1 → T2 → T3 → T4；这是 v1.7「无关缺陷各开 SMALL 分支」的应用，不要打包成一个分支。

### T1 — `small/pipeline-audit-frozen-parse`（F1 + F2，同一文件 `pipeline/audit.ps1`）
- **F1（medium，事实，CONFIRMED）**：冻结面解析对 `Server/protocol.h` 静默失明。
  - 机理：`audit.ps1:29` 用 `Get-Content -LiteralPath $manifest`（PS5.1 默认 ANSI/ACP936）逐行读；`project-docs/project-manifest.yaml` 为 **LF**；`frozen_paths` 首项注释以全角 `）`（U+FF09 = 字节 `EF BC 89`）结尾，`0x89` 作前导字节吞掉后继 `0x0A` → 两行并作一行 → 只解析出 `Client/protocol.h`。
  - 环境依赖：`core.autocrlf=true`、无 `.gitattributes`；全新 checkout 为 CRLF 时两项都能解析出（这就是测试节点误判「不可复现」的原因）。
  - 证据：`project-docs/evidence/REQ-EXT-FILE-002/independent-audit/report.md` §3 F1（含拟修法）。
  - 修法：改为 UTF-8 明确解码（如 `Get-Content -Raw -Encoding UTF8` 后按行分割，或直接用 .NET UTF8 读字节再解析）；**不得**对含中文注释的行做单行正则。
  - 硬约束：**只修解析、不放宽门**。修后必须给出可复现证据：主工作树（LF manifest）下解析出 **2** 项，且构造一个含 frozen 改动的分支能报出 **2 个** `FROZEN-PATH HIT`。
  - `audit.ps1` 头注释自述 "Repo contract: change only via its own SMALL task" — 正是本任务；仓库契约只此一处可改。
- **F2（信息）**：`impact-check: NOT_CHECKED`（无 test glob），影响面空跑。它当前是 advisory、不改变 verdict。二选一并写明结论：配置 test glob 使影响面有意义，或显式记录为 `NOT_APPLICABLE` 并说明原因。
- 收口证据：改后 `audit.ps1` 在 master 与在含 frozen 改动的分支上各跑一次，日志留证。

### T2 — `small/unify-e2e-runform`（FINDING-DEV-2）
- 事实：FILE-001 四套件依赖**旧相对运行形态**（`filesPath=./filesys` + `cwd=Server` + exe 在 `build/build-Server/release/`），与现行 `Server/connect.config` 的绝对路径 `E:/qt/xiangmu/dist/filesys`（须 `cwd=dist`）冲突；dev 曾以临时相对配置复现旧形态跑绿后还原。
- 目标：把 5 个 e2e 脚本统一到**一种**运行形态并在脚本头注释写明调用方式（cwd、exe 路径、config 来源、需要的未入库 dll）。
- 脚本：`project-docs/evidence/REQ-EXT-FILE-001/{entry_ops_e2e.py,dedup_e2e.py,cross_tree_e2e.py,sec001_regression.py}`、`project-docs/evidence/REQ-EXT-FILE-002/fastupload_e2e.py`。
- 验收：统一形态下 5 套件全部重跑绿（基线计数：fastupload 46/46、entry_ops 43/43、dedup 13/13、cross_tree 37/37、sec001 48/48）；日志存档。

### T3 — `small/normalize-logic-keys-predicate`（FINDING-DEV-3）
- 事实：`OperateDB::normalizeLogicKeys()`（`Server/operatedb.cpp`）的两条 SQL 选择谓词仍把 `/filesys/<owner>/…` 视为 canonical，存量该形态行不会被转换（数据面未收口）。当前库实测 `LIKE '/filesys/%'` 计数 = **0**（潜伏未触发）。
- 目标：谓词与 `isLogicKeyForm` 的判别对齐；补回归夹具覆盖该形态（含幂等重放）。
- 边界：这是**生产源码**改动，提交前必须 pipeline 绿。若实测证明修复超出函数局部（如需改表/改语义）→ **停止并回报**，按 `task.md` 既定预案降级处理，不要就地扩大范围。
- 若判定为「当前无触发面、不值得改」，也可给出 `WONT_FIX` + 理由，并在 `task.md` 记一行；不强制修。

## 环境事实（复现要点）
- 构建：`pipeline/build.ps1`（Qt 5.14.2 mingw73_64 + mingw730_64，Release 产物落 `dist/`）。
- Python 用 **`py -3.13`**；PATH 上的 `python` 是旧 Anaconda，勿用。
- DB：本机 MySQL 5.7 库 `mydb`；迁移脚本幂等可重放。
- Server 运行：配置为绝对 `filesPath`，须以 `dist/` 为 cwd 启动；`dist/libmysql.dll`、`dist/sqldrivers/qsqlmysql.dll`、`dist/platforms/` 需就位。
- 遇到无法执行的验证（GUI／人工观察／凭据／设备）：**最多尝试一次**，输出 `NEEDS-HUMAN` + 确切步骤并停下，不要重试循环。

## 收口要求
- 每个分支：≤10 行 handoff（`validate-handoff.py` 校验通过）+ 任务记录追加行 → 合并 `master`（no-ff，零冲突）。
- 全程禁止：跨分支合并、改动冻结面（`Client/protocol.h`/`Server/protocol.h`）、`push`。
- 全部完成后给出一页汇总：分支名 / 合并哈希 / 每项结论（FIXED / WONT_FIX / NOT_APPLICABLE + 理由）/ 复跑计数。

## 会话预算
- 软上限 60 请求/会话。四项预计吃满：**T1（F1 解析修复 + 双向证据）与 T2 优先**；若余量不足，先交回 handoff，把 T3/T4 留给下一个会话——**不要为赶进度压缩验证**。
- 若某任务判定为「不该修」，直接记 `WONT_FIX` + 理由收口，比勉强改动更符合治理。

## 执行台账（append-only）
- 2026-10-01 T1 `small/pipeline-audit-frozen-parse` → 合并 `master@488f964`（no-ff）：F1 **FIXED**（清单改按 raw UTF-8 读；主工作树 LF 解析 1→2；临时双冻结分支报 2 个 HIT，分支已删未合并）；F2 **NOT_APPLICABLE**（仓库 0 静态可达自动化测试，manifest 显式声明，不配置 glob 以免空结果被误判 PASS）。build GREEN / test GREEN-WARN(0) / audit PASS。handoff `handoffs/small-pipeline-audit-frozen-parse-2026-10-01.txt`。
- 2026-10-01 T2 `small/unify-e2e-runform` → 合并 `master@0d92607`（no-ff）：**FIXED**（五套件统一形态 cwd=`<repo>/dist` + `dist/Server.exe` + `filesPath=./filesys`（相对）+ 自管理生命周期 + 脚本自建夹具）；重跑全绿 13/43/37/48/46。**环境变更（不入库，需主人知悉）**：未跟踪的 `Server/connect.config` 第 3 行改回 `./filesys` 且必须 CRLF（`server.cpp:25` 按 `
` 切分，LF 会 segfault）。handoff `handoffs/small-unify-e2e-runform-2026-10-01.txt`。
- 2026-10-01 会话收口：T1、T2 完成并合并 master（488f964、0d92607）。**T3（normalizeLogicKeys 谓词收口）与 T4（本批次统一收口）未开始**——按交接词「会话预算」条款交回下一会话，未压缩验证。下一会话从 `master@f48f651` 起，直接执行 T3。环境现状：`Server/connect.config` 第 3 行 = `./filesys`（CRLF），`dist/Server.exe` 已按该配置重建。
