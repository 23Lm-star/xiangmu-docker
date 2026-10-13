# REQ-EXT-FILE-002 集成节点报告（2026-10-01）

- 分支 `codex/req-ext-file-002-fast-upload`（tip `69a8631`）合并入 `master`，合并提交 `abe2e5d`（no-ff，零冲突）。基线 `master@1123a98`。
- owner_agent: `claude-code`（沿用分支 owner 声明）。

## 三事实行（合并前核对，逐项自证）

| # | 事实 | 证据 | 结论 |
|---|---|---|---|
| ① 独立审计存在且被接受 | `independent-audit/report.md` = **PASS**，0 条归因产品缺陷；`pipeline/state.ps1` 记 `audit: accepted by user, 2026-10-01`；`audit-2026-10-01.txt` handoff | ✅ |
| ② 冻结面符合批准清单 | `git diff master..HEAD -- {Client,Server}/protocol.h`：仅追加 `UPLOADFILE_FASTCHECK_REQUEST=97` / `_RESPOND=98`；旧编号 1–96 零改动；删除行仅 `// 97-99 继续预留` 一处注释。两端 FASTCHECK 常量块逐字节一致（本节点自算 md5 相同）。与 `task.md` 2026-10-01 批准范围（只追加不重编号 + 新偏移/magic 常量）逐条相符 | ✅ |
| ③ 合并态 pipeline 绿 | 在合并结果 `abe2e5d` 上运行：`build.ps1` **build GREEN**；`test.ps1` **GREEN-WARN**（诚实报告 0 自动化用例）；`audit.ps1` **audit PASS**（分支范围 frozen 检查在 post-merge 为空，符合脚本设计） | ✅ |

三项齐备 → 按 v1.7 直接合并（节点转移，无需再问）。合并后分支范围 frozen diff 为空（delta 已成历史），`audit.ps1` 静默，符合预期。

## 合并结果

- `master`: `1123a98` → `abe2e5d`（merge, no-ff）。
- 交付面：`Client/protocol.h`+`Server/protocol.h`（PDU 97/98 追加）、`Server/msghandler.cpp`（秒传预检查 + 命中事务 + 复算对照）、`Server/fileobjectstore.cpp`（`commitUpload`/`attachExistingObject`）、`Server/operatedb.{h,cpp}`（`client_request_id`/`file_id`/`uk_req` + schema_version 2026100102）、`Server/migrate_ext_file_002.sql` + rollback、`Client/{client,file,msghandler}.cpp`（摘要预计算 + 新分派）。
- 回归（分支上证据）：`fastupload_e2e.py` 46/46（dist ×2 + Server-cwd ×2）；FILE-001 四套件 13/43/37/48 全绿；旧 `UPLOADFILE_INIT` 路径零破坏。

## 未随本分支收口（台账，均非本分支缺陷）

| ID | severity | 归属 | 处置 |
|---|---|---|---|
| F1 | medium | `pipeline/audit.ps1` 冻结面解析（LF manifest + 全角括号吞换行，主工作树下漏检 `Server/protocol.h`） | 另开独立 SMALL 任务 |
| F2 | 信息 | `impact-check` 无 test glob，脚本影响面空跑 | 与 F1 同批或另开 SMALL |
| FINDING-DEV-2 | — | FILE-001 四套件依赖旧相对运行形态（`filesPath=./filesys`+`cwd=Server`）与现行绝对路径配置冲突 | 建议另开 SMALL 统一运行形态 |
| FINDING-DEV-3 | — | `normalizeLogicKeys` SQL 谓词未收口 `/filesys/<owner>/…`（当前库计数=0，潜伏） | 数据面 SMALL |
| FINDING-DEV-4/5 | — | 无过期会话清理调度器；`MKDIR` 非幂等 | 记录，非缺陷 |
| F5 | low | 客户端整文件同步算 SHA-256，大文件阻塞 GUI 线程 | 声明，非验收项 |

## 观察（非本任务范围，未处置）

主工作树外残留 worktree `build/wt-test-group001-repair`（detached `514d150`，GROUP-001 遗留），内含未跟踪文件 `project-docs/handoffs/req-ext-group-001-independent-test-2026-09-30.txt`（与 HEAD 版本不同）。因含未提交内容且属他人已完成任务面，本节点不删除，仅上报主人裁决。

## 诚实边界

本节点只做「三事实行核对 + 合并 + 合并态 pipeline」。e2e 套件（46/46、13/43/37/48）采信分支上的测试/审计节点独立重跑记录，本节点未重跑；合并态仅运行登记的 pipeline 三门。
