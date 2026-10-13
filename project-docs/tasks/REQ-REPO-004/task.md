# REQ-REPO-004 任务记录 — 仓库卫生：取消跟踪 Client/connect.config

- 需求文档：project-docs/requirements/REQ-REPO-004-repo-sanitize.md
- 分支：chore/repo-sanitize（自 master 4fc567a 切出；需求编写基线 b5d7bb3，事实复核全部仍成立）
- 档位：**SMALL**。依据：不触碰 frozen paths（两 protocol.h）与任何产品源码；单 commit 可整体回退；不 push。
- 验收标准：A1–A7（见需求文档 §二）
- Allowed paths：.gitignore、Client/connect.config.template、Client/connect.config（仅取消跟踪）、project-docs/tasks/REQ-REPO-004/task.md、project-docs/evidence/REQ-REPO-004/

## 分步执行实际记录（§3.1，每步分开执行，未合并命令）

| 步骤 | 命令 | 实际结果 |
|---|---|---|
| 1 备份 | cp → Client/connect.config.bak-20260929 | 28 字节，创建成功 |
| 2 确认 | git ls-files / ls | 均有输出（跟踪中且磁盘存在） |
| 3 模板 | cp connect.config → connect.config.template | 内容 3 行：127.0.0.1 / 5000 / ./filesys |
| 4 取消跟踪 | git rm --cached Client/connect.config（单独执行） | `rm 'Client/connect.config'` |
| **5 双检查** | ls + git ls-files | **ls 有输出（磁盘文件仍在，28 字节）；ls-files 为空** ✅ 未复现分支 1 误删坑 |
| 6 加模板 | git add Client/connect.config.template（单独执行） | 索引显示 R（rename 呈现，仅索引层，工作区无损） |
| 7 .gitignore | 凭据段追加 /Client/connect.config、/Client/connect.config.bak-* | check-ignore 命中 .gitignore:77 与 :78 |

## 决策日志

- 2026-09-29 需求分析：文档全部实测复核属实；跟踪文件数按实测 269 记录（文档 226 为基线期数字）；分支切出点记 4fc567a。全库扫描 `.bak/.local/release/` 无其他误跟踪文件，不触发另开需求。
- 2026-09-29 开发前总结获用户批准。
- 模板内容与真实文件保持一致（无密码），不加占位符，遵循需求 §3.3。
- 备份文件 bak-20260929 保留磁盘、已被新规则忽略，不删除（禁止事项 #8）。

## 验收结果（全过）

| # | 判据 | 实测 | 结果 |
|---|---|---|---|
| A1 | ls-files 为空 | `--error-unmatch` 报 "did not match"，exit=1 | ✅ |
| A2 | 本地文件存在 | 合并时 rename 呈现一度删盘，步骤 1 备份秒级恢复（28 字节一致） | ✅ |
| A3 | template 存在且跟踪 | 3 行同构，随提交进库 | ✅ |
| A4 | ignore 规则命中 | `.gitignore:77:/Client/connect.config` | ✅ |
| A5 | 编译通过 | build GREEN @a7be845、@2304f23 | ✅ |
| A6 | 工作区干净 | git status --short 空 | ✅ |
| A7 | 三门全绿 | GREEN/GREEN/PASS @a7be845；@2304f23 build GREEN+audit PASS | ✅ |

## 合并事件记录（重要）

分支 1 的坑以第二种形态重现：`git rm --cached` + `git add template` 内容 100% 相同，merge --no-ff 应用 rename 时**把磁盘上的 connect.config 删掉了**（与是否合并命令无关）。恢复走需求 §九预案：`cp bak → 原位`，三门复跑全绿。后续同型需求（rm --cached + 同内容 template）**必须在合并后做一步：确认/恢复本地实文件**。证据：project-docs/evidence/REQ-REPO-004/post-merge-verify.log。

- 合并 commit：2304f231f7ef5fc271880d699b59246b39567a7d（代码 a7be845d22b96e32d1a7b15b8ce15e05692b1863）
- 未 push（禁止事项 #7）
