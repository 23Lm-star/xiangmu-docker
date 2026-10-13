# REQ-EXT-FILE-002 独立审计报告（2026-10-01）

- 分支 `codex/req-ext-file-002-fast-upload`；候选 `c452eb3`（基线 `1123a98`）；detached 只读快照。
- 输入：pipeline 报告（audit.ps1 / test.ps1）+ `git diff 1123a98..c452eb3` + codegraph 影响闭包。
- 未改任何 `Client/` `Server/` `tests/` `tools/` `pipeline/` 文件；写入仅本报告与 handoff。

## 0 独立性声明（限制，须主人知悉）
goal/design/dev/test 与本次审计 owner_agent 同为 `claude-code`（使用者直接指派）。**未满足技能默认的跨代理审计**；独立性仅来自：全新会话上下文 + 只读 + hash 固定快照。如需严格跨代理，另派会话重审。

## 1 结论
**PASS（审计）**：批准契约被精确落地；冻结面三项独立复核通过；**0 条归因产品缺陷**。
**1 项仓库契约缺陷（事实）**：F1 — `pipeline/audit.ps1` 冻结面解析不稳健，见 §3；属独立 SMALL 任务，不阻塞本分支。

## 2 冻结面独立复核（design §5 口径，逐项、非采信 harness）
| 项 | 判据 | 独立结果 |
|---|---|---|
| ① 只追加不重编号 | 两侧 diff 新增仅 97/98；删除行仅 `// 97-99 继续预留` 一处注释 | **PASS** |
| ② 两端常量块逐字节一致 | 21 行 FASTCHECK 常量；两端 raw md5 均 `fefc4be1805b56bd4b19efe05a81946e` | **PASS** |
| ③ 无既有 PDU 语义变更 | 两侧仅追加；旧编号 1–96 值未动 | **PASS** |

方法：`git show c452eb3:{Client,Server}/protocol.h` 提取后逐行/逐字节比对（本审计自算，不引用测试节点结论）。

## 3 机器门复核：`audit.ps1`（主工作树，`powershell` PS 5.1.26100，ACP=936）
```
FROZEN-PATH HIT: Client/protocol.h -> escalate to LARGE + user approval
impact-check: NOT_CHECKED (no test glob configured; affected test set is not meaningful)
audit RED: frozen:Client/protocol.h
```

### F1 — 冻结检查对 `Server/protocol.h` 静默失明（severity: medium；事实；root_cause_confidence: CONFIRMED）
- **机理（已定位）**：`project-docs/project-manifest.yaml` 为 **LF**；`frozen_paths` 首项注释以全角 `）`（U+FF09 = 字节 `EF BC 89`）结尾，ACP936 下 `0x89` 作前导字节**吞掉后继 `0x0A`**，两行并作一行，正则只取到第一项 `Client/protocol.h`。
- **环境依赖（已证）**：`core.autocrlf=true` 且无 `.gitattributes`。
  - **全新 checkout**（`git checkout-index` / `git worktree add`）→ 该文件工作树为 **CRLF** → 不吞换行 → **两项都解析出**（＝测试节点所见、记两个 HIT）。
  - **主工作树**该文件为 **LF** → 吞换行 → **只一项**（＝dev 的 `run-logs/audit.log` 与本审计所见）。
- **判定**：**FINDING-DEV-1 = 事实成立**。测试节点「不可复现」**不成立**——两边是同一文件在不同 EOL 状态下运行，非环境异常；其「dev 环境不符」属误判。dev/测试应据此对齐结论。
- **影响**：日常开发区（主工作树）下 frozen 门不覆盖 `Server/protocol.h`。本分支未受实质影响（冻结面已按 §2 人工三项复核通过），但**将来未获批的 `Server/protocol.h` 改动可绕过门**。
- **处置**：`pipeline/audit.ps1` 属仓库契约，不在本分支改（正确）。**另开 SMALL 任务**：manifest 解析用 `Get-Content -Raw` + `-Encoding UTF8`（或先按 UTF-8 读字节再解析），勿对含中文注释的行做单行正则。

### F2 — 脚本层影响门未生效（信息）
`impact-check: NOT_CHECKED`（no test glob）。脚本影响面为空跑；本报告以 codegraph 手工闭包替代（§4）。

## 4 影响闭包（codegraph MCP，手工）
- `finalizeUploadOnComplete`（Server/msghandler.cpp:1442）— 3 callers（mytcpsocket.cpp, msghandler.cpp）
- `uploadfilefastcheck` — 2 callers（Server/mytcpsocket.cpp, Client/client.cpp）
- `commitUpload`（fileobjectstore.cpp:218）— 2 callers（operatedb.cpp, msghandler.cpp）
- `attachExistingObject`（fileobjectstore.cpp:726）— 2 callers（fileobjectstore.cpp, msghandler.cpp）
- 仓库注册 0 自动化测试 → 「no tests found」为预期；回归由 e2e 脚本承担。
- 逐阅读并核对：`uploadfilefastcheck`、`commitUpload(expectedSha)`、`attachExistingObject`、`findObjectBySha`、`lookupUploadSessionByRequestId/restartUploadSession/insertCompletedUploadSession`、`isLogicKeyForm`、客户端 `on_upload_PB_clicked`。
- 关键正确性点（均事实）：形态校验含 `total == uiMsgLen` + 长度上下限；sha 严格 `[0-9a-f]`；目录经 `logicPathInActorRoot` + `normalizeParent` 双层约束；命中路径 `attachExistingObject` 单事务；转正复算**大小+摘要**双对照；幂等短路 `FOR UPDATE`；并发由 `uk_sha256`/`uk_req` 兜底；`mytcpsocket.cpp` 新 PDU 同时登记 `handlMsg` 分派**与** `authRejectPdu` 映射（design §1.4 易漏点已覆盖）。

## 5 schema_version 偏差复核（§4.1）
- 事实：`operatedb.h:22 kGroupMessageSchemaVersion=2026100101`、`:26 kFastUploadSchemaVersion=2026100102`；`operatedb.cpp:96-98 required=qMax(四项)`；`migrate_ext_file_002.sql:42` 插 `2026100102`；rollback 删 `2026100102`。
- 判定：**偏差合理、属批准范围**（"新增版本号并入 required 计算"）；"同名会使 fail-fast 失效"论证成立。`design.md` 已带更正说明；`task.md` 批准行保留历史原文（append-only），偏差另记事件行——**无需改动**。severity: low。

## 6 既有 FINDING 复核
| 编号 | 审计判定 |
|---|---|
| FINDING-DEV-1 | **推翻测试节点**：事实成立（§3 F1）。建议 dev/测试对齐运行环境后更正其「不可复现」结论。 |
| FINDING-DEV-2 | 确认（运行形态差异，非产品缺陷）。 |
| FINDING-DEV-3 | 确认（数据面 `normalizeLogicKeys` 谓词未收口；当前库 `LIKE '/filesys/%'` 计数=0，潜伏未触发）。符合 `task.md`「超出函数局部即降级独立 SMALL」预案，非本分支缺陷。 |
| FINDING-DEV-4 | 确认（无清理调度器；改为不变式验证）。 |
| FINDING-DEV-5 | 确认（`QDir::mkdir` 非幂等，既有行为）。 |

## 7 新增发现台账
| ID | severity | 性质 | 归属 |
|---|---|---|---|
| F1 | medium | 事实（CONFIRMED） | `pipeline/audit.ps1` → 独立 SMALL |
| F5 | low | 声明（claim，未验证影响） | 客户端对整文件**同步**算 SHA-256：内存 O(1) 满足（验收6），但大文件下阻塞 GUI 线程。design §4 已选此形，非验收项——仅记录。 |

## 8 停止条件（design §7）
协议兼容性、服务端复算、对象唯一性三项均有客观证据（测试节点独立重跑 + 本审计逐阅读），**未触发停止条件**。

## 9 诚实边界
本轮**未**重跑构建 / `test.ps1` / fastupload 46/46 / FILE-001 四套件（超出审计节点输入范围：pipeline 报告 + diff + 闭包）。上述结果采信测试节点的独立重跑记录（dist×2 + Server-cwd×2 稳定），本审计未对其推翻。

## 10 归因候选缺陷
**0 条**。本分支无归因产品缺陷；唯一新增缺陷 F1 属仓库契约（audit.ps1），非本分支交付物。
