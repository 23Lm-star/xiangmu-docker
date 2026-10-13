# REQ-DOC-005 任务记录 — 仓库门面：README + LICENSE

| 项目 | 值 |
|---|---|
| 档位 | SMALL（纯新增文档，单提交可回退，不触生产契约、不推远端） |
| 依据 | 需求文档 `project-docs/requirements/REQ-DOC-005-repo-chrome.md` §一（门面缺失非代码缺陷） |
| 分支 | `docs/repo-chrome`（原基线 082ebd9；REQ-REPO-006 历史清洗后重放至新基线 72e061f，内容提交 1cdaaa1→ea8dae1，映射见清洗 commit-map） |
| owner_agent | claude-code |

## 验收标准（对照需求 §二）

- A1/A2 README 存在、含依赖/构建/运行三步，按步构建成功 → 见 evidence/pipeline-run.log
- A3 LICENSE 存在且为标准全文（MIT）
- A4 学习项目免责声明置顶
- A5 connect.config 复制说明（与 REQ-REPO-004 模板呼应）
- A6 遗留分支处置有明确建议（本记录决策日志，不删任何分支）
- A7 pipeline 三门 → build GREEN / test 如实报告 / audit 见 O-4 备注

## Allowed paths

README.md、LICENSE、project-docs/tasks/REQ-DOC-005/task.md、project-docs/evidence/REQ-DOC-005/
（`.gitattributes` 经批准本次不加）

## 决策日志

1. **LICENSE = MIT**：主人 2026-09-29 批准卡选定；版权署名 `wcy`（git user，可后改）。
2. **`.gitattributes` 不加**：主人批准；本机 `core.autocrlf=true`，模板中 `eol=lf` 必然触发大面积伪 modified（§3.4 回退条款大概率命中）。行尾归一另开独立 SMALL 需求（需 `git add --renormalize` 单独评估）。
3. **遗留分支处置建议（只建议、零删除，删除属不可逆须主人逐个批准）**
   需求文档 §3.3 写于 REQ-REPO-006 历史清洗之前；下表以 **2026-09-29 清洗后实测**（`git ls-remote` + `git branch --merged`）为准：
   | 分支 | 现状（清洗后实测） | 建议 | 理由 |
   |---|---|---|---|
   | 远端 `origin/master`、`origin/backup-runnable-2026-07-30`、`origin/add_数据库连接池功能` | **已由 REQ-REPO-006 从远端删除** | 无需动作 | 已完成，本需求不重复处理 |
   | 远端 `origin/main`（@6599683 脏初始导入，GitHub 默认分支） | 唯一残存远端分支 | **归 REQ-REPO-006 后续**：主人 Web 端切默认/删仓后再推干净历史 | 属不可逆外部动作，非门面范围 |
   | 本地 `main`（@e424513 初始导入） | 未并入 master | 保留至远端默认分支处置完成 | 清洗前旧历史锚点 |
   | 本地 `backup-runnable-2026-07-30` | 已并入 master | 主人批准后可删 | 备份价值已被主干覆盖 |
   | 本地 `add_数据库连接池功能` | 已并入 master | 主人批准后可删 | 连接池已在主干实现 |
   | 本地 `fix/path-traversal-and-ownership` | 已并入 master（REQ-FS-003） | 主人批准后可删 | 死分支，文档 §3.3 未覆盖项 |
   | 本地 `worktree-agent-ac6d9e68` | 已并入 master，worktree 不存在 | 主人批准后可删 | 会话残留 |
   | 本地 `chore/history-purge` | 已并入 master（REQ-REPO-006） | 主人批准后可删 | 会话残留 |
4. **audit.ps1 在 docs 提交上的已知误报（O-4）**：仓库版未实现 v1.7 docs-only 证据回溯，直接跑会报缺证据；处置=如实记录，工具修复另开需求，不在本分支动 `pipeline/`。
5. README 技术栈修正：需求模板"自研内存池"实测代码中无内存池（仅对象池/memalloc），按禁令 #7 以代码为准。
6. `active-locks.yaml` 未登记：SMALL 单人单分支，写所有权由分支+handoff 记录，且不在 allowed paths 内。

## 完成判据

README 步骤实测可构建；全文无真实凭据/IP/密钥；本分支零删除分支、零 push（期间分支状态变化均系 REQ-REPO-006，与本需求无关）。
