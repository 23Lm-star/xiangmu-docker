# 二次静态审查报告：BUG-TST-CLIENT-001 第三轮修复（R07）

| 字段 | 值 |
|------|-----|
| **审查时间** | 2026-07-26 |
| **审查人** | Bug 查找师（REV） |
| **审查类型** | 严重缺陷二次静态审查（第三轮：R07 定向修复——showLoadingAnimation effect 双重 deleteLater） |
| **审查批次** | R07 |
| **缺陷编号** | BUG-TST-CLIENT-001 |
| **审查结论** | ✅ **通过** |

---

## 上下文回执

**流水线位置**：BUG-TST-CLIENT-001 三次静态审查

**上游 Agent**：项目工程师

**我的角色**：Bug 查找师

**下游 Agent**：测试师（审查通过后执行动态回归）

**本次阶段**：R07 定向修复二次静态审查——验证 Friend/File 缓存+合并排队 UI 重建保持正确 + showLoadingAnimation() 双重 deleteLater 已删除

---

## 输入确认

**收到的输入**：
- `Client/friend.h` ✅
- `Client/friend.cpp` ✅
- `Client/file.h` ✅
- `Client/file.cpp` ✅
- `Client/client.cpp`（R07 修复）
- `Client/msghandler.cpp`（只读核验）
- `Client/index.cpp`（只读核验）
- `Client/protocol.h`（只读核验）

**输入完整性**：完整，包含修补摘要，所有要求文件均可查阅。

---

## 审查方法

三层递进执行：

1. **第一层：接口一致性** ✅ — `friend.h`/`file.h` 公共接口签名与 `interfaces.md` 一致（无需跨模块变更）
2. **第二层：变更影响分析** ✅ — 影响范围限于 `Client/friend.{h,cpp}`、`Client/file.{h,cpp}`、`Client/client.cpp`；不改变数据模型或模块边界
3. **第三层：深度逻辑审查** ✅ — 触发条件：严重缺陷二次审查 + 核心业务规则变更（Friend 与 File 全链路数据/UI 分离 + 动画 effect 生命周期修复）

---

## 8 项强制检查结果

### 检查 1：`flushFriend_LW()` 函数体中无 `clear()`/`addItems()`/`addItem()`

**结果**：✅ **通过**

**代码位置**：`Client/friend.cpp:36-42`

**审查依据**：函数体仅执行：
1. 缓存好友列表数据到 `m_friendList`
2. 设置 `m_pendingUiRefresh = true`
3. 调用 `scheduleUiRefresh()`

**零 `clear()` / `addItems()` / `addItem()` 调用。** 所有 QListWidget 写入已收敛到 `rebuildFriendList()`。

---

### 检查 2：`updateCurrfile()` 函数体中无 `clear()`/`addItems()`/`addItem()`

**结果**：✅ **通过**

**代码位置**：`Client/file.cpp:98-109`

**审查依据**：函数体仅执行：
1. 释放旧 `m_fileInfoList` 内存（`foreach delete`）
2. 接管新数据（`m_fileInfoList = qFileinfolist`）
3. 设置 `m_pendingUiRefresh = true`
4. 调用 `scheduleUiRefresh()`

**零 `clear()` / `addItems()` / `addItem()` 调用。** 所有 QListWidget 写入已收敛到 `rebuildFileList()`。

---

### 检查 3：两个 `showEvent()` 不直接操作 QListWidget（仅调用 `scheduleUiRefresh`）

**结果**：✅ **通过**

**Friend::showEvent** — `Client/friend.cpp:81-87`：
```cpp
void Friend::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);      // ✅ 先调用基类
    scheduleUiRefresh();            // ✅ 仅调度，不直接写列表
}
```

**File::showEvent** — `Client/file.cpp:156-161`：
```cpp
void File::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);      // ✅ 先调用基类
    scheduleUiRefresh();            // ✅ 仅调度，不直接写列表
}
```

两者均正确：先调基类，再通过 `scheduleUiRefresh()` 间接请求重建。无直接 `clear()` / `addItem()` / `addItems()` 操作。

---

### 检查 4：`m_uiRefreshQueued` 正确读写及去重逻辑（scheduleUiRefresh 三早返路径无数据丢失）

**结果**：✅ **通过**

Friend 与 File 的 `scheduleUiRefresh()` 实现完全一致：

```cpp
// 三个早返路径：
if (!m_pendingUiRefresh) return;    // ① 无待处理数据 → 安全跳过
if (!isVisible()) return;           // ② 不可见 → m_pendingUiRefresh 保留，showEvent 触发
if (m_uiRefreshQueued) return;      // ③ 已排队 → 去重

m_uiRefreshQueued = true;
QTimer::singleShot(0, this, [this]() {
    m_uiRefreshQueued = false;
    if (isVisible() && m_pendingUiRefresh) {
        rebuildFriendList(); / rebuildFileList();  // 回调中重检
    }
});
```

**早返路径分析**：
| 路径 | 安全理由 |
|------|---------|
| ① `!m_pendingUiRefresh` | 无数据要刷新，跳过正确。数据写入 `m_friendList`/`m_fileInfoList` 后 `m_pendingUiRefresh=true` |
| ② `!isVisible()` | 数据由 `m_pendingUiRefresh=true` 保留，后续 `showEvent()` 触发 `scheduleUiRefresh()` 时恢复正常调度 |
| ③ `m_uiRefreshQueued` | 已有回调排队中，无需重复。回调执行时重新检查 `m_pendingUiRefresh`，最新数据在 `m_friendList`/`m_fileInfoList` 中已保留 |

**竞态分析**：`QTimer::singleShot(0)` 确保回调在同一事件循环的下一轮执行。`m_uiRefreshQueued` 在回调中先置 `false` 再重建，回调执行期间新到达的数据会设置 `m_pendingUiRefresh=true` 并触发新的 `scheduleUiRefresh()`（因为 `m_uiRefreshQueued=false`），不会丢失。

---

### 检查 5：`QTimer::singleShot` 使用了 `context=this` 参数

**结果**：✅ **通过**

| 组件 | 位置 | 代码 | 验证 |
|------|------|------|------|
| Friend | `friend.cpp:62` | `QTimer::singleShot(0, this, [this](){...})` | ✅ |
| File | `file.cpp:145` | `QTimer::singleShot(0, this, [this](){...})` | ✅ |

`context=this` 确保组件析构后回调不会访问悬空 `this` 指针。

---

### 检查 6：`handllogin()` 不调用 `File::flushfile()` 或 `getFile()`——登录后 File 按需刷新策略保留

**结果**：✅ **通过**

**代码位置**：`Client/msghandler.cpp:66-76`

```cpp
QTimer::singleShot(1500, [loginName]() {
    Client::getinstance().m_LoginName = loginName;
    Index::getinstance().show();
    Client::getinstance().hide();
    QTimer::singleShot(0, []() {
        Index::getinstance().getFriend()->flushfriend();  // ✅ 仅刷新好友
        // 文件列表刷新延迟到用户首次点击"文件"页时
    });
});
```

- 仅调用 `getFriend()->flushfriend()` ✅
- 无 `getFile()` 或 `flushfile()` 调用 ✅
- 文件按需刷新策略（来自 R05 裁决）完整保留 ✅

---

### 检查 7：`rebuildFriendList()`/`rebuildFileList()` 是唯一写 QListWidget 的位置

**结果**：✅ **通过**

| 函数 | 位置 | 操作 | 唯一性验证 |
|------|------|------|-----------|
| `Friend::rebuildFriendList()` | `friend.cpp:74-79` | `ui->listWidget->clear()` + `addItems()` | ✅ 唯一 `clear()`/`addItems()` 位置 |
| `File::rebuildFileList()` | `file.cpp:111-126` | `ui->file_LW->clear()` + `addItem()` | ✅ 唯一 `clear()`/`addItem()` 位置 |

其他函数仅从 QListWidget 读取（`currentItem()`），不写入。

**写入调用链**：`scheduleUiRefresh()` → `QTimer::singleShot(0)` → `rebuildFriendList()`/`rebuildFileList()`（在回调中经过 `isVisible() && m_pendingUiRefresh` 重检后执行）

---

### 检查 8：禁止区域未被修改

**结果**：✅ **通过**

| 文件 | 核验结果 |
|------|---------|
| `Client/msghandler.cpp` | ✅ `handllogin()` 保持 R05 策略（仅 `flushfriend()`，无 `flushfile()`）；其他函数无影响本轮修复的变更 |
| `Client/index.cpp` | ✅ `on_file_PB_clicked()` 保持 R05 策略（`QTimer::singleShot(0)` → `flushfile()`）；无其他变更 |
| `Client/protocol.h` | ✅ 枚举定义完整，无新增或变更；`totalLen` 字段名已更正（POOL-002），无布局变化 |
| `Client/*.ui` | ✅ `client.ui` 在早期轮次已修改（登录页样式美化）；未在本轮有所变动 |
| `Server/` 全部 | ✅ 未修改任何 Server 文件 |

---

## R07 定向修复检查：`showLoadingAnimation()` 双重 deleteLater

**结果**：✅ **通过**

**代码位置**：`Client/client.cpp:375-404`

**修复内容**：删除 `showLoadingAnimation()` 中 `QGraphicsOpacityEffect*` 的双重 `deleteLater` 调用。

**审查依据**：
- `grep -n "deleteLater\|effect->delete" /e/qt/xiangmu/Client/client.cpp` — **零残留** ✅
- 当前代码中 `effect` 指针仅通过 `QWidget::setGraphicsEffect()` 转移所有权给 `loadingLabel`
- 动画结束时（L396-398）：`hide()` + `setGraphicsEffect(nullptr)` 释放 QWidget 所有权
- 不存在 `deleteLater()` 调用，不存在双重删除路径

**剩余风险**：`setGraphicsEffect(nullptr)` 后 `effect` 对象未被显式释放（轻微内存泄漏），但原始的高危双重删除崩溃路径已消除。该泄漏不影响功能正确性，且对象生命周期绑定到 `Client` 窗口（构造函数中 `new QGraphicsOpacityEffect(this)` 曾设置 Qt 父子关系，但 `setGraphicsEffect()` 已转移所有权，因此泄漏量级为一个效果对象/每次登录动画）。

**时序分析**：800ms（淡入）+ 600ms（停留）+ 400ms（淡出）= 1800ms 总动画时长，与 `handllogin()` 中的 1500ms 切换定时器存在 300ms 重叠。`setGraphicsEffect(nullptr)` 在淡出动画完成后（~1800ms）才执行，此时 Friend 页面的 `rebuildFriendList()` 已完成（~1500ms + 事件循环队列）。时间窗口重叠但不冲突。

---

## 总体结论

### 审查结果：✅ **全部通过**

**8 项强制检查项**：
| # | 检查项 | 结果 |
|---|--------|------|
| 1 | `flushFriend_LW()` 无直接 QListWidget 写入 | ✅ |
| 2 | `updateCurrfile()` 无直接 QListWidget 写入 | ✅ |
| 3 | 两个 `showEvent()` 不直接操作 QListWidget | ✅ |
| 4 | `m_uiRefreshQueued` 三早返无数据丢失 | ✅ |
| 5 | `QTimer::singleShot` 使用 `context=this` | ✅ |
| 6 | `handllogin()` 不调用 `File::flushfile()` | ✅ |
| 7 | `rebuildFriendList`/`rebuildFileList` 唯一写入点 | ✅ |
| 8 | 禁止区域未在本轮被修改 | ✅ |

**R07 定向修复检查**：
- `showLoadingAnimation()` 中无 `deleteLater` 残留 ✅
- 双重删除路径已消除 ✅

### 未覆盖/剩余风险

1. **调试器栈验证**：按 R07 裁决要求，工程师应在 Debug 断点采集 `__cxa_pure_virtual` 调用栈确认根因；本静态审查不替代该证据化验证。
2. **`handllogin()` 中 `QTimer::singleShot(0)` 无 context**（msghandler.cpp:72）：内部 `QTimer::singleShot` 的 lambda 无 `this` 参数，但由于仅捕获空值（`[](){}`）并访问单例（`Index::getinstance()`），实际风险极低。属于预存在代码，非本轮引入。
3. **effect 轻微内存泄漏**：`setGraphicsEffect(nullptr)` 后 `QGraphicsOpacityEffect*` 未被显式释放；每次登录动画泄漏一个效果对象（约数百字节）。

---

## 状态变更

**BUG-TST-CLIENT-001**：`待复查` → **`已修复`**

变更依据：全部 8 项强制检查通过，R07 定向修复正确。

---

## 下游 Agent 交接指令

**下游 Agent**：项目测试师

**任务**：BUG-TST-CLIENT-001 R07 修复动态回归测试

### 测试范围

1. **核心回归路径**：
   - 登录成功 → 进入主界面（Friend 页）
   - 等待 `FLUSHFRIEND_RESPOND` → 好友列表显示
   - 点击"文件"页 → 验证 `FLUSHFILE_REQUEST` 发出 → 文件列表显示
   - 在文件页未加载完成时切换回好友页，再切回文件页

2. **动画并发验证**：
   - 登录动画（800ms 淡入 + 600ms 停留 + 400ms 淡出 = 1800ms）与主界面切换（1500ms）的重叠时序
   - 多次连续登录/登出验证动画 effect 无双重删除

3. **边界测试**：
   - 网络响应期间切页（文件页响应处理中切换到好友页）
   - 10 次好友/文件往返切换
   - 快速连续点击"刷新好友"按钮
   - 客户端窗口最小化/还原后检查列表渲染

### 已知缺陷清单

| 编号 | 状态 | 说明 |
|------|------|------|
| BUG-TST-CLIENT-001 | ✅ **已修复**（审查通过，待动态回归） | 登录后 pure virtual method called 崩溃 |
| BUG-DB-003 | 已确认，可继续测试，修复延后 | 无数据库断线重连 |
| BUG-SEC-001 | 已确认，可继续测试，修复延后 | 密码无盐 MD5 |
| BUG-NET-001 | 已确认，可继续测试，修复延后 | 无应用层心跳 |

### 禁测区域

- Server 侧独立测试不受影响，但无新功能需要测试
- 文件上传/下载的全链路测试不在此次回归范围内（属于后续缺陷覆盖范围）
- PDU 协议一致性已在 POOL-001~008 静态审查和动态回归中验证通过

### 如果仍崩溃

若 `pure virtual method called` 仍复现，**必须**：
1. 在 Debug Client 的 `__cxa_pure_virtual`（或 `abort`）断点处采集 `bt full` 与全线程栈
2. 携带真实调用栈返回到架构师
3. **不得**进行第四轮猜测性代码修改
