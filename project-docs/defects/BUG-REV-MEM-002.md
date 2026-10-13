# BUG-REV-MEM-002: sharefile() resendpdu 内存泄漏

| 字段 | 内容 |
|------|------|
| **缺陷编号** | BUG-REV-MEM-002 |
| **严重等级** | 🔴 严重 |
| **发现日期** | 2026-07-29 |
| **发现者** | Bug 查找师 |
| **涉及模块** | 内存管理 (MEM) |
| **当前状态** | ✅ 已修复 |

## 问题描述

`MsgHandler::sharefile()` 通过 `mkPDU()` 创建 `resendpdu`，在循环中用 `reseed()` 发送给 N 个接收者，但 `reseed()` 仅写入 socket 不释放 PDU，循环结束后 `resendpdu` 从未 `memfree`。每次群分享操作泄漏一个 PDU。

## 触发条件

用户向一个或多个好友分享文件时触发。

## 根因位置

- `Server/msghandler.cpp:449-460` — `mkPDU` → for 循环 `reseed` → 无释放

## 修复

在循环结束后添加 `memfree(resendpdu);`
