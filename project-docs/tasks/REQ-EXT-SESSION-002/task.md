# REQ-EXT-SESSION-002 启动期在线态对账 — 任务记录

## 档位
**MEDIUM**。理由：改了生产启动行为（新增一步对账，且失败即拒绝启动）——有界、单提交可回滚；
不触协议（未碰 `protocol.h`）、不触数据迁移、不触安全/系统边界。无 LARGE 硬触发。

## 来源
2026-10-12 全功能审计缺陷 ③，`build/audit-20261012/probe_abnormal.py` 修复前 **7/7** 复现。

## 现象
服务端**异常终止**（kill / 崩溃 / 断电）时，当时在线的用户在 DB 里永久留下 `user_info.online = 1`。
只有该用户自己重新登录再断开才会自愈。后果：在线用户列表与用户信息查询把幽灵当在线返回；
"添加好友"依赖目标 `online=1`，会给幽灵转发请求（无接收者、静默丢弃）。

## 根因（代码锚点）
- 全库**唯一**把 online 置 0 的地方是 `Server/operatedb.cpp` 的 `handleOffLine`（`UPDATE user_info SET online = 0`），
  它只在**连接断开**时按人调用。异常终止没有断开事件 → 谁也走不到它。
- 启动路径 `Server/main.cpp`：`initialize()`（加载配置 + 监听）→ `OperateDB::connect()` →
  `checkSchemaVersion()` → `normalizeLogicKeys()` → `migrateLegacyFiles()` → `ReclaimScheduler::start()` → `a.exec()`，
  全程**没有**任何在线态对账。
- `project-docs/init_test_data.sql` 把 `tester`、`friend1` 直接插成 `online = 1` —— 装好库就自带幽灵。

## 设计裁决（design）
| # | 问题 | 裁决 | 理由 |
|---|---|---|---|
| D1 | 全量归零还是精细判活 | **全量归零** `UPDATE user_info SET online = 0 WHERE online <> 0` | 启动瞬间本进程一条连接都没有，任何 `online=1` 都必然是上个进程的残留；"断开即归零"由 handleOffLine 保证，故清零不会冤枉真实会话 |
| D2 | 多实例怎么办 | **写明单实例假设**，不实现租约 | 本项目单进程单端口单库；多实例共享一库时全量清零会误清另一实例的在线用户——这条假设写进代码注释与本节，将来必须改成"按实例/租约"判活 |
| D3 | 对账失败 | **fail-fast 退出** | 与 `checkSchemaVersion` 同一立场：库不可写时登录/注册同样会失败，带着幽灵启动只是把问题藏起来 |
| D4 | 放在哪一步 | `main` 里 `checkSchemaVersion()` 之后 | 必须在 DB 可用之后；必须在 `a.exec()` 之前（那时才有连接）；与既有的一次性启动维护（normalizeLogicKeys/migrateLegacyFiles）同族 |
| D5 | `init_test_data.sql` 的种子 | **不改** | 修复后启动即归零，种子里的 `online=1` 不再能变成幽灵；且已核验**没有任何套件依赖** tester/friend1 处于在线态。动一个"运行必需"的仓库资产收益为零、风险非零 |
| D6 | 要不要周期对账 | **不做** | 运行期幽灵由 handleOffLine + 会话登记表覆盖；周期性全量清零会误伤并发登录中的真实会话，属超范围 |

## 改动
- `Server/operatedb.h` / `.cpp`：新增 `OperateDB::reconcileOnlineFlags()`（先 COUNT 再 UPDATE，无残留走 debug 分支、有残留打一次 warning 带条数）。
- `Server/main.cpp`：`checkSchemaVersion()` 之后调用，失败 `return 1`。
- `project-docs/evidence/REQ-EXT-SESSION-001/reconcile_online_e2e.py`（新增）：回归套件 13 断言。
- `project-docs/project-manifest.yaml`：注册 `reconcile-online`（16 套 552 → **17 套 565** 断言）。

## 验收
- 自动：`reconcile-online` 13/13；全量门禁 17 套 / 565 断言。
- 反向对照（真跑两个二进制）：`probe_abnormal.py` 在 master 二进制上 **7/7**（幽灵在），
  在候选二进制上 **5/7**（G5 在线列表不再返回幽灵、G7 不再需要本人自愈）；
  G3 仍 PASS 属正常——它描述的是"kill 当时库里还是 1"，对账发生在下一次启动。
- 无需 GUI：缺陷链全在服务端（socket + MySQL），套件发的 PDU 与 GUI 按钮同构，本次未改客户端一行。

## 证据
`build/evidence/current/{build,test,audit}-1c8ea2e…-run-20261013-22.*`
+ `build/evidence/merge-3797dfd/probe-{BEFORE,AFTER}.txt`。
