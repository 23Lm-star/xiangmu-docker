# REQ-EXT-SESSION-001 独立审计报告

- 审计节点：`audit`
- 审计对象：`fc1b6c7a372e64b53b6a9dec4793827cbd21deea`（detached worktree）
- 审计代理：Codex；与开发/测试代理 `claude-code` 不同
- 只读边界：未修改产品源码、验收标准、Git 历史、分支；仅写本报告、审计日志与 handoff

## 自跑结果

| 检查 | 结果 | 审计证据 |
|---|---|---|
| `pipeline/build.ps1` | PASS，`build GREEN (fc1b6c7, 20260930-124744)` | 审计树临时日志；已随 worktree 删除，摘要保留于本报告 |
| `pipeline/test.ps1` | exit 0；脚本报告 `0 automated tests registered` | 审计树临时日志；已随 worktree 删除，摘要保留于本报告 |
| `pipeline/audit.ps1` | PASS | 审计树临时日志；已随 worktree 删除，摘要保留于本报告 |
| `unique_login_e2e.py` | 16/16 PASS；包含 SEC-001 48/48、协议冻结检查 | 审计树临时日志；已随 worktree 删除，摘要保留于本报告 |
| `unique_login_probes.py` | 最后一次 8/8 PASS；审计中同参数曾出现一次 P4 失败 | 审计树临时日志；已随 worktree 删除，摘要保留于本报告 |

## Claim-check

1. **成立** — 零协议。`git diff master...fc1b6c7 -- Server/protocol.h Client/protocol.h` 退出 0；登录 ID 在 `Client/client.cpp:387-395` 通过 `caMsg` 的验证码首个 NUL 后附加区发送，服务端在 `Server/msghandler.cpp:270-286` 只扫描 `uiMsgLen` 内首个 NUL。E2E 的 E7b 亦 PASS（`project-docs/evidence/REQ-EXT-SESSION-001/unique_login_e2e.py:193-196`）。
2. **成立** — 原子版本事务。`Server/operatedb.cpp:293-344` 在同一事务中执行 `LAST_INSERT_ID(login_version + 1)`、读取版本、插入 `login_record`、置 `online=1`、提交；E1c/E1d/E3a/E3b PASS。
3. **成立** — 单调 CAS 与锁内快照/出锁 abort。`Server/mytcpserver.cpp:166-207` 只接受不低于现值的登记，快照在 socket 列表锁内，`abort()` 在出锁后；扫尾同样遵守 `Server/mytcpserver.cpp:211-227`。E3c/E3d、P5/P6 PASS。
4. **成立** — 版本门位置和语义。`Server/mytcpsocket.cpp:50-75` 先过 SEC-001 未认证门，再在业务分派前检查已认证连接版本；心跳也包含在已认证过期切断路径。E2/E6、P7/P6 PASS。
5. **成立** — 竞态守卫。`Server/mytcpsocket.cpp:470-484` 仅权威版本执行 offline 和登记表清理，过期连接跳过；E4/P5 PASS。
6. **成立** — 驱动事实与空值语义。代码使用 `QString` 绑定（`Server/operatedb.cpp:315-320`），迁移和种子将 `client_id` 定义为 `VARCHAR(64) NULL DEFAULT NULL`（`Server/migrate_ext_session_001.sql:5-11`、`project-docs/init_test_data.sql:18-24`）；P3/E5 观测空 ID 为 `NULL`/空值且登录成功。任务记录已申报该偏差（`project-docs/tasks/REQ-EXT-SESSION-001/task.md:45`）。
7. **成立** — SEC-001 归档回归。E7a 自跑结果为 `48/48`、零 FAIL；运行日志仅在 disposable worktree 生命周期内存在，摘要见本报告“自跑结果”。
8. **成立** — C1 边界。候选 diff 只新增独立 `Server/migrate_ext_session_001.sql` 与回滚 SQL；未触碰 `Server/main.cpp` 启动区或 `checkSchemaVersion`。设计明确 FILE-001 未合并时不接启动闸门（`project-docs/tasks/REQ-EXT-SESSION-001/design.md:62-63`）。
9. **部分成立** — 三处开发偏差均已记录且与代码一致：CAS（`task.md:44` 对应 `mytcpserver.cpp:167-175`）、`client_id NULL`（`task.md:45` 对应迁移/种子）、D4 心跳语义（`task.md:46` 对应 `mytcpsocket.cpp:62-75`）。未发现额外代码扩面；但设计表仍保留旧的 `NOT NULL DEFAULT ''` 文字（`design.md:21`），与实际迁移的 NULL 形态不一致，属于文档残差。

## 发现

- **medium**：对抗探针同参数复跑不稳定。审计中一次完整运行在 P1-P3 PASS 后，P4 返回 `ret=-99`，随后 Server 端口拒绝连接；之后空闲 70 秒诊断保持存活，最后一次完整复跑恢复为 P1-P8 `8/8 PASS`。现有证据不足以证明是产品竞态、数据库状态还是外部运行环境，不能把该失败静默升级为稳定通过。失败与恢复摘要已固化在本报告，原始临时日志随 worktree 删除。
- **low**：`pipeline/test.ps1` exit 0 但注册测试数为 0；本任务验收依赖 E2E/SEC-001 脚本，脚本本身仍是流程能力缺口，不构成产品断言通过。原始临时日志随 worktree 删除。
- **low**：`design.md:21` 的旧 `NOT NULL DEFAULT ''` 与代码/迁移的 NULL 事实不一致；任务记录已申报，但设计文档未同步修正。

## 分歧与结论

- 开发节点与独立测试节点记录 `8/8 PASS`、结论 PASS；本审计也取得最后一次 `8/8 PASS`，但观察到一次同参数 P4 失败。审计不自行调和该分歧，交主人仲裁。
- **结论：REJECT**。理由是审计期间出现未归因的中等严重度非确定性失败，不能证明所有关键声明在重复运行中稳定成立；需主人决定是否进入修复/复测节点后再审计，或以外部环境事实接受该风险。

## 审计产物哈希

- 候选：`fc1b6c7a372e64b53b6a9dec4793827cbd21deea`
- 最后探针日志：由工作树内审计输出生成，未写入产品 Git
- 独立测试输入哈希：`independent-test-fc1b6c7.md` = `F88659D72BBAFAC90FD15269DBAB93F540A80CE7FE037FC2A2C05C92E4B0816F`；`manifest.txt` = `D7E50F7E32B695EF2BDBC873430B6BED164CA52F5330B0D5B10AFDD0E795BF35`
