# BUG-REV-MEM-001: addfriendFin() PDU 内存泄漏

| 字段 | 内容 |
|------|------|
| **缺陷编号** | BUG-REV-MEM-001 |
| **严重等级** | 🔴 严重 |
| **发现日期** | 2026-07-29 |
| **发现者** | Bug 查找师 |
| **涉及模块** | 内存管理 (MEM) |
| **当前状态** | ✅ 已修复 |

## 问题描述

`MsgHandler::addfriendFin()` 通过 `mkPDU()` 创建 PDU，分别用 `seedAddFriend()` 发送给两个用户，但 `seedAddFriend()` 仅写入 socket 不释放 PDU，且调用结束后无人 `memfree(pdu)`。每次好友确认操作泄漏一个 PDU（≥76 字节）。

## 触发条件

任一用户确认添加好友请求时触发。

## 根因位置

- `Server/msghandler.cpp:257-263` — `addfriendFin()` 创建 PDU → 两次 `seedAddFriend` → 无释放

## 修复

在两次 `seedAddFriend()` 之后添加 `memfree(pdu);`

## 关联

与 BUG-ARC-NET-001（已关闭）类似但作用范围不同。BUG-ARC-NET-001 针对单次转发路径的 `seedAddFriend` 泄漏，`addfriendFin` 是双次发送路径的遗漏。
