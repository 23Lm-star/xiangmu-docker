# 决策记录：BUG-TST-CLIENT-001 回归失败后的重新裁决

> 背景：项目测试师提交 `project-docs/test-reports/R02-20260725-TST-BUG-TST-CLIENT-001-regression.md`。三处工程修复已正确应用，但 Client 登录后主界面加载文件列表仍崩溃。

## 问题描述

1. 原修复已移除 `File::File()` / `Friend::Friend()` 构造函数中的网络刷新，并在 `MsgHandler::handllogin()` 中使用 `QTimer::singleShot(0, ...)` 延迟初始刷新。
2. 回归证据显示：`FLUSHFILE_RESPOND` 中 26 个文件条目全部被 `File::updateCurrfile()` 解析并执行 `ui->file_LW->addItem()`，崩溃发生在函数返回后的 Qt 延迟布局事件中。
3. 关键上下文：`Client/index.ui` 中 `QStackedWidget.currentIndex=0`，`friendPage` 是当前页，`filePage` 位于 index=1。登录后初始刷新仍会在 File 页非当前页时更新 `QListWidget`。
4. 崩溃类型仍为 `pure virtual method called` / `terminate called without an active exception` / exit code 3，属于严重运行时崩溃，继续阻塞 Client 端全链路动态测试。

## 方案评估

| 方案 | 结论 | 理由 |
|------|------|------|
| A：登录时先切到 File 页再发 `flushfile`，完成后切回 Friend 页 | 否决 | 这是以 UI 页切换规避生命周期问题，会产生闪烁/状态跳动；若响应返回时页面已被用户切走，仍可能在隐藏页更新列表；还把登录流程与文件页实现强耦合。 |
| B：不在 login 时刷新文件列表，将 `flushfile` 延迟到用户首次/每次进入 File 页时；响应处理不更新隐藏 `QListWidget` | 采纳 | 符合 QStackedWidget 生命周期：只有 File 页成为当前页后才触发文件刷新；同时要求 `File::updateCurrfile()` 分离“数据缓存”和“UI 列表重建”，隐藏时不得 `clear()`/`addItem()`。这是稳定架构修复，不依赖时序碰运气。 |
| C：在 `updateCurrfile()` 中再包一层 `QTimer::singleShot(0, ...)` | 否决 | 只是改变事件顺序，未改变“隐藏页 QListWidget 被更新”的根因；可能让崩溃更晚出现或更难复现，不作为主修复方案。 |

## 决策

采纳 **方案 B 的增强版：文件页按需刷新 + File 组件内部数据/UI 分离**。

工程师必须按以下约束实施：

1. `MsgHandler::handllogin()` 登录成功后仅刷新好友列表，不再主动调用 `Index::getinstance().getFile()->flushfile()`。
2. `Index::on_file_PB_clicked()` 切换到 `filePage` 后，通过事件队列下一轮触发 `filePage->flushfile()`，确保页面已成为当前可见页。
3. `File::updateCurrfile()` 不得直接假设 `ui->file_LW` 可安全布局：
   - 先接管/替换 `m_fileInfoList` 数据；
   - 若 File 页当前不可见，不执行 `ui->file_LW->clear()` 或 `addItem()`，只标记待刷新；
   - 若 File 页可见，再调用独立的 `rebuildFileList()` 更新 `QListWidget`。
4. `File` 应重写 `showEvent(QShowEvent*)` 或提供等效的 `onPageActivated()`，在页面变为可见且存在待刷新数据时重建列表。
5. 不修改 PDU 协议、`FileInfo` 二进制结构、Server 端文件列表响应格式，不新增外部依赖。

## 理由

1. 本缺陷根因不再是构造期发网络请求，而是 UI 更新发生在 QStackedWidget 非当前页上，触发 Qt 延迟布局队列后崩溃。
2. File 页按需刷新降低登录首屏耦合：登录只负责认证与进入主界面，文件列表由文件模块在用户进入文件页时负责加载。
3. 数据/UI 分离能覆盖响应到达后用户切页等边界，不依赖“响应一定在 File 页可见期间到达”的假设。
4. 修复范围局限于 Client UI 生命周期和文件页刷新调度，不改变协议、服务端和数据模型，风险可控。

## 影响范围

- 直接影响：`Client/msghandler.cpp`, `Client/index.cpp`, `Client/index.h`, `Client/file.cpp`, `Client/file.h`。
- 间接影响：Client 登录后首屏只刷新好友列表；文件列表在用户进入“文件”页时刷新。
- 不影响：Server、PDU 枚举、`FileInfo` 布局、数据库、上传/下载协议。

## 当前缺陷状态

- `BUG-TST-CLIENT-001`：保持 **重新打开**，立即移交项目工程师重新修复。工程师开始修复时必须将状态更新为 **修复中**；完成后更新为 **待复查**。
- `FILE-002-R1`：保持 **待复查**。
- `DB-003`, `SEC-001`, `NET-001`：保持 **已确认，可继续测试，修复延后**。

## 验证要求

工程师自检：

1. Client Debug 构建通过。
2. 静态搜索确认登录成功路径不再调用 `getFile()->flushfile()`。
3. 静态搜索确认 `File::updateCurrfile()` 中隐藏页路径不会调用 `ui->file_LW->clear()` / `addItem()`。
4. 登录 → 主界面停留 Friend 页时不触发 `FLUSHFILE_REQUEST`。
5. 点击“文件”页后触发 `FLUSHFILE_REQUEST`，响应到达后列表显示且不崩溃。

复查/回归：

1. 修复完成后必须交 Bug 查找师二次静态审查，因为这是严重缺陷修复。
2. 二次审查通过后交项目测试师执行 `BUG-TST-CLIENT-001` 动态回归。

## 相关缺陷

- BUG-TST-CLIENT-001

## 裁决时间

2026-07-26
