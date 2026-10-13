# REQ-EXT-GROUP-003 证据索引

## integration / merge 节点（2026-10-01，合并结果 495e552）

目录 `integration/`：`merge-facts.txt`（三事实行原始核验：报告已接受 / 冻结面零越批准改动 /
合并结果 pipeline GREEN）、`audit-merged.txt`（合并提交上 `audit.ps1` 原始输出 =
`audit PASS`，因冻结面检查已按分支范围生效，合并即历史不重复命中）、
`build-495e552-*.log`、`test-495e552-*.log`、`manifest-{build,test}-495e552-*`。
合并：`codex/req-ext-group-003-group-files@9129900` → `master`，no-ff（`495e552`，父 `285b4b0` + `9129900`），无冲突。
分支层冻结命中证据见 `independent-test/pipeline-audit.log`（`audit RED: frozen:Client/protocol.h, frozen:Server/protocol.h`）。

## independent audit 节点（2026-10-01，审计对象 0b83aab）

目录 `audit/`：`report.md`（结论 **PASS**，0 条阻塞缺陷、6 条非阻塞观察；冻结面 99–106 独立复证仅追加且两端逐字节一致；
调用路径源码直读替代不可用 codegraph 索引）、`independent-verification.log`。
审计者 claude-code（新会话、只读 detached worktree），与 development owner 同名——**跨代理未满足**，报告已如实记录，
经主人裁决接受（`audit: accepted by user, 2026-10-01`）。

## independent test 节点（2026-10-01，候选 0b83aab）

目录 `independent-test/`：`report.md`（结论/矩阵/独立核验）、运行日志
`groupfile_e2e-run.log`、五个既有套件日志 `{dedup,entry_ops,cross_tree,sec001_regression,fastupload}_e2e.log`、
`group_lifecycle_regress.log`、pipeline 三日志 `pipeline-{build,test,audit}.log`。
结论：**PASS**，0 条候选归因缺陷；`groupfile_e2e.py` **45/45**；既有回归 13/43/37/48/46 + 群生命周期 **15/15**；
独立追加性核验通过（两端 `protocol.h` 仅追加 99–106，无删除行）。audit RED 为预期冻结面命中。

## development 节点（2026-10-01，候选 0b83aab）

| 文件 | 节点 | 结论/内容 | 锚定哈希 |
|---|---|---|---|
| `groupfile_e2e.py` | development | 验收 1–6 端到端 harness（45 用例：协议静态/列表/上传复用/失权/删除隔离/负向面） | 0b83aab |
| `project-docs/tasks/REQ-EXT-GROUP-003/design.md` | design | PDU 99–106 字段/偏移、`group_file` DDL、权限判定点、验收 1–6 手段、允许路径 | — |
| `project-docs/tasks/REQ-EXT-GROUP-003/task.md` | goal/development | 档位/依赖核查/契约影响/门次事件 | — |

复现步骤见 `independent-test/report.md` §7。
