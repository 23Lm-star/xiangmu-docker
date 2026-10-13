# BUG-REV-PUBLINK-001: createPublishLink() 使用未初始化的 ClientName

| 字段 | 内容 |
|------|------|
| **缺陷编号** | BUG-REV-PUBLINK-001 |
| **严重等级** | 🔴 严重 |
| **发现日期** | 2026-07-29 |
| **发现者** | Bug 查找师 |
| **涉及模块** | 分享链接 (PUBLINK) |
| **当前状态** | ✅ 已修复 |

## 问题描述

`MsgHandler::createPublishLink()` 使用成员变量 `ClientName` 作为分享链接的 `ownerName`，但 `ClientName` 从未被赋值（始终为空字符串）。这导致所有创建的外链分享记录 `owner_name` 为空，后续 `resolvePublishLink` 解析时文件路径构造 `QDir(filesPath).filePath("")` 将失败。

## 触发条件

用户登录后创建应用内分享链接（`ENUM_MSG_TYPE_CREATE_PUBLINK_REQUEST`）。

## 根因

- `Server/msghandler.h:19` — `QString ClientName;` 仅声明，从未赋值
- `Server/msghandler.cpp:726` — `QString ownerName = ClientName;` 使用了未初始化变量
- `Server/mytcpsocket.cpp:149-151` — handlMsg 中未将 `m_strLoginName` 传递给 `createPublishLink()`

## 修复

1. `createPublishLink()` 改为接受 `const QString& loginName` 参数
2. `mytcpsocket.cpp` 的 handlMsg switch 中传入 `this->m_strLoginName`
3. 头文件声明同步更新
