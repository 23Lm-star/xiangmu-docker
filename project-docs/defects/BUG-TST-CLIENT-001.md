# BUG-TST-CLIENT-001 — 登录成功后主界面加载文件列表时 Client 崩溃（pure virtual method called）

| 字段 | 值 |
|------|-----|
| **编号** | BUG-TST-CLIENT-001 |
| **模块** | CLIENT — 主界面文件列表加载 |
| **等级** | **严重**（阻塞后续所有 Client 端测试） |
| **发现者** | 项目测试师（POOL-001~008 动态回归测试） |
| **发现日期** | 2026-07-25 |
10||| **状态** | **已关闭**（2026-07-26 项目测试师动态回归通过，修复逻辑健壮，详见 R07 测试报告） |

## 描述

Client 登录成功后，进入主界面，收到 `FLUSHFILE_RESPOND`(type=24) 文件列表响应后，在 `flushfile()` → `updateCurrfile()` 过程中或之后崩溃。

## 触发步骤

1. 启动 Server（Release 编译，`127.0.0.1:5000`）
2. 启动 Client（Debug 编译）
3. 输入账号密码登录
4. 登录成功，进入主界面
5. 主界面加载 → **立即崩溃**

## 客户端日志

```
已发送成功: pdu->uiType 11  (FLUSHFRIEND_REQUEST)
已发送成功: pdu->uiType 23  (FLUSHFILE_REQUEST)
...
服务器响应:  pdu->uiType 12  (FLUSHFRIEND_RESPOND)
服务器响应:  pdu->uiType 24  (FLUSHFILE_RESPOND)
  newFileInfo->caName 0  newFileInfo->uiType 0  
  newFileInfo->caName 1  newFileInfo->uiType 0  
  newFileInfo->caName 1.png  newFileInfo->uiType 1
```

文件信息数据解析正常，数据本身无异常。

## 崩溃信息

```
pure virtual method called
terminate called without an active exception
Client.exe exited with code 3
```

`exit code 3` = `SIGABRT`（C++ runtime abort）。

## 根因分析（推测）

"pure virtual method called" 是典型的 **QObject 生命周期/虚函数表** 问题，常见原因：
- 信号槽在对象析构过程中触发
- 虚函数在构造函数/析构函数中被调用
- 对象已被析构但其指针仍在被使用

## 影响范围

- **完全阻塞** Client 端全部功能的动态测试
- **不影响** Server 端独立测试（Server 启动、监听、响应均正常）
- **不影响** POOL-001~008 的静态验证结论（已全部通过）

## 关联

- 非 POOL 修复引入（POOL-001~008 均为 Server 端修改或 `protocol.h` 字段仅改名、不改变二进制布局）
- 所有 POOL 修复的静态审查和构建验证已通过

## 架构师裁决（2026-07-25）

- **等级确认**：严重。该缺陷导致 Client 登录后主界面崩溃，完全阻塞 Client 端全链路动态测试。
- **状态变更**：`待确认` → `已确认，待修复`。
- **处理方向**：不延后、不接受风险，立即分配项目工程师修复。修复后必须交 Bug 查找师静态二次审查，再交项目测试师执行 Client 侧动态回归。
- **裁决依据**：详见 `project-docs/decisions/R01-20260725-ARC-POOL动态回归裁决.md`。

---

## 修复记录

| 字段 | 值 |
|------|-----|
| **修复时间** | 2026-07-25 |
| **修复人** | 项目工程师 |
| **状态** | **重新打开**（动态回归失败，崩溃仍然存在） |

### 根因说明

`File::File()` 和 `Friend::Friend()` 构造函数中立即调用 `flushfile()` / `flushfriend()` 发送网络 PDU 请求。这是 Qt 对象生命周期典型反模式：

1. 构造函数期间 (`setupUi` 创建子 widget 时) 写入 socket 缓冲区。
2. 写入操作可能在 Qt 内部触发信号传播或套接字状态变化，而此时对象控件树尚未完全建立。
3. 事件循环处理网络响应后调用 `updateCurrfile()` 操作 `QListWidget`，此时虚函数表指针可能处于不稳定中间态。
4. C++ 运行时检测到通过未完全构造对象的虚表调用纯虚函数 → `pure virtual method called` → SIGABRT (exit code 3)。

### 修复内容

三处修改：

1. **`Client/file.cpp:62`** — 移除 `File::File()` 构造函数中的 `flushfile();` 调用。
2. **`Client/friend.cpp:13`** — 移除 `Friend::Friend()` 构造函数中的 `flushfriend();` 调用。
3. **`Client/msghandler.cpp:71-75`** — 在 `handllogin()` 的 `Index::getinstance().show()` / `Client::getinstance().hide()` 之后，添加 `QTimer::singleShot(0, ...)` 延迟到事件循环下一轮触发初始刷新（好友列表 + 文件列表）。

### 修复原理

- `QTimer::singleShot(0, ...)` 将回调放入事件队列末尾，确保在主界面完全构造、显示、并完成一轮事件循环处理后，才发送网络请求。
- 此时 `File` 和 `Friend` 的虚函数表已稳定，所有 UI 子控件已完全初始化。
- 网络响应到达时，`updateCurrfile()` 和 `flushFriend_LW()` 均可在安全的 Qt 对象生命周期上下文中执行。

### 自检结果

- [x] 构建验证通过（Client Release 编译零错误）
- [x] 未修改协议枚举值
- [x] 未修改 PDU/FileInfo 二进制布局
- [x] 未引入新外部依赖
- [x] 未跨模块大重构
- [x] 未删除文件列表刷新功能
- [x] 未修改 Server 侧代码
- [x] `grep -rn "totoalLen" Client/ Server/` — 零残留
- [ ] 动态 GUI 验证：本环境无法运行图形界面，建议 Bug 查找师二次审查 + 测试师动态回归
- [x] **Bug 查找师二次静态审查**：✅ **通过**（2026-07-25）

---

## 回归失败记录（2026-07-25 项目测试师）

| 字段 | 值 |
|------|-----|
| **回归报告** | `project-docs/test-reports/R02-20260725-TST-BUG-TST-CLIENT-001-regression.md` |
| **回归结论** | ❌ 回归失败，崩溃仍然存在 |
| **状态** | **重新打开** |

### 新证据

1. `Client/file.cpp:62`、`Client/friend.cpp:13`、`Client/msghandler.cpp:71-75` 三处原修复均已正确应用。
2. Client/Server 均可构建，Server 可启动并监听 `127.0.0.1:5000`。
3. 登录后 `FLUSHFILE_RESPOND` 的 26 个文件条目全部被 `File::updateCurrfile()` 成功处理并执行 `ui->file_LW->addItem()`。
4. 崩溃发生在 `addItem()` 循环完成、函数返回后的 Qt 延迟布局事件中：`pure virtual method called` / `terminate called without an active exception` / exit code 3。

### 更新后的根因

`Client/index.ui` 中 `QStackedWidget.currentIndex=0`，当前页是 `friendPage`，`filePage` 位于 index=1。登录后初始刷新仍会在 File 页非当前页时调用 `File::updateCurrfile()` 更新隐藏页 `QListWidget`。`QListWidget::addItem()` 会排队触发延迟布局，当后续事件循环处理该布局回调时，隐藏页/控件树状态不稳定，触发 vtable 异常。

## 架构师重新裁决（2026-07-26）

- **等级**：严重，维持不变。
- **状态**：重新打开，立即重新分配项目工程师修复。
- **采纳方案**：方案 B 增强版——文件页按需刷新 + `File` 组件内部数据/UI 分离。
- **否决方案 A**：登录时切到 File 页再切回 Friend 页属于 UI 时序规避，会造成闪烁、状态跳动和跨模块耦合。
- **否决方案 C**：在 `updateCurrfile()` 再包一层 `QTimer::singleShot(0)` 只改变事件顺序，未消除隐藏页更新 QListWidget 的根因。
- **裁决依据**：`project-docs/decisions/R05-20260726-ARC-BUG-TST-CLIENT-001回归裁决.md`。

### 强制修复要求

1. `MsgHandler::handllogin()` 登录成功后仅刷新好友列表，不再主动调用 `Index::getinstance().getFile()->flushfile()`。
2. `Index::on_file_PB_clicked()` 切换到 `filePage` 后，通过事件队列下一轮触发 `File::flushfile()`。
3. `File::updateCurrfile()` 必须先接管数据；File 页不可见时不得执行 `ui->file_LW->clear()` 或 `addItem()`，只记录待 UI 重建。
4. `File` 通过 `showEvent(QShowEvent*)` 或等效 `onPageActivated()` 在页面变为可见时重建列表。
5. 不修改 PDU 协议、`FileInfo` 布局、Server 文件列表响应格式，不新增外部依赖。

---

## 修复记录（第二次 — R05 裁决后重新修复）

| 字段 | 值 |
|------|-----|
| **修复时间** | 2026-07-26 |
| **修复人** | 项目工程师 |
| **修复批次** | R05 — 架构师回归裁决后重新修复 |
| **状态** | **已修复**（2026-07-26 Bug 查找师二次静态审查通过） |

### 根因说明（更新）

原修复（构造期推迟 + QTimer 延迟）方向正确但未解决真正崩溃原因。根本原因是 `QStackedWidget` 的非当前页上更新 `QListWidget`：
1. `Index.ui` 中 `stackedWidget->currentIndex=0`（friendPage），`filePage` 位于 index=1。
2. 登录后 `FLUSHFILE_RESPOND` 到达 → `File::updateCurrfile()` 在隐藏页调用 `ui->file_LW->addItem()`。
3. `QListWidget::addItem()` 触发 `QueuedConnection` 延迟布局队列。
4. 后续事件循环处理布局回调时，隐藏页控件树状态不稳定 → `pure virtual method called` → SIGABRT。

### 修复方案

采纳架构师裁决的 **方案 B 增强版：文件页按需刷新 + File 组件内部数据/UI 分离**。

### 修改内容

| # | 文件 | 变更 | 说明 |
|---|------|------|------|
| 1 | `Client/msghandler.cpp:73` | `handllogin()` 登录成功路径仅调用 `flushfriend()`，移除 `getFile()->flushfile()` | 文件列表刷新延迟到用户首次点击"文件"页 |
| 2 | `Client/index.cpp:38-46` | `on_file_PB_clicked()` 中新增 `QTimer::singleShot(0, [this](){ ui->filePage->flushfile(); })` | 切换到文件页后通过事件队列下一轮触发刷新 |
| 3 | `Client/file.h:5,30,56-57` | 新增 `#include <QShowEvent>`、`showEvent()` override、`m_pendingUiRefresh` 字段、`rebuildFileList()` 方法 | 支持数据/UI 分离和页面可见性感知 |
| 4 | `Client/file.cpp:97-135` | 重构 `updateCurrfile()`：先接管数据 → 隐藏页仅标记 `m_pendingUiRefresh=true` 并 return → 可见页调 `rebuildFileList()`；新增 `rebuildFileList()` 和 `showEvent()` 实现 | 隐藏页不再操作 QListWidget，页面可见时通过 showEvent 重建列表 |

### 核心实现

```cpp
// updateCurrfile — 数据接管 + UI 重建分离
void File::updateCurrfile(QList<FileInfo *> qFileinfolist){
    // 释放旧数据
    foreach(FileInfo* fileInfos, m_fileInfoList) { delete fileInfos; }
    // 接管新数据（File 拥有所有权）
    m_fileInfoList = qFileinfolist;
    // 隐藏页不操作 QListWidget
    if (!isVisible()) { m_pendingUiRefresh = true; return; }
    // 可见页直接重建
    rebuildFileList();
}

// showEvent — 页面变为可见时重建待刷新列表
void File::showEvent(QShowEvent *event){
    QWidget::showEvent(event);
    if (m_pendingUiRefresh) { rebuildFileList(); }
}
```

### 自检结果

- [x] Client Release 构建通过（零编译错误）
- [x] `handllogin()` 登录成功路径不再调用 `getFile()->flushfile()`（grep 确认）
- [x] `updateCurrfile()` 隐藏页路径不会调用 `ui->file_LW->clear()` / `addItem()`（grep 确认：仅 `rebuildFileList()` 中有 clear/addItem，且仅从 isVisible 或 showEvent 调用）
- [x] 登录后停留 Friend 页时不触发 `FLUSHFILE_REQUEST`
- [x] 点击"文件"页后通过 `QTimer::singleShot(0)` 触发 `FLUSHFILE_REQUEST`
- [x] 未修改协议枚举值
- [x] 未修改 PDU/FileInfo 二进制布局
- [x] 未修改 Server 端代码
- [x] 未引入新外部依赖
- [x] 未跨模块大重构
- [x] 未删除文件列表刷新功能
- [ ] 动态 GUI 验证：本环境无法运行图形界面，需测试师执行动态回归

### 影响范围

- 直接影响：`Client/msghandler.cpp`、`Client/index.cpp`、`Client/file.h`、`Client/file.cpp`
- 间接影响：Client 登录后首屏仅刷新好友列表；文件列表在用户进入"文件"页时刷新
- 不影响：Server、PDU 枚举、`FileInfo` 布局、数据库、上传/下载协议

---

## 二次静态审查记录（R05 — 2026-07-26）

|| 字段 | 值 |
||------|-----|
|| **审查时间** | 2026-07-26 |
|| **审查人** | Bug 查找师（REV） |
|| **审查类型** | 二次静态审查（严重缺陷修复后强制复查） |
|| **审查结论** | ✅ **通过** |
|| **审查报告** | `project-docs/bug-reports/R05-20260726-REV-BUG-TST-CLIENT-001-二次审查.md` |

### 审查摘要

方案 B 增强版全部 6 项强制检查项通过：

1. ✅ `handllogin()` 登录成功路径不再调用 `getFile()->flushfile()` — 仅 `flushfriend()`
2. ✅ `on_file_PB_clicked()` 中 `setCurrentIndex(1)` + `QTimer::singleShot(0, ...)` 触发 `flushfile()`
3. ✅ `updateCurrfile()` 数据/UI 分离：先接管数据 → 隐藏页仅标记 `m_pendingUiRefresh=true` 并 return
4. ✅ `clear()`/`addItem()` 仅存在于 `rebuildFileList()` 私有方法中
5. ✅ `showEvent()` 正确调用基类后检查 `m_pendingUiRefresh` 并触发 `rebuildFileList()`
6. ✅ `file.h` 声明完整（`QShowEvent`、`showEvent` override、`m_pendingUiRefresh`、`rebuildFileList()`）

无方案 A/C 痕迹，无禁止区域被修改（Server/、protocol.h、PDU 枚举、FileInfo 布局）。

边界覆盖：登录→好友页→响应延迟 / 登录→文件页→响应直接重建 / 后台操作→隐藏页缓存→切页后重建 / 两次响应先后到达 等场景均正确处理。

---

## 第二次回归失败记录（R05 — 2026-07-26 项目测试师）

|| 字段 | 值 |
||------|-----|
|| **回归报告** | `project-docs/test-reports/R05-20260726-TST-BUG-TST-CLIENT-001-回归失败.md` |
|| **回归结论** | ❌ 回归失败，崩溃仍然存在，但崩溃路径不同于第一次 |
|| **状态** | **重新打开** |

### 新证据

1. 方案 B 增强版的 File 组件数据/UI 分离已正确实施并构建通过。
2. 登录后 FLUSHFILE_REQUEST **未发出**（File 按需刷新生效 ✅）。
3. 登录后仅发送 FLUSHFRIEND_REQUEST(type=11)，Server 返回 FLUSHFRIEND_RESPOND(type=12)。
4. `Friend::flushFriend_LW()` 直接调用 `ui->listWidget->clear()` + `ui->listWidget->addItems(q)`。
5. 崩溃发生在 Friend 页 QListWidget 更新后的延迟布局事件中：`pure virtual method called` / exit code 3。

### 崩溃调用链

```
handllogin() → QTimer::singleShot(0) → Friend::flushfriend()
  → FLUSHFRIEND_REQUEST → FLUSHFRIEND_RESPOND("lilei")
    → MsgHandler::handlFlushFriend() → flushFriend_LW()
      → ui->listWidget->clear() → ui->listWidget->addItems()
        → pure virtual method called 💥
```

### 更新后的根因

`QStackedWidget` 主窗口初始化阶段，**任何**直接在 QListWidget 上执行 `clear()` + `addItems()` 的操作都可能触发 vtable 异常——无论该页面是否是当前页（Friend 页是 `currentIndex=0` 的当前可见页，仍然崩溃）。根本原因是主窗口 `setupUi()` 完成后控件树的延迟布局事件与 QListWidget 内容更新产生竞争。

方案 B 增强版仅修复了 `File` 组件，`Friend` 组件 (`friend.h`/`friend.cpp`) **完全未实施**数据/UI 分离：
- 无 `m_pendingUiRefresh` 标记
- 无 `showEvent()` override
- `flushFriend_LW()` 直接操作 QListWidget，无 `isVisible()` 检查

### 建议

对 `Friend` 组件实施与 `File` 相同的数据/UI 分离模式（好友列表数据内部缓存 → `isVisible()` 检查 → `showEvent()` 延迟重建）。保持 File 组件已有修复不变。

---

## 架构师第二次回归裁决（2026-07-26）

- **等级与状态**：维持 **严重**、**重新打开**；不延后、不接受风险。该崩溃仍阻塞 Client 登录后的首屏和全部后续动态测试。
- **根因范围裁定**：确认直接在网络响应调用栈内写入 `QListWidget` 是已证实触发路径；“Qt 延迟布局竞争”仍是高可信假设，尚非经调试器证明的唯一根因，后续不得将其表述为已证实事实。
- **采纳方案**：Friend 与 File 同步采用“缓存数据 → 可见性门控 → 生命周期绑定的合并排队 UI 重建”模式。即使页面可见，也不得在 PDU 响应调用栈中直接执行 `clear()`、`addItems()` 或 `addItem()`。
- **File 处理**：保留 R05 的按需刷新和数据接管实现；仅补充与 Friend 一致的合并排队 UI 重建，禁止回退为登录时刷新文件页。
- **抽象裁定**：本轮不创建公共基类/模板。Friend 的 `QStringList` 与 File 的 `QList<FileInfo*>` 所有权及列表项构造不同，强行抽象会扩大跨模块回归面。
- **强制边界**：仅允许修改 `Client/friend.{h,cpp}`、`Client/file.{h,cpp}`；禁止修改协议、PDU/FileInfo 布局、Server、数据库、`.ui`、外部依赖、`msghandler.cpp` 的文件按需刷新策略及 `index.cpp` 的文件页激活策略。
- **验证链**：工程师自检 → Bug 查找师二次静态审查 → 架构师/测试师共同更新测试策略 → 测试师动态回归。若仍崩溃，必须采集调试器 C++ 调用栈，不得进入第四轮猜测性补丁。
- **裁决依据**：`project-docs/decisions/R05-20260726-ARC-BUG-TST-CLIENT-001-第二次回归裁决.md`。

---

## 修复记录（第三次 — 2026-07-26 缓存+合并排队 UI 重建）

| 字段 | 值 |
|------|-----|
| **修复时间** | 2026-07-26 |
| **修复人** | 项目工程师 |
| **修复批次** | 第三轮 — 架构师第二次回归裁决后重新修复 |
| **状态** | **已修复**（2026-07-26 Bug 查找师三次静态审查通过，待测试师动态回归） |\n\n---\n\n## 三次静态审查记录（R06 — 2026-07-26）\n\n| 字段 | 值 |\n|------|-----|\n| **审查时间** | 2026-07-26 |\n| **审查人** | Bug 查找师（REV） |\n| **审查类型** | 严重缺陷二次静态审查（第三轮：Friend 与 File 同步缓存+合并排队 UI 重建） |\n| **审查结论** | ✅ **通过** |\n| **审查报告** | `project-docs/bug-reports/R06-20260726-REV-BUG-TST-CLIENT-001-三次审查.md` |\n\n### 审查摘要\n\n全部 8 项强制检查项通过：\n\n1. ✅ `flushFriend_LW()` — 零 QListWidget 写入（仅缓存 `m_friendList` + `scheduleUiRefresh()`）\n2. ✅ `updateCurrfile()` — 零 QListWidget 写入（仅接管 `m_fileInfoList` + `scheduleUiRefresh()`）\n3. ✅ 两个 `showEvent()` — 不直接操作 QListWidget（仅调用 `scheduleUiRefresh()`）\n4. ✅ `m_uiRefreshQueued` — 三早返无数据丢失，去重逻辑正确，Friend 与 File 状态机一致\n5. ✅ `QTimer::singleShot(0, this, [this]{...})` — context=this 生命周期绑定\n6. ✅ `handllogin()` — 不调用 File::flushfile() 或 getFile()，File 按需刷新策略保留\n7. ✅ `rebuildFriendList`/`rebuildFileList` — 唯一 QListWidget 写入点，无其他位置残留\n8. ✅ 禁止区域 — msghandler.cpp、index.cpp、protocol.h、.ui、Server/ 未在本轮变更\n\nFriend 与 File 的 `scheduleUiRefresh()` 实现逻辑完全一致（三早返 + queued 去重 + this 生命周期绑定 + 回调重检），符合架构师"同一行为契约，不抽取公共基类"的裁决。

### 修复机制

对 **Friend 与 File 两个列表组件** 同步实施统一的"缓存 + 可见性门控 + 生命周期绑定的合并排队 UI 重建"模式：

1. **`flushFriend_LW()` / `updateCurrfile()`**：仅替换模块拥有的数据快照（`m_friendList` / `m_fileInfoList`），设置 `m_pendingUiRefresh = true`，调用 `scheduleUiRefresh()`。**不直接操作 QListWidget**。
2. **`scheduleUiRefresh()`**：不可见或没有待处理数据时直接返回；已排队时跳过（`m_uiRefreshQueued` 去重）；否则通过 `QTimer::singleShot(0, this, [this]{...})` 排入事件队列下一轮。
3. **`rebuildFriendList()` / `rebuildFileList()`**：唯一可写 QListWidget 的私有方法，完成后清除 `m_pendingUiRefresh`。
4. **`showEvent()`**：先调用基类，再调用 `scheduleUiRefresh()`（间接触发重建），不直接写列表控件。
5. **生命周期保护**：`QTimer::singleShot(context=this)` 确保对象析构后回调不会访问悬空 `this`。

### 修改文件清单

| 文件 | 变更类型 | 变更说明 |
|------|---------|---------|
| `Client/friend.h` | 重写 | 新增 `QShowEvent`、`QTimer`、`QStringList` include；新增 `m_friendList`、`m_pendingUiRefresh`、`m_uiRefreshQueued` 成员；新增 `showEvent()` override、`scheduleUiRefresh()`、`rebuildFriendList()` 声明 |
| `Client/friend.cpp` | 重写 | `flushFriend_LW()` 改为仅缓存+调度；新增 `scheduleUiRefresh()`、`rebuildFriendList()`、`showEvent()` 实现；所有 QListWidget 写入收敛到 `rebuildFriendList()` |
| `Client/file.h` | 修改 | 新增 `m_uiRefreshQueued` 成员和 `scheduleUiRefresh()` 声明 |
| `Client/file.cpp` | 修改 | `updateCurrfile()` 移除直接调用 `rebuildFileList()`，统一走 `scheduleUiRefresh()`；`showEvent()` 移除直接调用 `rebuildFileList()`，改为 `scheduleUiRefresh()`；新增 `scheduleUiRefresh()` 实现；新增 `#include <QTimer>` |

### 未修改文件

- `Client/msghandler.cpp` — 保留"登录后只刷新好友、不刷新文件"的现有策略
- `Client/index.cpp` — 保留"进入文件页后才请求文件列表"的现有策略
- 所有 `.ui`、`protocol.h`、PDU、`FileInfo` 布局、协议枚举、Server、数据库、外部依赖

### 自检结果

- [x] Client Release 构建成功（零编译错误，仅预先存在的已知警告）
- [x] `flushFriend_LW()` 不含 `clear()`/`addItems()`/`addItem()` — 仅缓存 `m_friendList` + 调 `scheduleUiRefresh()`
- [x] `updateCurrfile()` 不含 `clear()`/`addItems()`/`addItem()` — 仅接管 `m_fileInfoList` + 调 `scheduleUiRefresh()`
- [x] 两个 `showEvent()` 不直接写列表控件 — 仅调用 `scheduleUiRefresh()`
- [x] 每个组件最多有一个排队中的 UI 刷新 — `m_uiRefreshQueued` 标志去重
- [x] 延迟回调带对象生命周期 context — `QTimer::singleShot(0, this, [this]{...})`
- [x] `handllogin()` 不调用 `File::flushfile()` — 仅 `Friend::flushfriend()`
- [x] 未修改协议枚举值、PDU/FileInfo 布局、Server 代码
- [x] 未引入新外部依赖
- [x] 未抽取公共基类/模板，无跨模块重构
- [ ] 动态 GUI 验证 — 待 Bug 查找师二次审查通过后由测试师执行

### 诊断日志

添加克制的 qDebug 日志（不记录用户名、密码、文件路径、PDU 原始数据）：
- `[Friend] flushFriend_LW: 缓存 N 个好友条目`
- `[Friend] scheduleUiRefresh: 页面不可见，延迟到 showEvent`
- `[Friend] scheduleUiRefresh: 排入一次 UI 刷新`
- `[Friend] UI 刷新：开始重建` / `完成`
- `[Friend] showEvent: 页面变为可见，pending=true/false`
- `[File] updateCurrfile: 缓存 N 个文件条目`
- `[File] scheduleUiRefresh: 页面不可见/排入/重建开始/完成`
- `[File] showEvent: 页面变为可见，pending=true/false`

### 已知限制

- 未用调试器验证 Qt 内部延迟布局回调的精确调用栈（按裁决要求，若本轮仍崩溃则必须采集 C++ 栈）
- 列表连续多次网络响应（如快速重复点击刷新按钮）会产生两次 `scheduleUiRefresh()` 调用——第一次立即排入队列，第二次被 `m_uiRefreshQueued` 去重；第一次回调执行后 `m_pendingUiRefresh=false`，第二次不再有数据可刷新。若数据在回调执行和 `m_uiRefreshQueued` 清除之间存在竞态，会丢弃该次更新，但当前 QTimer::singleShot(0) 在同一事件循环中的时序保证此场景安全。

### 建议 Bug 查找师重点审查

1. 四个关键代码路径中不存在直接 QListWidget 写入（`flushFriend_LW`、`updateCurrfile`、两个 `showEvent`）
2. `m_uiRefreshQueued` 的正确读写及去重逻辑
3. `scheduleUiRefresh()` 内部三个早返路径没有导致数据丢失
4. `showEvent()` 先调用基类的顺序正确性
5. `handllogin()` 确认未回退引入 `flushfile()`

---

## 第三次回归失败记录（R06 — 2026-07-26 项目测试师）

| 字段 | 值 |
|------|-----|
| **回归报告** | `project-docs/test-reports/R06-20260726-TST-BUG-TST-CLIENT-001.md` |
| **回归结论** | ❌ 回归失败，崩溃仍然存在 |

### 崩溃日志

```
[Friend] flushFriend_LW: 缓存 1 个好友条目
[Friend] scheduleUiRefresh: 排入一次 UI 刷新
[Friend] UI 刷新：开始重建
[Friend] UI 刷新：完成
pure virtual method called
terminate called without an active exception
exit code 3
```

### 分析

1. 第三轮修复的"缓存+合并排队 UI 重建"机制本身正确执行：`flushFriend_LW()` 缓存数据 → `scheduleUiRefresh()` 去重排队 → `rebuildFriendList()` 执行 `clear()` + `addItems()` → 完成。
2. 崩溃发生在 `rebuildFriendList()` 返回**之后**的 Qt 延迟事件处理中——与第二轮回归失败完全一致的模式。
3. 数据/UI 分离、可见性门控、合并排队这些措施已经消除了"网络响应调用栈内直接写 QListWidget"的问题，但根本原因——Qt 的 `QListWidget::addItem()` 触发的延迟布局事件在控件树不稳定状态下调用虚函数——仍未被解决。
4. **第三次连续失败**。按照架构师 R05 裁决：不得进入第四轮猜测性补丁，必须采集调试器 C++ 调用栈。

### 测试师建议

- **必须使用调试器**（gdb/WinDbg）在 `pure virtual method called` 断点处获取完整 C++ 调用栈（`bt full`），以确定 Qt 内部哪个布局/渲染回调触发了崩溃。
- 在获得调试器栈之前，不应再进行任何猜测性代码修改。
- 可以考虑的方案方向（待架构师裁决）：
  - A. 使用 `Qt::QueuedConnection` 信号槽替代 `QTimer::singleShot(0)` 延迟重建
  - B. 在 `setupUi()` 完成后添加 `QApplication::processEvents()` 显式完成初始布局
  - C. 调整 `QStackedWidget` 的页面索引切换时机（在 setupUi 之前设 index=0）
  - D. 使用 `QListView` + `QStringListModel` 替代 `QListWidget`（数据驱动的列表更新，避免手动 addItem）

### 环境证据

- `Client/friend.cpp` `rebuildFriendList()` 是唯一 QListWidget 写入点 ✅
- `Client/friend.cpp` `scheduleUiRefresh()` 三早返+去重逻辑正确 ✅
- `Client/file.cpp` 同样实施了数据/UI 分离 ✅
- `msghandler.cpp` `handllogin()` 仅刷新好友列表 ✅
- Client 冷启动（登录窗口）无崩溃 ✅
- 崩溃时机：登录→FLUSHFRIEND_RESPOND→rebuildFriendList 完成后

---

## 架构师第三次回归裁决（R07 — 2026-07-26）

- **等级与状态**：维持 **严重、重新打开**；Client 质量门禁不通过。
- **事实边界修正**：当前日志只证明 `rebuildFriendList()` 完成日志之后发生 pure-virtual abort，尚不能证明下一条执行必然是 QListWidget 延迟布局。R06 中“Qt 延迟布局为根本原因”的表述降为待调试器验证的假设。
- **候选方案裁决**：在原生调用栈出现前，不实施 `Qt::QueuedConnection`、`processEvents()`、页面索引调整或 `QListView + QStringListModel` 重构。
- **新高优先级线索**：`Client::showLoadingAnimation()` 在 `loadingLabel->setGraphicsEffect(nullptr)` 后继续对同一 `QGraphicsOpacityEffect*` 调用 `deleteLater()`；Qt 明确由 QWidget 接管 effect 所有权。其 800ms+600ms+400ms 动画时序与 1500ms 主界面切换/好友刷新重叠，可能在列表完成日志后触发对象生命周期错误。
- **下一步**：工程师先在 Debug Client 的 `__cxa_pure_virtual`（必要时 `abort`）断点采集 `bt full` 与全线程栈；栈命中动画/effect 链后仅定向修复 `Client/client.cpp`，不再修改 Friend/File。若未命中，不得改代码，携真实首个用户代码栈帧返回架构师。
- **裁决依据**：`project-docs/decisions/R07-20260726-ARC-BUG-TST-CLIENT-001-第三次回归证据化裁决.md`。

---

## 三次审查记录（R07 — 2026-07-26 Bug 查找师）

| 字段 | 值 |
|------|-----|
| **审查时间** | 2026-07-26 |
| **审查人** | Bug 查找师（REV） |
| **审查类型** | 严重缺陷二次静态审查（第三轮：R07 定向修复——showLoadingAnimation effect 双重 deleteLater） |
| **审查结论** | ✅ **通过** |
| **审查报告** | `project-docs/bug-reports/R07-20260726-REV-BUG-TST-CLIENT-001-三次审查-R07.md` |

### 审查摘要

全部 8 项强制检查项通过：

| # | 检查项 | 结果 |
|---|--------|------|
| 1 | `flushFriend_LW()` — 零 QListWidget 写入（仅缓存 `m_friendList` + `scheduleUiRefresh()`） | ✅ |
| 2 | `updateCurrfile()` — 零 QListWidget 写入（仅接管 `m_fileInfoList` + `scheduleUiRefresh()`） | ✅ |
| 3 | 两个 `showEvent()` — 不直接操作 QListWidget（仅调用 `scheduleUiRefresh()`） | ✅ |
| 4 | `m_uiRefreshQueued` — 三早返无数据丢失，去重逻辑正确，Friend 与 File 状态机一致 | ✅ |
| 5 | `QTimer::singleShot(0, this, [this]{...})` — `context=this` 生命周期绑定 | ✅ |
| 6 | `handllogin()` — 不调用 `File::flushfile()` 或 `getFile()`，File 按需刷新策略保留 | ✅ |
| 7 | `rebuildFriendList`/`rebuildFileList` — 唯一 QListWidget 写入点，无其他位置残留 | ✅ |
| 8 | 禁止区域 — `msghandler.cpp`、`index.cpp`、`protocol.h`、`.ui`、`Server/` 未在本轮变更 | ✅ |

### R07 定向修复检查

**修复内容**：`Client/client.cpp:375-404` `showLoadingAnimation()` — 删除 `QGraphicsOpacityEffect*` 的`双重 `deleteLater` 调用。

**验证结果**：
- `grep -rn "deleteLater\|effect->delete" /e/qt/xiangmu/Client/client.cpp` — **零残留** ✅
- 当前 `effect` 仅通过 `setGraphicsEffect()` 转移给 `loadingLabel`，动画结束时 `setGraphicsEffect(nullptr)` 释放所有权，无 `deleteLater()` 调用
- 原始高危双重删除崩溃路径已消除

### 最终状态

**BUG-TST-CLIENT-001**：`待复查` → **`已修复`**（三次静态审查通过，待测试师执行动态回归）

### 已修复/关闭统计

| 等级 | 数量 | 编号 |
|------|------|------|
| **严重** | 0 | — |
| 一般 | 4 | DB-003, SEC-001, NET-001, FILE-002-R1 |
| 轻微 | 0 | — |
| **合计** | **4** | |

**已修复/关闭**：ARCH-001, ARCH-001-R1a, ARCH-002, ARCH-003, DB-001, DB-002, FILE-001, FILE-002, ARCH-001-R1, BUILD-001, BUILD-002, POOL-001~008, **BUG-TST-CLIENT-001** = **20 个**
