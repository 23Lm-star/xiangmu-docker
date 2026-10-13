# REQ-EXT-RECLAIM-001 — 周期清理调度器（延迟删除的收割者）

## 这个需求解决什么问题

D5 之后"删"被有意推迟了：引用归零只把对象标成 `pending_reclaim`（留复活窗口给秒传），
上传失败/中止的 `.part` 有意保留待续传。两处机制各自正确，合起来留下**两个无界增长点**——
废弃暂存文件与 `pending_reclaim` 积压。东西不会自己消失，所以需要一个只负责"收"的角色。

硬约束：**不得影响正常业务**。三条红线见 `Server/reclaimscheduler.h` 顶部的注释：
主线程有界切片、候选走索引不扫全表、删除权=一次条件状态迁移（与复活在同一行上串行化）。

## 一轮的形状（复述用）

两条收割线共用同一形状：**取候选 → 抢权 → 落地**。

- **A. 待回收对象**：抢权(`status='reclaiming'`) → unlink → 同一事务内摘悬挂引用 + 删对象行。
  unlink 失败（Windows 上被下载句柄占着）→ 回退 `pending_reclaim`，本轮不再碰它，下轮重试。
- **B. 废弃暂存**：超 TTL 且无 uploading/verifying 会话引用 → 直接 unlink。
  无跨资源不变量，所以不需要抢权。

判据（都必须"且"）：

- 暂存：① mtime 超过 `staging_ttl_hours` ② 不被任何活跃上传会话引用
- 对象：① `status='pending_reclaim'` ② `refcount<=0` ③ `reclaim_at` 超过 `grace_hours`
  ④ **表上确实没有 active 引用**（`user_file`/`group_file` 两表 `NOT EXISTS`）

第 ④ 条是刻意加的：`refcount` 只是提示，真相从表里现算。删除路径上宁可多查一次，
也不能因为一个计数 bug 把还有活引用的对象当垃圾。

## 三个关键抉择（面试问答用）

**1. 为什么删除权要状态机，而不是"先查再删"？**
"先查再删"在查与删之间有窗口：判定"没有活跃引用"之后、删除之前，别人可能刚好复活了它。
改成两侧都是同一行上的条件 UPDATE——调度器 `SET status='reclaiming' WHERE ... AND
status='pending_reclaim'`，复活方 `SET status='ready' WHERE ... AND status IN
('ready','pending_reclaim')`——由 InnoDB 行锁串行化，**必有一方影响行数为 0**。
赢的删，输的让位（`lostRace` 分支），不靠时序约定，所以也不需要"读时兜底"的补丁。

**2. 为什么宽限期锚点另起一列 `reclaim_at`，不用 `updated_at`？**
`updated_at ON UPDATE CURRENT_TIMESTAMP` 会被**抢权和回退自动刷新**，等于每次重试都把
宽限期重新计时——一个 unlink 一直失败的对象会变成"每过一个宽限期才重试一次"，
永远等不到下一轮。`reclaim_at` 只在引用归零时写一次，之后的抢权/回退都不碰它。

**3. 为什么必须新开线程？——恰恰相反，必须不新开线程。**
清理的阻塞量很小：候选路径就在 `file_object.storage_path` 里，**根本不用遍历对象目录**；
`.staging` 是单个扁平目录，列一次毫秒级；unlink 每次亚毫秒。新开线程反而要引入跨线程访问
全局 DB 连接池、第二个 `user_file`/`file_object` 写入者、新的关闭顺序风险——用更大的风险
换更小的收益。挂主线程 QTimer、每拍只动 `per_tick` 条即可，主线程最坏停顿因此可控。
额外白拿一道保护：Windows 上下载流持有句柄时 unlink 必然失败，**正在被下载的对象天然排除**。

## 顺带修掉的一个硬阻塞（设计方案里没有）

`file_object` 上有两条外键：`fk_uf_obj`(`user_file.object_id`)、`fk_gf_obj`(`group_file.object_id`)。
软删条目与已移除群文件都**保留** `object_id`，于是"引用归零"之后外键仍然钉着那一行，
`DELETE FROM file_object` 必然报 **1451**。
`user_file.object_id` 本就可空，摘掉悬挂引用只需一条 UPDATE；
但 `group_file.object_id` 是 `NOT NULL`，摘不掉 → **群文件用过的对象永远回收不了**。
所以迁移里把它放宽为可空，删除事务内摘掉非 active 引用
（所有存量读路径都带 `status='active'` 过滤，`releaseGroupFile` 早已写了 `if (!obj.isNull())`
判空分支，说明这个方向本来就是预期内的）。

## 交付物

| 文件 | 作用 |
|---|---|
| `Server/reclaimscheduler.h/.cpp` | 调度器本体（两条收割线：待回收对象 / 废弃暂存） |
| `Server/reclaim.cfg` | 配置权威副本（运行期从 `Server.exe` 同目录读取，缺失则回落保守默认） |
| `Server/migrate_ext_reclaim_001.sql`（+ `_rollback.sql`） | schema 迁移，版本号 2026100701 |
| `Server/fileobjectstore.cpp` | 4 处标记点写 `reclaim_at`；5 处复活点加状态谓词 + 影响行数校验 |
| `Server/main.cpp` | `start()` 挂到事件循环；退出前显式 `stop()` |
| `Server/operatedb.{h,cpp}` | `kReclaimSchemaVersion` 纳入 fail-fast 启动检查 |

## 验证（本目录）

`py -3.13 project-docs/evidence/REQ-EXT-RECLAIM-001/reclaim_e2e.py` —— 自管理 Server 生命周期
（经 `_harness/server_ctl.py` 按 PID 结束自己启动的进程，不用 `taskkill /IM`），
自己写好 `dist/reclaim.cfg` 并在收尾还原现场。输出见 `run-log-20261007.log`。

实跑结果 **18/18 PASS**（2026-10-07，保留期改按小时后的复跑）：
`dry_run=1` 的 phase 1 仍证明"只报候选、库与盘分毫未变"，phase 2 的 A–I 九场景全部成立。

phase 1（`dry_run=1`）：候选只报不动，库与盘**分毫未变**——
P1a 夹具 5 行完好 / P1b `999904` 仍 `reclaiming` / P1c `999901` 文件仍在 / P1d 过期暂存仍在。

phase 2（`dry_run=0`）：

| # | 场景 | 期望 | 实际 |
|---|---|---|---|
| A | 到期 + 无活跃引用 | 行与文件都删 | ✅ A1 A2 |
| B | 到期 + 但有 active 引用 | 一条都不许动 | ✅ B1 B2 B3 |
| C | 未到宽限期 | 不许动 | ✅ C1 C2 |
| D | 崩溃残留 `reclaiming`，文件已无 | 补完删记录 | ✅ D1 |
| E | 崩溃残留 `reclaiming`，文件仍在 | 退回 `pending_reclaim` | ✅ E1 E2 |
| F | 暂存过期且无会话引用 | 删除 | ✅ |
| G | 暂存未过期 | 保留 | ✅ |
| H | 暂存过期但 uploading 会话在用 | 保留 | ✅ |
| I | 夹具之外既有的 `.part` 计数 | 同一次运行前后不变 | ✅ |

断言 I 的语义要读准：它是**同一次运行的前后比较**，证明"本轮只动了该动的"。
它测不到"在本轮开始之前就已被别的运行收走"——那不是它的职责。所以它比较的那个数
（当天 21）只是当时 `dist/filesys/.staging` 里上一轮套件留下的残渣数量，**不代表"未过期"**；
见下节。

未由实跑覆盖、只由构造保证的一条：**抢权 vs 复活的真并发竞态**。两侧都是同一行上的条件
UPDATE（`rows==1` 才算赢），由 InnoDB 行锁串行化，必有一方影响 0 行；`lostRace` 分支即让位
路径。要实跑复现需要精确卡在两个 UPDATE 之间，夹具成本高于其证明力，故此处如实标注为
"构造保证"而非"实跑证明"。

## 保留期单位：小时（默认 6 小时）

两个保留期都按**小时**配：`staging_ttl_hours=6`、`grace_hours=6`（对应代码里
`Config::stagingTtlHours` / `graceHours`，SQL 用 `INTERVAL ? HOUR`，mtime 比较用
`hours * 3600_000`）。原因很实际：本项目里被收走的都是可再生的临时物，7 天 / 30 天
的窗口只把"观察一次真实回收"变成"等一周 / 等一个月"。**6 小时 = 观察一轮的最长等待**。
要按天用就写倍数（1 天=24、7 天=168、30 天=720），单位不换、判定不变。

代价要说清：`staging_ttl_hours` 同时是**续传现场**的保留期，6 小时意味着超过 6 小时
不来续传的 `.part` 会被收走（原设计是 7 天）；`grace_hours` 同时是**秒传复活窗口**，
6 小时意味着引用归零 6 小时后同内容再上传就不再命中（原设计是 30 天）。
这是纯运营取舍，判定与互斥逻辑一行没动。

改小时时暴露出的一个**顺序交互**（值得记住）：`recoverStaleClaims()` 在每轮开头跑，
它把崩溃残留的 `reclaiming` 退回 `pending_reclaim`，**紧接着同一轮**就去取候选。
所以"退回"并不等于"保住"——若该对象的 `reclaim_at` 已超宽限期，它退回后当轮就被正常收走。
这是对的（宽限期是从最后一次引用消失起算，不是从崩溃恢复起算），但断言要按它设计：
夹具里 E 例（要验"退回并保留"）的 `reclaim_at` 必须**在宽限期内**，否则它会顺路被删。
第一版夹具正是踩了这个（E 用了 40 小时 > 6 小时），E1/E2 直接 FAIL——**是夹具的判据错了，
不是产品错了**，改成 1 小时后 18/18。

## 发布态默认行为：无配置也真删（实测）

`loadConfig()` 找不到 `reclaim.cfg` 时回落的 `Config c` 就是 `enabled=true, dryRun=false`，
即**"没有配置文件"等于"立刻真删"**。受控实验（2026-10-07）：移走 `dist/reclaim.cfg`，
在 `.staging` 放两个探针（10 小时前 / 刚刚），启动 `dist/Server.exe` 4 秒 ——
**10 小时前的被收走、刚刚的保留**。判定正确（符合 TTL），但有两个必须知道的后效：

1. **每次启动都会跑一轮真清理**（`m_lastRoundStartMs` 初值 0 → 第一个心跳即满足间隔）。
   本仓库的 e2e 套件每套都启动自己的 `Server.exe`，于是**任何套件启动都会对共享运行树
   做一次真实回收**。这在本次回归窗口内真实发生：`dist/filesys/.staging` 里 21 个上一轮
   套件遗留的 `.part`（mtime 已过 TTL）被收走，同套件重跑又生成了 21 个新的（未过期、保留）。
   被收的是废弃残渣、判定条件全满足，不是误删；但它是**跨套件副作用**，性质和
   FINDING-GATE-1（套件互杀）同类——登记新套件时要考虑。
2. 想先观察再放开，只能靠操作员手动 `dry_run=1`；`reclaim.cfg` 里已写明这条路，
   但默认值不会替操作员做这件事。

## 已知的收尾噪音（非缺陷）

用 `timeout` / `taskkill /F` **强杀**进程时，Qt 静态析构会打一行
`Timers cannot be stopped from another thread`。正常停服（窗口关闭 / `taskkill` 不带 `/F`）
已在 `main()` 里显式 `stop()`，无此行。强杀场景下的状态一致性由 D/E 两条断言覆盖——
下个进程启动后的第一次心跳会把残留收干净。

## 未注册进 test_gate 的原因

`project-manifest.yaml: test_gate.suites` 里**没有**登记本套件。该套件会主动创建/删除
共享 DB 行与 `filesys` 下的文件（这正是它的用途），而现有十套件已经共同挤在同一个端口、
同一个库、同一棵运行树上串行排队；再加一个"故意删文件"的第 11 套是净风险而非净收益。
故仅作为本需求的可复跑证据归档，不进门。

注意这条理由现在只是"不再加一份风险"——**产品二进制里调度器默认就是活的**，所以既有十套
每次启动本来就已经各带一轮真清理（见上节）。本套件不进门，是为了不在此之上再加一个
"故意造出待回收对象/过期暂存"的套件。
