# BUG-ARC-NET-001 — 添加好友转发 PDU 未释放

| 字段 | 值 |
|------|-----|
| **编号** | BUG-ARC-NET-001 |
| **模块** | NET — 服务端好友请求转发 / PDU 所有权 |
| **等级** | **一般** |
| **发现者** | 项目架构师（根据 Bug 查找师二次审查 OBS-002 裁定） |
| **发现日期** | 2026-07-27 |
| **发现文件** | `Server/msghandler.cpp:196-202`、`Server/mytcpserver.cpp:43-55` |
| 当前状态 | **已关闭** |

## 问题描述

`MsgHandler::addfriend()` 在目标用户在线时通过 `mkPDU()` 分配转发请求 PDU，并调用 `MyTcpServer::seedAddFriend()` 写入目标 socket。该处理函数随后返回 `nullptr`，因此 `MyTcpSocket::recvmsg()` 的 `seedMsg(respdu)` 不会释放该 PDU；`seedAddFriend()` 也仅写入、不承担释放责任。每次成功的在线添加好友请求都会遗留一次服务端 `memalloc` 分配。

## 影响与等级裁定

- 单次泄漏不会立即破坏好友事务结果或安全缓冲机制；不属于 BUG-DB-004/BUG-SEC-003 的回归。
- 请求可被持续触发，长时间运行的 Server 会累积未释放内存，故定级为**一般**，不得以“观察项”永久搁置。

## 架构师裁决（2026-07-27）

在合并本分支到 `master` 前修复。不得改变既有 `seedAddFriend()` 的借用语义，因为 `addfriendFin()` 会以同一个 PDU 连续向两个客户端转发。应新增名称明确的**单次拥有型**辅助方法（例如 `sendAddFriendAndRelease()`），仅供 `addfriend()` 的在线请求路径调用；该方法在一次 `write()` 尝试后无条件 `memfree(pdu)`，目标未命中时也必须释放。调用方在调用后不得再访问或释放该 PDU。

禁止：
1. 修改 `seedAddFriend()` 为无条件释放，或在 `addfriendFin()` 两端转发后释放同一 PDU；
2. 改变好友请求的 PDU 类型、数据库事务或协议布局；
3. 用 `delete`/`free` 替代 Server 端 `memfree()`。

## 最终裁决（关闭）
测试师提交的回归证据表明，`mytcpserver.cpp:73` 确认调用了 `memfree(pdu)`，`msghandler.cpp:200` 在线转发路径已切换。虽然测试师指出 `addfriendFin` 存在潜在的泄漏，但根据先前的裁决，该项未阻塞本次回归。接受风险，关闭本缺陷。
