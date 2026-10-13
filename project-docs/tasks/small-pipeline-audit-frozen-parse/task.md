# 任务记录：small/pipeline-audit-frozen-parse（F1 + F2）

- **tier**: SMALL
- **tier 依据**: pipeline 脚本属仓库契约，按 v1.7 pipeline.md 规则"经 SMALL 任务带独立提交修改"；不碰产品源码、不碰 `frozen_paths`、单提交可回滚、不推送。
- **owner_agent**: claude-code
- **date**: 2026-10-01
- **状态**: COMPLETED
- **来源**: REQ-EXT-FILE-002 独立审计 `§3 F1`（medium，事实，CONFIRMED）+ `§3 F2`（信息）。

## 需求（业务语言）

冻结面（`Client/protocol.h` / `Server/protocol.h`）是必须经主人批准才能改的界线，机器门 `pipeline/audit.ps1` 负责拦。但在日常开发区（主工作树）里，这个门实际上只看得到两个文件中的一个——`Server/protocol.h` 的改动可以不经提示直接通过。原因是读清单文件时用了系统默认编码，清单里一行中文注释的行尾字节把换行"吃掉"，两行并成一行。要修的是这个"看得见/看不见"的问题，不是放宽门本身。

## 验收标准

1. 主工作树（清单为 LF）下，冻结清单解析出 **2** 项（`Client/protocol.h`、`Server/protocol.h`）。
2. 构造一个同时改动两个冻结文件的分支，机器门报出 **2 个** `FROZEN-PATH HIT`。
3. 只改解析，不放宽门：解析修复后，命中判据（前缀匹配 + 命中即 RED）与改前一致。
4. F2 二选一并写明结论：让影响面有意义，或显式记为 `NOT_APPLICABLE` 并说明原因。
5. 分支上 `build.ps1` / `test.ps1` / `audit.ps1` 三项与改前同等（build GREEN、test 诚实 0、audit PASS）。

## allowed paths

- `pipeline/audit.ps1`
- `project-docs/project-manifest.yaml`（新增 `code_intel.impact_status` / `impact_reason` 两个声明键）
- `project-docs/tasks/small-pipeline-audit-frozen-parse/`、`project-docs/evidence/small-pipeline-audit-frozen-parse/`、`project-docs/handoffs/`

## 不做的事（划界）

- **不放宽门**：命中判据、RED 语义、分支作用域（merge-base..HEAD）零改动。
- **不改冻结面**：`Client/protocol.h` / `Server/protocol.h` 内容零改动。验证用临时分支 `scratch/t1-frozen-probe` 仅用于取证，已删除，永不合并。
- **不给 F2 造一个假 PASS**：不配置 test glob——本仓库无静态可达自动化测试，配置后的空结果会被脚本判为 PASS，属伪造通过。

## 技术结论

| 项 | 结论 | 依据 |
|---|---|---|
| F1 | **FIXED** | 改前主工作树解析 = 1（丢 `Server/protocol.h`）；改后 = 2；含双冻结改动的分支报 2 个 HIT |
| F2 | **NOT_APPLICABLE** | 仓库 0 个静态可达自动化测试；回归由 `project-docs/evidence/**` 下的 e2e 套接字脚本承担，`codegraph affected` 对其无语义。manifest 显式声明，审计输出由 `NOT_CHECKED`（易被误读为"还没查"）改为 `NOT_APPLICABLE (原因)` |

## 决策日志（append-only）

| 日期 | 决策 | 说明 |
|---|---|---|
| 2026-10-01 | 档位 SMALL | pipeline 脚本契约改，无产品源码、无契约面、单提交可回滚 |
| 2026-10-01 | F1 修法取 `Get-Content -Raw -Encoding UTF8` + 按行分割 | 审计 §3 拟修法；解码先于分行，全角 `）`(U+FF09) 不再吞 `0x0A`。未对含中文注释的行做单行正则 |
| 2026-10-01 | F1 证据用"临时分支 + 双冻结改动"取证后删除 | handoff 硬约束要求报 2 个 HIT；该分支 `scratch/t1-frozen-probe`（d0d2dd8）仅取证，未推送、未合并、已 `-D` 删除 |
| 2026-10-01 | F2 判 `NOT_APPLICABLE` 而非配置 test glob | 配置 glob 会把脚本的空结果判为 PASS（`test_glob` 非空即不再回落 `NOT_CHECKED`）——等于伪造通过，违反 no-forged-pass |
| 2026-10-01 | F2 落到 manifest 声明键，由 audit.ps1 读取 | 声明式、单一事实源；避免每次运行重复追问"为什么没查" |

## 验证结果

| 门 | 结果 |
|---|---|
| 主工作树冻结解析计数 | 改前 **1** → 改后 **2**（`parse-before.txt` / `parse-after.txt`） |
| 含双冻结改动分支 | **2 个 `FROZEN-PATH HIT`**（`audit-frozen-branch.log`） |
| 本分支 `audit.ps1` | **PASS**（`audit-branch.log`；`impact-check: NOT_APPLICABLE (…%)`） |
| `pipeline/build.ps1` | **GREEN**（`build-360baeb.log`） |
| `pipeline/test.ps1` | **GREEN-WARN**（0 自动化测试注册，诚实报告；`test-360baeb.log`） |
| 清单 EOL | 保持 LF（`git ls-files --eol` = `i/lf w/lf`），未引入 CRLF 抖动 |

## 提交

- 代码：`fix(pipeline): explicit UTF-8 frozen-path parse + declared impact-check non-result`
- 文档：本记录 + 证据归档 + handoff
- 合并：`merge: small/pipeline-audit-frozen-parse — …`（no-ff）
