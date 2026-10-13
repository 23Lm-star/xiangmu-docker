# 本任务全部非阻塞发现 → 后续分支修复建议（主人 2026-10-09 要求；暂不开修）

来源：independent test `test-report.md §7`（IND-1..6）、`human-round-addendum.md` / `pre-existing-object-drift.md`、
repair 人工复验、audit `4965738/audit-report.md §4`（A-1..A-6）。已去重（IND-6=A-2、IND-2≈A-3）。

**前置**：等 `feat/req-ext-transfer-003`（以及并行未合的 `feat/req-ext-transfer-002`）合入主干后，**从主干切分支**——
否则 S3/S5 会撞 003 的 `file.cpp`/`mytcpsocket.cpp` 上下文，S2 的口径也依赖 003 已被接受。每条都是 SMALL（单分支、单提交、
流水线绿即合），彼此无依赖，可任意顺序/并行。

| # | 分支名（建议） | 级别 | 内容与改动点 | 验证 | 备注 |
|---|---|---|---|---|---|
| **S1** | `fix/group002-harness-root` | 既有红 | `project-docs/evidence/REQ-EXT-GROUP-002/verify.py:32` `parents[2]→parents[3]`（ROOT 现落到 `project-docs/`） | 该套件从 `rc=1`（1 s 内、任何断言前）变为全绿 | 纯 harness 一行；这条不修，"回归全绿"永远差一套 |
| **S2** | `fix/unique-login-e7b-criterion` | 既有门禁口径 | `unique_login_e2e.py:202-205` E7b「分支 diff 对两 protocol.h 零命中」对任何**经批准**改协议头的分支结构性红。二选一：改为"diff 仅含 §9 批准块（逐字节比对）"或把该断言移出全分支回归 | unique-login 16/16；对 003 分支跑应绿 | **需 design owner 一句话定口径**（改口径属设计面，不属 harness 自己决定） |
| **S3** | `fix/file-page-user-path` | 中（既有缺陷） | A-4：`Client/file.cpp:71` `m_strUserPath/m_strCurPath` 只在构造期赋值，切账号后不刷新（`flushfile()`、`selectedFileRelativePath():614` 前缀剥离用**当前** `m_LoginName` ⇒ 路径不再裁剪） | GUI 一步：注销→换账号登录→打开"文件"页看路径与列表；修后应指向新账号 | claim 部分需主人 GUI 确认现象；**这是本批唯一真实功能缺陷**（与 002/003 均无关） |
| **S4** | `chore/transfer003-doc-nits` | 低 | docs-only 批次：design §9(a) 括注"新增四行"与自检"+4/−1" → 实为 5 行/`+5−2`（IND-6=A-2）；A-1 把 D-a/D-b 的"防御"改述为"可观测性 + 结构约束（当前不可达）"；A-6 deploy-note "0 warning" 注明是增量构建；IND-5（越界 31 包只断连不给明确错误，符合 design §2.1）记入 design 备注 | 免跑流水线（v1.7：docs-only 提交复用上一代码提交证据） | **不要**为了让 D-a/D-b 可达去改早退分支（审计 A-1 明确警示） |
| **S5** | `chore/stale-log-text-256kb` | 低 | `Server/mytcpsocket.cpp:635` 注释与 `:637-638` 日志文本仍写"256 KB"，常量已是 1 MiB+8 KB（IND-1）→ 改为打印常量值或更新文案 | 一行源码：需 build + 套件（可用 transfer003 53/59 条 + 13 套抽样） | 与 S4 分开：这条动 `Server/`，属代码提交 |
| **S6** | `chore/sweep-evidence-stamp` | 低（流程） | IND-2≈A-3：13 套扫尾曾在**未提交**工作树（HEAD=e49e8aa/b861351）上产出，"11 绿"与提交后事实脱钩。做法：本地驱动 `build/run_declared_suites.py` 开头打印 `git rev-parse HEAD` + `git status --porcelain` 非空即警告；并把"扫尾必须在提交后跑"写进 E2E-RUNFORM | 驱动在脏工作树上应显式告警 | 驱动不入库（per-worktree），故落地 = 更新本地驱动 + 文档约定，每个工作树复制一次 |
| **S7** | `chore/object-drift-report`（可选） | 低 | `pre-existing-object-drift.md`：主人 13 个历史对象"有行无实体"（跨运行树漂移）。建议：小脚本列出"DB 有行、实体缺失"的对象（只报不删）+ 明确"唯一运行树"策略写进文档；是否清 13 条陈旧条目由主人定 | 脚本对当前库应报出那 13 条与 2 条在 `Server/filesys` 的 | 数据类，优先级最低；主人已表示这些文件无用 |
| **S8** | `fix/pipeline-audit-ps1-restore` | 中（更早节点遗留） | 主工作树的 `pipeline/audit.ps1` 是**未适配骨架**（冻结检查为空、`codeRoots` 不认 `Client/Server` ⇒ 一切提交被误判 docs-only）。适配版可用 `git show 360baeb:pipeline/audit.ps1` 取回 | 用适配版重跑主线：冻结命中应只报批准块、docs-only 判定应正确 | repo contract，按规矩**只在本任务内改**（不得混进别的工作树）；`pipeline/` 未入库，需逐工作树同步 |
| **S9** | `fix/pipeline-artifacts-deploy` | 中（更早节点遗留） | `pipeline/artifacts.ps1 -Deploy` 会把 `build/build-*/**` 的陈旧 exe/DLL 递归覆盖进 `dist`（曾污染 Qt 5.9.5 DLL 并伪记录 provenance） | 修后用一次假部署验证：`dist` 只被当前构建的产物覆盖；provenance 用 `artifact-trace.py record` 直记 | 同上，repo contract 面 |

**建议顺序**：S1 → S2（测试面先全绿）→ S3（真缺陷）→ S5 → S4 → S8/S9（流程契约）→ S6 → S7。

**不建议动的**：IND-5 的"越界只断连"若要改成"明确错误回执"，属**契约变更**（需 design + 主人批准），当前 design §2.1 已声明越界走既有非法 PDU 路径——除非主人明确要求，保持现状并在 S4 里记录。
