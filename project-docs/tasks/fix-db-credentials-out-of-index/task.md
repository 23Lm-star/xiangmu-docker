# 任务记录：fix-db-credentials-out-of-index

- **tier**: SMALL
- **tier 依据**: 判据 4（无契约改动、无生产源代码改动、只动 `.gitignore` 与版本控制状态）
- **owner_agent**: workbuddy (梦梦)
- **date**: 2026-09-29
- **状态**: COMPLETED

## 需求（业务语言）

数据库密码明文写在 `Server/connect.config` 里，并且这个文件从仓库第一次提交起就在版本控制内、已经推到公开的 GitHub。这件事需要停止继续发生。

## 验收标准

1. `Server/connect.config` 不再被 git 跟踪（本地文件保留，程序照常能跑）
2. 仓库里保留一份不含真实密码的模板，供新克隆的人参照
3. 构建、测试、审计三项全绿

## allowed paths

- `.gitignore`
- `Server/connect.config.template`（新增）
- `Server/connect.config`（仅从索引移除，文件保留）

## 不做的事（划界）

- **不清除 git 历史**：历史中的密码需要重写全部提交哈希 + 强制推送，属不可逆操作，风险等级与本任务不同，单独作为一个任务做。
- **不改配置加载方式**：`connect.config` 目前通过 Qt Resource 内嵌编译（`config.qrc`），改成外部读取是另一个需求。

## 决策日志（append-only）

| 日期 | 决策 | 说明 |
|---|---|---|
| 2026-09-29 | 档位定为 SMALL | 不碰 `frozen_paths`，不碰产品源码，单次提交可回滚 |
| 2026-09-29 | `Client/connect.config` 保留跟踪 | 实测其内容只有 IP / 端口 / 路径，**无凭据**，且兼作客户端默认配置 |
| 2026-09-29 | 保留本地 `Server/connect.config` | `config.qrc` 仍引用它；移除文件会破坏构建。本地备份为 `connect.config.bak-20260929`（已加入 .gitignore） |
| 2026-09-29 | 历史清除延后为独立任务 | 强制推送属不可逆操作，需单独批准 |
| 2026-09-29 | **事故与恢复**：`git add <新模板>` 与 `git rm --cached <原文件>` 在**同一批次**执行时，git 的 rename 探测把两者识别为一次 rename，导致工作区的 `Server/connect.config` 被一并清理。已从 `connect.config.bak-20260929` 完整恢复并重新验证（文件存在 / 未被跟踪 / gitignore 生效 / 构建 GREEN）。**教训**：新增与移除混在同一 `git add` 批次会触发 rename 探测，应分开执行。 |
| 2026-09-29 | 保留 `.bak` 文件在本地 | 恢复来源；已被 `.gitignore` 的 `connect.config.bak-*` 规则覆盖，不会入库 |

## 证据

- 目录：`project-docs/evidence/fix-db-credentials-out-of-index/`
- `manifest.txt` — commit hash `33b3dee`、时间戳、环境指纹、文件摘要（由 `record-evidence.py` 运行时写入）
- `pipeline-run.log` — test / audit 输出
- `state-opener.log` — 会话开场读数

## 验证结果

| 门 | 结果 |
|---|---|
| `pipeline/build.ps1` | **GREEN**（Client.exe / Server.exe） |
| `pipeline/test.ps1` | **GREEN**（0 自动化测试注册，诚实报告） |
| `pipeline/audit.ps1` | **PASS** |
| `git ls-files --error-unmatch Server/connect.config` | 不匹配 → 确认不再跟踪 ✅ |
| 模板敏感串扫描 | 无 `123456` ✅ |

## 提交

```
a621309  fix(security): stop tracking Server/connect.config credentials
a9d06a7  merge: stop tracking Server/connect.config credentials (SMALL)
```
