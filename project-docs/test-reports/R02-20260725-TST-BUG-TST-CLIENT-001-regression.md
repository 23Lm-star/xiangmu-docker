# 测试报告 — BUG-TST-CLIENT-001 动态回归测试

| 字段 | 值 |
|------|-----|
| 报告编号 | R02-20260725-TST |
| 测试日期 | 2026-07-25 23:01 |
| 测试阶段 | BUG-TST-CLIENT-001 修复后 Client 动态回归 |
| 测试师 | 项目测试师 |
| 被回归缺陷 | BUG-TST-CLIENT-001 |
| 构建环境 | Qt 5.14.2 MinGW 64-bit, g++ 7.3.0 |

---

## 1. 回归结论

**❌ 回归失败 — 缺陷未修复，崩溃仍然存在。**

---

## 2. 修复代码验证

| # | 文件 | 变更 | 验证结果 |
|---|------|------|----------|
| 1 | `Client/file.cpp:62` | 移除 `File::File()` 构造函数中的 `flushfile()` | ✅ 已正确应用 |
| 2 | `Client/friend.cpp:13` | 移除 `Friend::Friend()` 构造函数中的 `flushfriend()` | ✅ 已正确应用 |
| 3 | `Client/msghandler.cpp:71-75` | 在 `handllogin()` 中 `Index::show()` 之后添加 `QTimer::singleShot(0, ...)` 延迟刷新 | ✅ 已正确应用 |

经 `git diff HEAD` 逐行确认，全部三处修改均已正确应用。

---

## 3. 构建验证

| 组件 | 构建类型 | 结果 |
|------|---------|------|
| Server | Release | ✅ 编译通过（仅已知 warning: `__declspec(thread)` |
| Client | Debug | ✅ 编译通过（仅已知 warning: signed/unsigned comparison） |

Server 启动正常，监听 `127.0.0.1:5000`；MySQL `localhost:3306` 正常运行。

---

## 4. 动态测试结果

### TC-01：登录 → 主界面 → 崩溃（核心验证项）

| 步骤 | 操作 | 结果 |
|------|------|------|
| 1 | 启动 Client.exe | 登录界面正常显示 |
| 2 | 输入账号密码，点击登录 | 登录验证通过，1.5s 延迟后主界面出现 |
| 3 | 主界面加载 | ❌ **崩溃** |

**崩溃日志：**

```
pFileInfo->caName: 0  qFileinfolist[i]->uiType 0
pFileInfo->caName: 1  qFileinfolist[i]->uiType 0
pFileInfo->caName: 1.png  qFileinfolist[i]->uiType 1
...（共 26 个文件条目全部成功处理）
pFileInfo->caName: 标准录音 8.mp3  qFileinfolist[i]->uiType 1
pure virtual method called
terminate called without an active exception
Client.exe exited with code 3
```

**关键发现**：26 个文件条目全部被 `updateCurrfile()` 成功处理并调用 `ui->file_LW->addItem()`。崩溃发生在 **所有 addItem 完成、函数返回之后的延迟事件中**。

### 其余测试用例

| 编号 | 测试项 | 结果 |
|------|--------|------|
| TC-02 | 双击目录进入子目录 | ⬜️ 被崩溃阻塞 |
| TC-03 | 创建/删除/重命名文件后自动刷新 | ⬜️ 被崩溃阻塞 |
| TC-04 | 上传文件后自动刷新 | ⬜️ 被崩溃阻塞 |
| TC-05 | 添加/删除好友后自动刷新 | ⬜️ 被崩溃阻塞 |
| TC-06 | 重复登录/登出 3 次 | ⬜️ 被崩溃阻塞 |

---

## 5. 根因分析（测试师诊断）

### 5.1 崩溃点

崩溃点在 `File::updateCurrfile()` (file.cpp:96-116) 中 `addItem()` 循环完成并函数返回后。具体触发机制：

1. `QListWidget::addItem()` 在内部调用 `QListView::rowsInserted()` 
2. 后者调用 `d->doDelayedItemsLayout()` 
3. 该函数使用 `QMetaObject::invokeMethod(this, "doItemsLayout", Qt::QueuedConnection)` 将布局更新排入事件队列
4. 当这个延迟调用在后续事件循环中被处理时，若控件树状态不稳定，虚表指针 → `pure virtual method called`

### 5.2 与构造函数修复的关系

构造函数修复 **方向正确**（不应在构造函数中发网络请求），但 **没有解决真正的崩溃原因**。崩溃不是发生在请求发送时（构造函数中），而是发生在响应处理时（`updateCurrfile()` 后）。

### 5.3 核心问题

**File 的 QListWidget 位于 QStackedWidget 的非当前页上**。Index 的 `stackedWidget->currentIndex=0` 对应 `friendPage`，File 在 index=1。当 `flushfile()` 的响应到达并调用 `updateCurrfile()` 时，`ui->file_LW` 所在的页面未激活、未显示，Qt 底层渲染状态处于过渡期，导致 `QueuedConnection` 的 `doItemsLayout()` 回调时虚函数调用失败。

### 5.4 工程师根因分析评定

- 原分析「构造函数中发网络请求导致 vtable 不稳定」：**部分正确但不充分**
- 缺失分析：「QStackedWidget 非当前页 + QueuedConnection 延迟渲染」
- 此遗漏导致修复方向正确但未解决实际问题

---

## 6. 建议修复方向

1. **方案 A（推荐）**：在 `handllogin()` 中先将 `stackedWidget` 切到 File 页（index=1），再发 flushfile 请求，完成后切回 Friend 页。
2. **方案 B**：不在 login 时刷新文件列表，将 flushfile 延迟到用户首次点击"文件"按钮时（如在 QStackedWidget 的 `currentChanged` 信号中处理）。
3. **方案 C**：在 `updateCurrfile()` 中，用 `QTimer::singleShot(0, ...)` 再包裹一层延迟，让 addItem 后的渲染事件在更稳定的上下文中执行。

---

## 7. 缺陷状态变更

| 缺陷编号 | 原状态 | 新状态 | 原因 |
|---------|--------|--------|------|
| BUG-TST-CLIENT-001 | 待复查 | **重新打开** | 修复后动态回归失败，崩溃仍然存在 |

---

## 8. 归档文件

| 文件 | 说明 |
|------|------|
| `project-docs/test-reports/R02-20260725-TST-BUG-TST-CLIENT-001-regression.md` | 本报告 |
| `project-docs/defects/README.md` | 缺陷索引待更新 |
| `project-docs/defects/BUG-TST-CLIENT-001.md` | 缺陷详情待更新 |
