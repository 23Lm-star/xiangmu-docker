# 动态回归测试报告：BUG-TST-CLIENT-001 第三轮 — ❌ 回归失败

| 字段 | 值 |
|------|-----|
| **报告编号** | R06-20260726-TST-BUG-TST-CLIENT-001 |
| **测试时间** | 2026-07-26 17:57 |
| **测试人** | 项目测试师（GUI 测试由用户协助执行） |
| **缺陷** | BUG-TST-CLIENT-001（第三轮修复：Friend+File 缓存+合并排队UI重建） |
| **测试类型** | 严重缺陷修复后动态回归 |
| **结论** | ❌ **回归失败 — 第三次连续失败** |

---

## 源码验证（全部通过）

| 检查项 | 结果 |
|--------|------|
| `flushFriend_LW()` 仅缓存+调度 | ✅ |
| `updateCurrfile()` 仅接管数据+调度 | ✅ |
| 两个 `showEvent()` 不直接操作 QListWidget | ✅ |
| `scheduleUiRefresh()` 三早返+去重 | ✅ |
| `QTimer::singleShot(0, this, [this]{...})` | ✅ |
| `rebuildFriendList()` / `rebuildFileList()` 唯一写入点 | ✅ |
| `handllogin()` 仅 flushfriend | ✅ |
| `on_file_PB_clicked()` QTimer→flushfile | ✅ |

---

## 自动化验证（通过）

| 测试 | 结果 |
|------|------|
| Client 冷启动 | ✅ 无崩溃，30+ 秒存活 |
| Server 连接 | ✅ 127.0.0.1:5000 可连接 |

---

## GUI 回归测试（❌ 失败）

### TC-1：登录好友页测试

**状态**：❌ **失败**

**日志**：

```text
[Friend] flushFriend_LW: 缓存 1 个好友条目
[Friend] scheduleUiRefresh: 排入一次 UI 刷新
[Friend] UI 刷新：开始重建
[Friend] UI 刷新：完成
pure virtual method called
terminate called without an active exception
17:57:23: Client.exe exited with code 3
```

**分析**：

1. `rebuildFriendList()` 正确执行完毕（`clear()` + `addItems()` 完成，"UI 刷新：完成" 日志已输出）
2. 崩溃发生在函数返回**之后**的 Qt 内部延迟事件处理中
3. 与第二轮回归失败完全一致的模式（`rebuildFriendList` 完成 → Qt 延迟布局事件 → `pure virtual method called`）
4. 数据/UI 分离、合并排队等措施已消除"网络响应调用栈内直接操作 QListWidget"问题，但 `QListWidget::addItem()` 触发的 Qt 内部延迟布局竞争仍然存在

### TC-2 ~ TC-5：未执行（TC-1 崩溃阻塞）

---

## 三轮回归对比

| 轮次 | 修复方案 | 崩溃点 | 崩溃时机 |
|------|---------|--------|---------|
| R02（第一轮） | 构造期推迟 + QTimer | `File::updateCurrfile()` | addItem 循环中 |
| R05（第二轮） | File 数据/UI 分离 | `Friend::flushFriend_LW()` | clear+addItems 后 |
| R06（第三轮）⭐ | Friend+File 同步缓存+合并排队 | `Friend::rebuildFriendList()` 完成后 | addItems 完成后 Qt 延迟事件 |

三轮修复逐步缩小了崩溃触发范围，但**根本原因**（Qt 内部延迟布局事件在控件树不稳定状态下调用虚函数）始终未被根除。

---

## 测试师判定

- **不得进入第四轮猜测性修改**（架构师 R05 裁决）
- **必须使用调试器获取 C++ 调用栈**（gdb `bt full` 或 WinDbg）
- 移交架构师进行新一轮裁决
- 建议方向（待架构师选择）：
  - A. 使用 `Qt::QueuedConnection` 信号槽替代 `QTimer::singleShot(0)`
  - B. 在 `setupUi()` 后显式调用 `QApplication::processEvents()` 完成初始布局
  - C. 调整 `QStackedWidget` 页面索引切换时机
  - D. 使用 `QListView` + `QStringListModel` 替代 `QListWidget`（数据驱动，避免手动 addItem）
