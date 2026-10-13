# BUG-ARC-RECOVERY-001 — 错误 Git 回退导致功能基线丢失

| 字段 | 值 |
|---|---|
| **编号** | BUG-ARC-RECOVERY-001 |
| **模块** | ARCH — 版本治理与集成 |
| **等级** | **严重** |
| **状态** | **已确认，待修复** |
| **发现者** | 项目架构师（根据用户反馈与 Git 证据确认） |
| **发现日期** | 2026-07-28 |
| **责任人** | 项目工程师 |

## 事实证据

- 当前分支 `add_数据库连接池功能` 的 HEAD 为 `d6c579c`。
- `git reflog` 记录：提交 `c6a472c`（`feat: heartbeat, sharelink framework, cascading logoff cleanup, UI buttons`）之后发生 `reset: moving to HEAD~1`，回退至 `d6c579c`。
- 历史提交 `149a087` 包含安全注销、心跳、外链及相关 UI/协议代码；当前基线中注销、心跳、外链实现均缺失。
- `149a087` 同时混入危险回退：Server PDU 的 `memfree()` 被改为 `free()`、`seedMsg()` 的释放被移除、好友转发从 `sendAddFriendAndRelease()` 回退为 `seedAddFriend()`。因此禁止整体恢复该提交。

## 影响范围

1. `REQ-AUTH-001` 安全注销的 UI、协议分发、事务清理及用户目录清理缺失。
2. `EXP-NET-01` 心跳与 `EXP-BUS-03` 公共外链框架未落在当前分支。
3. 当前 `protocol.h` 存在用户信息请求枚举定义与现有分发调用不一致的风险，必须先修复协议显式编号。

## 架构师裁决（2026-07-28）

采用**原子化定点恢复**，禁止 `reset`、`amend`、整体 `checkout` 或 `cherry-pick` 历史提交。

恢复顺序：

1. 先将 Client/Server `protocol.h` 同步为显式、稳定的消息编号；
2. 再以最小差异恢复 `REQ-AUTH-001`；
3. 经静态审查与动态回归后，才分别恢复心跳、外链。

## 禁止操作

- 禁止 `git reset`、`git reset --hard`、`git checkout .`、`git restore .`、`git commit --amend`；
- 禁止整体 `cherry-pick 149a087`、`c6a472c`，或整文件覆盖 `mytcpsocket.cpp` / `mytcpserver.cpp` / `msghandler.cpp`；
- 禁止修改 `MyTcpSocket::seedMsg()`、`sendAndRelease()`、下载流、好友转发与既有 socket 清理路径；
- 禁止将 Server PDU 的 `memfree()` 改为 `free()`。
