# REQ-EXT-FILE-002 开发节点报告（2026-10-01）

- 分支 `codex/req-ext-file-002-fast-upload`，代码提交 `c452eb3`（基线 `master@1123a98`）。
- 档位 LARGE；本节点按 `design.md` §1–§4 落地，owner = claude-code，主工作树（唯一活动开发区，未另切 dev worktree）。

## 1. 交付内容（按设计节次）

| 设计节 | 落地物 |
|---|---|
| §1 协议 | 两端 `protocol.h` 追加 `UPLOADFILE_FASTCHECK_REQUEST=97/_RESPOND=98` + 请求/响应偏移与 magic 常量；旧编号 1–96 逐项未动 |
| §2 服务端 | `MsgHandler::uploadfilefastcheck()`：形态校验 → actor 根逻辑校验 → 请求级幂等短路 → 摘要预检查命中（零正文）→ 未命中登记暂存会话；`finalizeUploadOnComplete()` 增复算摘要对照；`OperateDB` 增 `lookupUploadSessionByRequestId/insertCompletedUploadSession/restartUploadSession`；`FileObjectStore` 增 `attachExistingObject/findObjectBySha`，`commitUpload(...,expectedSha)` |
| §2.1 判别收紧 | `isLogicKeyForm` 增两条否证（含 `':'`；首段等于 fs 根 basename）→ 收口 FILE-001 L1/L2 判别面 |
| §3 数据模型 | `migrate_ext_file_002.sql`（加 `client_request_id`/`file_id` + `uk_req`，幂等）+ 回滚脚本；`checkSchemaVersion` 并入新版本 |
| §4 客户端 | 上传入口流式算 SHA-256（1MiB 窗口）+ 32hex 请求键发 FASTCHECK；命中提示+刷新，未命中复用既有 `Uploader` |

## 2. 证据（哈希 `c452eb3`）

| 项目 | 结果 | 日志 |
|---|---|---|
| 构建（dist 形态） | GREEN | `run-logs/build.log` |
| pipeline/test.ps1 | GREEN-WARN（仓库注册 0 个自动化测试；本条不主张绿灯） | `run-logs/test.log` |
| pipeline/audit.ps1 | RED: `frozen:Client/protocol.h`（**预期签名**，见 §4.2 判读口径） | `run-logs/audit.log` |
| **新增验收 1–6** `fastupload_e2e.py` | **46/46 PASS**（dist 形态与 Server-cwd 形态各跑一次，多次重跑稳定） | `run-logs/fastupload_e2e-distmode.log`、`run-logs/fastupload_e2e-servercwd.log` |
| FILE-001 回归 `dedup_e2e` | 13/13 PASS | `run-logs/dedup_e2e.log` |
| FILE-001 回归 `entry_ops_e2e` | 43/43 PASS | `run-logs/entry_ops_e2e.log` |
| FILE-001 回归 `cross_tree_e2e` | 37/37 PASS | `run-logs/cross_tree_e2e.log` |
| FILE-001 回归 `sec001_regression` | 48/48 PASS | `run-logs/sec001_regression.log` |

验收映射：A1 命中零正文 + 条目立即可读（分块 PDU 计数 0、flushfile 立即可见）；A2 单物理对象 + `(1)` 递推稳定；
A3 伪造摘要不命中、复算不符不转正/不建条目 + 会话 failed + staging 清零、跨用户拒绝、未认证拒绝响应类型正确、失败会话同请求键可重试且仅一条会话行；
A4 两账号并发同内容 → 单对象 refcount=2 两条目；A5 中断续传（同键回同一会话与偏移）、完成态重放幂等、清理不动已完成对象与条目；
A6 四套件全绿 + 客户端 SHA 分块流式（静态断言）。

## 3. 运行环境（复现要点）

- MySQL 5.7 本机：`mydb`；先执行 `Server/migrate_ext_file_002.sql`（幂等，已实测重放/回滚/再应用）。
- 构建：`pipeline/build.ps1`（Release 产物落 `dist/`）。
- 现行 `Server/connect.config`：`filesPath = E:/qt/xiangmu/dist/filesys`（绝对路径，部署策略），
  故 Server.exe 需以 `dist/` 为工作目录启动（相对 `./filesys` 的 PDU 路径才与 filesPath 同一棵树）。
- 未入库依赖：`dist/libmysql.dll`、`dist/sqldrivers/qsqlmysql.dll`、`dist/platforms/` 已就位。
- Python 用 `py -3.13`。

## 4. 判读口径与偏差（供独立测试/审计节点）

### 4.1 与批准契约文本的偏差（1 项，需主人知悉，非范围扩张）

- **schema_version 取 `2026100102` 而非设计 §3 写的 `2026100101`**。原因：`2026100101` 已被
  REQ-EXT-GROUP-002 占用（`kGroupMessageSchemaVersion = 2026100101`）。同名会使"只跑了 GROUP-002 迁移、
  未跑 FILE-002 迁移"的库通过启动 fail-fast 检查，而运行时缺列必炸。仍属批准范围（"新增版本号并入 required 计算"）。
  设计 `design.md:87` 与 `task.md:54` 的数值需随本报告更正。

### 4.2 冻结面判读（design §5 口径）

`audit.ps1` 报 `frozen:Client/protocol.h` 是本分支的**预期签名**（改动正是 protocol.h）。判读按批准清单逐项比对：
① 只追加不重编号（旧编号 1–96 值不变，隐式序号解析断言）② 两端常量块逐字节一致（md5 相同，1352B）
③ 无既有 PDU 语义变更。三项由 `fastupload_e2e.py` 的 P1/P2/P3 断言客观给出（见日志）。

### 4.3 待独立测试/审计确认的既有面问题（本节点发现，未在分支内修）

| 编号 | 事实 | 证据 | 建议 |
|---|---|---|---|
| FINDING-DEV-1 | `pipeline/audit.ps1` 解析 `project-manifest.yaml` 用 `Get-Content`（PS5.1 默认 ANSI），中文注释被破坏后把两行冻结项并成一行，**只取到第一个 `Client/protocol.h`，`Server/protocol.h` 未被冻结检查覆盖** | 同一文件按 UTF-8 解析得两项；PS 循环实测只得一项（见本报告 §4.2 附注） | 另开 SMALL 任务修 pipeline 脚本（脚本属仓库契约，不在本分支改） |
| FINDING-DEV-2 | 四套回归脚本依赖部署前的运行形态（`filesPath=./filesys` + `cwd=Server` + exe 在 `build/build-Server/release/`），与现行绝对路径配置不一致 | 现行 `dist/filesys` 树缺 fixture 用户目录，直接跑必红 | 本次以临时相对配置复现其原形态跑绿并已还原；建议另开 SMALL 任务统一运行形态 |
| FINDING-DEV-3 | L1 只在本分支收口了"判别"面（`isLogicKeyForm`），`normalizeLogicKeys` 的两条 SQL 选择谓词仍把 `/filesys/<owner>/…` 当 canonical，**存量此类行不会被转换**（数据面未收口） | 谓词 `parent_path NOT LIKE '/%' OR …` 实测不含该形态；当前库 `SELECT DISTINCT parent_path FROM user_file` 无该类行 | 若日后出现该类行，另开 SMALL 任务补谓词 |
| FINDING-DEV-4 | 验收 5 的"过期临时会话清理"在代码库中**本无调度器**（FILE-001 亦无）。本节点不新增清理器，改为验证不变式：删过期/失败会话行 + 删孤儿 `.part` 后，`file_object`/`user_file` 计数与 `ready` 状态不变 | `fastupload_e2e.py` A5-cleanup-safe / A5-cleanup-entry-alive | 若产品要求定期清理，另立需求 |
| FINDING-DEV-5 | `MKDIR` 非幂等（目录已存在时回 `ret=0`）；用户根仅在"首次注册"时创建 | A2 断言不得用 MKDIR 返回值；夹具需显式建目录 | 既有行为，仅记录 |

## 5. 下一节点

独立测试（新会话、建议不同 agent）：干净 detached worktree @ `c452eb3`，独立重跑
`fastupload_e2e.py` + FILE-001 四套件 + pipeline 三门，并复核 §4.1–§4.3。测试节点按 `task.md` L6 沿用 `node: test`。
