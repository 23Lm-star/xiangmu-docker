# 任务记录：small/unify-e2e-runform（FINDING-DEV-2）

- **tier**: SMALL
- **tier 依据**: 只改证据目录下的 e2e 脚本 + 文档；不碰 `Client/`、`Server/` 产品源码，不碰 `pipeline/`，不碰 `frozen_paths`；单提交可回滚。
- **owner_agent**: claude-code
- **date**: 2026-10-01
- **状态**: COMPLETED
- **来源**: REQ-EXT-FILE-002 `FINDING-DEV-2`（dev 发现，独立测试与独立审计均确认）。

## 需求（业务语言）

五套端到端回归脚本此前"各跑各的"：FILE-001 的四套要旧形态（工作目录 `Server/`、`filesPath=./filesys`、执行文件取自 `build/build-Server/release/`），fastupload 要新形态（工作目录 `dist/`、绝对 `filesPath`）。复跑前得临时改配置、跑完再改回来，容易跑出"假红"。统一成一种形态，并在每个脚本头部写明怎么跑。

## 验收标准

1. 五套件运行形态一致：工作目录、执行文件路径、config 来源、所需未入库 dll 都在脚本头注明。
2. 统一形态下五套件全部重跑绿（基线：dedup 13、entry_ops 43、cross_tree 37、sec001 48、fastupload 46）。
3. 不破坏 cross_tree 的"两棵树"场景语义。

## allowed paths

- `project-docs/evidence/REQ-EXT-FILE-001/{entry_ops_e2e,dedup_e2e,cross_tree_e2e,sec001_regression}.py`
- `project-docs/evidence/REQ-EXT-FILE-002/fastupload_e2e.py`
- `project-docs/evidence/E2E-RUNFORM.md`（新增，单一形态说明）
- `project-docs/tasks/small-unify-e2e-runform/`、`project-docs/evidence/small-unify-e2e-runform/`、`project-docs/handoffs/`

## 不做的事（划界）

- **不改产品源码**：`Server/`、`Client/` 零改动。
- **不改 pipeline 脚本**：`pipeline/*` 零改动（T1 已单独收口）。
- **不碰冻结面**：`protocol.h` 零改动。
- **不改 e2e 断言**：只改运行形态与夹具自建；用例数与断言语义逐条不变。

## 技术结论

| 项 | 结论 | 依据 |
|---|---|---|
| 收敛到哪种形态 | **相对 `./filesys` + Server cwd = `<repo>/dist`** | `Server/connect.config` 经 `config.qrc` **构建期内嵌**，`server.cpp:25` 按 `\r\n` 切分、要求 8 行；内嵌值必须为相对路径，cross_tree 的"两棵树"才成立（绝对路径下两棵树退化为同一物理根，用例语义丢失）。tracked 模板 `Server/connect.config.template` 本就是 `./filesys` |
| 执行文件 | `<repo>/dist/Server.exe` | qmake `DESTDIR=dist` 直接落 `dist/`；`build/build-Server/release/Server.exe` 是**陈旧残留**（旧脚本取它即取到过期二进制） |
| 生命周期 | 五套件统一自管理（`taskkill` + `Popen(cwd=dist)`） | 此前只有 entry_ops / cross_tree 自管理，其余三套要求"先手工起服"，是形态分裂的一半 |
| 夹具 | 用户根目录由脚本显式 `makedirs` | 服务端只在**首次注册**时建用户根；账号在多轮运行后已在 DB 中，旧形态靠 `Server/filesys/` 的历史残留"碰巧"存在 |

## 环境变更（不入库，须主人知悉）

- `Server/connect.config`（未跟踪的机器本地文件）第 3 行由绝对 `E:/qt/xiangmu/dist/filesys` 改为相对 `./filesys`，
  **且必须保持 CRLF**（`server.cpp:25` 按 `\r\n` 切分；写成 LF 会让整份配置并作 1 行 → 启动崩溃 segfault）。
  这是使"一种形态"成立的**唯一**前提；tracked 模板与 `Client/connect.config` 本就如此。
- 构建期由 `Server/connect.config` 内嵌，故改后须 `pipeline/build.ps1` 重新构建（产物直接落 `dist/`）。

## 复现（失败模式留档）

| 现象 | 根因 | 处置 |
|---|---|---|
| 重建后 `dist/Server.exe` 一启动即 segfault | 配置被写成 LF，`server.cpp:25` 的 `split("\r\n")` 只切出 1 行 → "配置不完整" | 配置以 CRLF 重写 |
| dedup 13 用例全红（上传初始化失败） | `dist/filesys/dedupuser` 不存在（旧形态靠 `Server/filesys/` 残留） | 脚本自建夹具目录 |
| sec001 K1i/K2b 失败并 IndexError | `dist/filesys/sec001a` 等用户根不存在 | 四用户根显式预建 |

## 验证结果

| 门 | 结果 |
|---|---|
| `dedup_e2e.py` | **13/13 passed**（`dedup_e2e.log`） |
| `entry_ops_e2e.py` | **43/43 passed**（`entry_ops_e2e.log`） |
| `cross_tree_e2e.py` | **37/37 passed**（`cross_tree_e2e.log`） |
| `sec001_regression.py` | **48/48 PASS**（`sec001_regression.log`） |
| `fastupload_e2e.py` | **46/46 passed**（`fastupload_e2e.log`） |
| 重跑稳定性 | 五套件在最终形态下**完整重跑一次全绿**（上表即重跑日志；修复过程中另各跑过一次同样全绿） |
| `pipeline/build.ps1` | **GREEN**（`build-9016154.log`） |
| `pipeline/test.ps1` | **GREEN-WARN**（0 自动化测试注册，诚实报告；`test-9016154.log`） |
| `pipeline/audit.ps1` | **PASS**（`audit-branch.log`；另补 `test-488f964-merge.log` 以覆盖 T1 合并提交的 evidence 引用） |

## 提交

- 脚本 + 说明：`test(e2e): unify the five regression harnesses on one run form (FINDING-DEV-2)`
- 文档：本记录 + 证据归档 + handoff
- 合并：`merge: small/unify-e2e-runform — …`（no-ff）
