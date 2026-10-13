# REQ-EXT-GROUP-002 可靠群文本消息 — independent test 报告

- 节点：independent test（Development OS v1.7 / LARGE，新会话、分离工作树、只读候选）
- 候选：`03dc01d`（`codex/req-ext-group-002-group-messaging`，HEAD 亦为 `b0fba52` 的代码前驱）
- 工作树：`build/wt-test-group002`（`git worktree add --detach ... 03dc01d`，**未修改任何生产源码**）
- 构建：`pipeline/build.ps1` **GREEN**（`build-03dc01d-20261001-104114.log`）；被测二进制为本工作树独立构建
- 被测二进制：`build/wt-test-group002/dist/Server.exe`，sha256 `4a5aca0f6c0aba1c72eccefbce0405d3aee21ba3b6a274cb19b45f2eb0dd1bfd`
- 数据库：MySQL 5.7 / mydb；`Server/migrate_ext_group_002.sql` **幂等重放通过**（列已存在走 `SELECT 1` 分支，exit 0）
- 环境：`QT_ASSUME_STDERR_HAS_CONSOLE=1 QT_FORCE_STDERR_LOGGING=1`，CWD=dist；测试账号/群名全新前缀 `it2*`，未触碰开发 harness 残留数据

## 结论

**PASS（可交 independent audit）**：本需求范围内 **0 条归因于候选的缺陷**。
独立自研 harness（非复用 `verify.py`，断言均由候选源码推导）覆盖 design.md §12 全矩阵：
**part1 58/58、part2 36/37、part3 13/13、GROUP-001 回归 15/15**。

part2 的唯一 FAIL 与 part3 的两处无失败均指向**同一条基线既有缺陷**，源码级证明其与候选无关（见 FINDING-PRE1），
按流程**不阻断本需求验收**，另立缺陷交 repair/新分支处理。

## 覆盖矩阵（design.md §12）

| §12 条目 | 探针 | 结果 |
|---|---|---|
| 迁移/索引（last_seq、group_message、uk_group_seq、uk_group_client_msg、schema 2026100101） | S1_* | PASS |
| 1 幂等：串行重放同键同内容 → 同 seq/msg_id、1 行、不重复广播 | IDEM_serial_* / IDEM_replay_no_rebroadcast | PASS |
| 1 幂等：同键异内容 → 返回原消息不覆盖；跨发送者同键 → `-2104` 拒 | IDEM_replay_diff_content_* / IDEM_cross_sender_* | PASS |
| 1 幂等：单连接流水线同键 8 连发 → 全 0、同 seq、DB 1 行 | IDEM_pipelined_* | PASS |
| 2 并发序号：4 连接 × 50 对打 → 200 条 seq 连续唯一无缺无重，DB/last_seq 一致 | CC_* | PASS |
| 3 先提交后广播（负路径观测）：内容非法/非成员 → 无行、不耗序号、成员零事件；丢播仍落库可拉取 | CTB_* | PASS |
| 4 离线补齐 + 未读：离线 M 条 → 重连按游标补齐 M 条，unread==M，确认后归零 | OFF_* | PASS |
| 4/5 已读单调：不可回退、超水位钳制到 last_seq、不越权推进他人游标 | READ_* | PASS |
| 5 权限三断：非成员/已退出/被移除 × 发送/拉取/确认 → 全 `-2101`；未认证 → `-1001` | PERM_* / AUTH_* | PASS |
| 5 解散群：发送/拉取/确认 → 全 `-2102` | DISS_* | PASS |
| 6 分页：mode0/mode1 各 800/800 不缺不重（limit 37 与 1）、has_more、next_cursor 等于实际边界、limit 0/101 拒、mode2 拒、尾部冗余字节拒、水位空页 | PG_* | PASS |
| 6 载荷边界：空内容/空键 `-2103`；2000B 通过、2001B `-2002`；键 >64 `-2002` | VAL_* | PASS |
| 7 有界广播：慢连接不读 → 该连接 `[GROUP] ... user="it2slow" drops=1` 告警、实时仅收 419/800；快成员 800/800 无损；慢连接事后游标拉取补齐 802/802 | BC_* | PASS |
| 8 回归：一对一在线（转发+落库 issuccess=1）、离线内容补齐、发布链接（创建/去重 hash 形式/两端可解析/落盘） | REG_* | PASS |
| 8 回归：GROUP-001 全量（原独立 harness 复跑） | group001-regress.log | **15/15 PASS** |

## 观察（非缺陷）

- **mode1 边界语义**：`mode1` 查询为 `seq <= cursor`（含边界），`next_cursor` = 本批最小 seq，
  故调用方须传 `next_cursor - 1` 才能续翻（`PG_mode1_boundary_is_inclusive` 证实直接回传会重复边界条）。
  客户端 `Client/groupchat.cpp:163` 正是 `cursor = m_firstSeq - 1`，与契约自洽；`mode0` 为开区间无此问题。
  属"契约需读者注意"级，不构成缺陷，建议 design.md §7 补一句调用约定。

## FINDING-PRE1（基线既有缺陷，非本候选引入 — 见 `probe_chatmsg.py` / `part2-run.log`）

- **现象**：一对一离线消息在接收者每次调用 `GETCHATMSG(17)` 时**被重复下发**，且 `message_info.issuccess` 永不置 1。
  实测：同一连接连续两次 `getchatmsg` 均收到同一条 `probe-1`，两次查询后 `issuccess` 仍为 `(0,)`。
- **影响**：离线消息"拉取即已读"的语义失效 → 用户每次开窗重复看到历史离线消息。
- **非候选引入的证据**：
  - `git diff master...03dc01d -- Server/operatedb.cpp | grep -c getchatMsg` = **0**（该函数不在本分支 diff 内）；
  - `git show master:Server/operatedb.cpp` 与候选的 `getchatMsg` 函数体 sha256 **完全一致**（`690cbe79f52c5304…`）；
  - 函数体内部 `UPDATE message_info SET issuccess=1 WHERE seed_name=? AND rev_name=?` 与 `SELECT` 同参，
    SQL 手工执行可命中该行——疑似在同一 `QSqlQuery` 上"遍历完 SELECT 后直接 `prepare()` UPDATE"未生效（返回值被忽略、无日志）。
- **与 REQ-EXT-GROUP-002 的关系**：无。design D5/§4 明确群消息**不复用** `message_info` 旧模型，采用独立 `seq` 游标；
  群侧对应的"拉取不回写、未读由游标计算"行为在本轮 58/58 全通过。
- **建议**：另开 SMALL 分支修复（修 `getchatMsg`：SELECT 后 `q.finish()`/重建 `QSqlQuery` 再 UPDATE，或改写为单条事务），
  并在其独立测试中复验"二次 getchatmsg 不再重复下发 + issuccess=1"。

## 冻结路径人工复核（audit.ps1 覆盖不到 master...03dc01d）

`git diff master...03dc01d -- Client/protocol.h Server/protocol.h`：两份 `protocol.h` **仅追加** 88–96 九个枚举，
旧编号 1–87 一字未动、无重排、无删除；仅移除原先"88-95 预留"注释占位行。**符合 D2 批准范围**。

## 复现步骤

```bash
git worktree add --detach build/wt-test-group002 03dc01d
cd build/wt-test-group002
"C:/Program Files/MySQL/MySQL Server 5.7/bin/mysql.exe" -uroot -p123456 mydb < Server/migrate_ext_group_002.sql
powershell -ExecutionPolicy Bypass -File pipeline/build.ps1
cd dist && QT_ASSUME_STDERR_HAS_CONSOLE=1 QT_FORCE_STDERR_LOGGING=1 ./Server.exe > server-console.log 2>&1 &
python build/itest/itest_group002.py      # part1: 58/58
python build/itest/itest_part2.py         # part2: 36/37 (1 = FINDING-PRE1)
python build/itest/itest_part3.py         # part3: 13/13
python build/itest/group_e2e_regress.py   # GROUP-001: 15/15
```

## 未执行项 / 边界

- **GUI 冒烟**（群聊窗口打开/发送/双击重试）：无法自动化，本节点**未尝试**（不计入结论）；已知遗留，需人工或后续节点。
- DB 层"广播失败注入"未做源码级故障注入（只读候选，不改生产源码）；以负路径观测（失败无事件/无行/不耗序号）
  与慢连接丢播仍落库可拉取两项替代，结论等价。
- `pipeline/test.ps1` = honest-0（仓库自动化测试注册数为 0），不作为 GREEN 依据。

## cleanup_status

- 本节点启动的 `Server.exe`（PID 41844）已 `taskkill //F`，`tasklist` 复查无残留。
- 工作树 `build/wt-test-group002` 保留（工作副本在 gitignored `build/` 内，未入库）；候选 `03dc01d` 生产源码零改动。
- 测试临时文件全部位于 `build/`，证据副本已归档 `project-docs/evidence/REQ-EXT-GROUP-002/independent-test/`。
