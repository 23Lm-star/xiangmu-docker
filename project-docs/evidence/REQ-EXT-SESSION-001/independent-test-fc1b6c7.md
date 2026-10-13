# REQ-EXT-SESSION-001 独立测试报告 — 候选 fc1b6c7（2026-09-30）

结论：**PASS**（无需修复迭代）。测试仅报告，未触碰产品源码/验收标准。

代理身份声明（如实）：本测试节点由 claude-code 在新节点会话内执行（干净 detached worktree、全量独立复跑，未引用开发段任何输出数字）；"独立测试宜不同代理"为偏好而非硬门，**审计节点的 auditor≠developer 硬约束必须在下一节点由不同代理满足**。

## 执行环境
- 干净 detached worktree（.claude/worktrees/s001-itest @fc1b6c7），同仓库会话但独立于开发段进程。
- 环境铺设（gitignore 输入，非缺陷）：`Client/Server 的 connect.config`；运行时 MySQL 驱动 `libmysql.dll/qsqlmysql.dll/sqldrivers/`（主树为手放产物，新树必须复制）；`Server/filesys/sec001{a,b,r,v}` 预建用户根（verify.py 依赖，FILE-001 已记录同类环境事实）。

## 独立复跑结果（在 worktree 内自行执行，不引用开发段数字）
| 项 | 结果 |
|---|---|
| pipeline/build.ps1 | GREEN (fc1b6c7) |
| pipeline/audit.ps1 | PASS (fc1b6c7) |
| unique_login_e2e.py | **16/16 PASS**（itest-harness-run.txt；主树另 3 次复跑一致） |
| 对抗探针 P1-P8 | **8/8 PASS**（itest-probes-run.txt） |
| SEC-001 verify.py | **48/48 PASS** 零改动（经 harness E7a 在 worktree 内执行） |
| protocol.h 分支零命中 | PASS（E7b + audit 双重） |

## 对抗探针面（超出开发 harness 的输入）
- P1 超长 ID 200B→截断 ≤64 存储；P2 非打印字节净化（`\x01\x02AB\xff`→"AB"）；P3 无 NUL/无附加区老格式兼容（登录成功，档案 NULL）；P4 同连接重复登录不自踢（验证码 60s 冷却后取新码）；**P5 过期连接无法注销账号**（被切断，账号完好——踢出会话不留任何业务面）；P6 过期连接下载请求被切断且无响应；P7 未认证白名单不受版本校验影响；P8 踢后重连重新登录完全自愈。

## 发现（信息级，均不构成缺陷）
- I-1（存量，非本任务）：验证码取码 60s 冷却是既有机制；同连接连续登录需冷却或新连接——P4 验证过行为可预期。
- I-2（理论残余，接受）：极端调度下若旧会话断开事件滞后到权威会话已下线之后，会跳过一次在线清零；实测无法构造（abort 同步链路），影响止于 online 列，下次登录即覆盖。登记为设计已知边界。
- I-3（环境事实建议）：建议长期把 connect.config/驱动 DLL/用户根三项列入独立测试环境清单（本报告 §执行环境 已固化步骤）。

## 判定
验收 §6.1-§6.7 全部有独立证据支撑；无阻塞项；候选 fc1b6c7 可进入独立审计节点（审计代理须≠开发代理）。
