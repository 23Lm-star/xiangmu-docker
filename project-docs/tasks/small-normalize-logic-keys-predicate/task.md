# 任务记录：small/normalize-logic-keys-predicate（FINDING-DEV-3）

- **tier**: SMALL
- **tier 依据**: 单个函数内的判别条件改写（`OperateDB::normalizeLogicKeys`），不新增/变更表结构、不改 PDU、不改冻结面、不改 `pipeline/`；单提交可回滚；不 push。
- **owner_agent**: claude-code
- **date**: 2026-10-01
- **状态**: COMPLETED（合并 `master` = `fcceff8`）
- **来源**: REQ-EXT-FILE-002 / GROUP-003 遗留批次 `FINDING-DEV-3`（交接词 `req-ext-small-backlog-t3-t4-dispatch-2026-10-01.md` rev2）。

## 需求（业务语言）

服务器启动时会把历史遗留的"文件所在的目录"字段统一改写成一套与机器无关的写法（逻辑键），
以便同一个文件在不同工作目录下被认为是同一个。改写时它用一段 SQL 挑出"已经是规范写法"的行跳过不改。

这段 SQL 的判断标准（以 `/` 开头、以 `/` 结尾、中间没有 `//`）**比实际改写函数自己的判断标准更宽**：
像 `/filesys/张三/报告/` 这种行，SQL 认为"已经规范"于是跳过，但改写函数其实认为它不规范、本该被改写。
结果：这类历史行永远不会被改写，用户在列表里看不到它们，实际发出的请求会再建一条**重复条目**——
同一个目录下出现两条同名记录，且重复条目的引用计数会多算。

本任务把两处判断标准统一到同一个来源，并把"该转换的集合"从 SQL 补集改为全量扫描 + 代码判别，
顺带消除空串/NULL 在 SQL 三值逻辑下的漏判。

## 验收标准

1. `SERVER 启动时的规范化` 与 `改写函数` 使用同一判断标准（`FileObjectStore::isLogicKeyForm` / 等价实现）。
2. `/filesys/<owner>/…/` 形态的历史行在启动后被改写为 `/…/`，重复启动结果稳定（幂等）。
3. 冲突收敛语义不变：canonical 行优先于最早旧行存活，被挤掉的行只软删 + 镜像减引用，且不改写其旧键（无 1062）。
4. 五套 e2e + groupfile 套件全绿；三件套（build/test/audit）绿。

## allowed paths

- `Server/operatedb.cpp`（`normalizeLogicKeys` 局部）
- `project-docs/evidence/REQ-EXT-FILE-001/cross_tree_e2e.py`（新增夹具；不改既有断言语义）
- `project-docs/evidence/E2E-RUNFORM.md`（cross_tree 基线计数 37 → 41）
- `project-docs/tasks/small-normalize-logic-keys-predicate/`、`project-docs/evidence/small-normalize-logic-keys-predicate/`、`project-docs/handoffs/`

## 不做的事（划界）

- 不改表结构、不改迁移脚本、不加 `schema_version`。
- 不改 `protocol.h`（冻结面）、不改 `pipeline/`、不改 `Client/`。
- 不改 `group_file` 语义（仅核查其关联的 `user_file` 行形态；实测全部为 `/`）。
- 不扩大范围：实测未证明修复超出函数局部（无需改表/改语义的降级预案未触发）。

## 技术结论

| 项 | 结论 | 依据 |
|---|---|---|
| 缺陷根因 | 判别二义：SQL 形状谓词 vs `isLogicKeyForm`（fs 根 basename 判别） | `Server/operatedb.cpp:229-230/237-238/293-294`；`Server/fileobjectstore.cpp:61-84` |
| 差异集合 | 旧谓词判 canonical 但实际非 canonical 的形态：首段 = filesys 根名、含盘符 `:`、含 `\`、含 `.`/`..` 段 | 逐条比对两处判别 |
| 修复方式 | canonical ⟺ `FileObjectStore::normalizeParent(owner, path)` 原样回显（与 `isLogicKeyForm` 等价，且顺带覆盖 `\` 归一） | 两处 SELECT 去掉 WHERE，判别下沉到 C++；第 2 段循环里原有的 `key == stored` 分支退化为 `continue` |
| 顺带修掉 | 旧 SQL 的 `NOT LIKE` 对空串/NULL 走三值逻辑会漏行；新写法无此面 | `parent_path` 实际为 `NOT NULL`，属防御性收益 |
| 冲突收敛 | **语义未变** | 预种子（canonical）→ 按 `file_id` 单遍 → 撞键时 loser 只软删 + 镜像减引用 |
| 实测口径 | `SELECT COUNT(*) FROM user_file WHERE parent_path LIKE '/filesys/%'` | 修复前 = 0（潜伏未触发）；GROUP-003 后复核仍 = 0；`group_file` 关联的 5 行 `user_file` 全在 `/` |

## 验证结果

| 门 | 结果 |
|---|---|
| `pipeline/build.ps1` | GREEN @6175c54、@fcceff8 |
| `pipeline/test.ps1` | GREEN-WARN（0 registered tests，诚实 0） |
| `pipeline/audit.ps1` | **PASS @6175c54**（分支）、PASS @fcceff8 / @5d1cabb（master） |
| 夹具先行（同一夹具打旧码/新码） | 旧码 **37/41（CVXG/CVXH/CVXJ/CVXK FAIL）** → 新码 **41/41** |
| 计数探针（同一夹具，各启动两次） | BEFORE：`LIKE '/filesys/%'` user_file=1 / upload_session=1，两次启动均不变<br>AFTER：boot1 → 0 / 0，行落 `/sub/`；boot2（重放）仍 0 / 0 且仍 1 行 |
| cross_tree | **41/41**（新增 CVXG/CVXH/CVXJ/CVXK） |
| entry_ops / dedup / sec001 / fastupload / groupfile | 43/43 · 13/13 · 48/48 · 46/46 · 45/45 |

## 复现与留档

| 现象 | 根因 | 处置 |
|---|---|---|
| 夹具在旧码下 4 条 FAIL | SQL 形状谓词把 `/filesys/<owner>/sub/` 判为 canonical → 跳过转换 | 判别对齐 `normalizeParent` |
| BEFORE 日志的产出方式 | 需要"旧二进制"才能证明夹具能红 | `git stash push -- Server/operatedb.cpp` → 重建 → 跑套件 → `git stash pop` → 重建（见证据 README） |

证据归档：`project-docs/evidence/small-normalize-logic-keys-predicate/`（含 BEFORE/AFTER 套件日志、计数探针、三件套日志）。
