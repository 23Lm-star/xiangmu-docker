# 决策记录：BUG-TST-CLIENT-001 第二次动态回归失败裁决

> 背景：项目测试师在 `project-docs/test-reports/R05-20260726-TST-BUG-TST-CLIENT-001-回归失败.md` 中确认：R05 方案 B 增强版已阻止登录后 File 页刷新，但登录后 Friend 页收到 `FLUSHFRIEND_RESPOND` 后仍发生 `pure virtual method called`（exit code 3）。

## 一、已确认事实

1. `Client/msghandler.cpp:71-75` 登录成功后仅发起 `Friend::flushfriend()`；本次测试没有发送或接收文件列表 PDU。因此，File 页按需刷新没有回归，**不得回退**。
2. `Client/friend.cpp:35-39` 的 `Friend::flushFriend_LW()` 是当前唯一直接在登录后的响应处理链中对 `QListWidget` 执行 `clear()` 和 `addItems()` 的入口。
3. 崩溃链为：`FLUSHFRIEND_RESPOND` → `MsgHandler::handlFlushFriend()` → `Friend::flushFriend_LW()` → `QListWidget::clear()/addItems()` → 延迟布局事件崩溃。
4. `Friend` 是 `Index` 的 `QStackedWidget` 当前页（`Client/index.ui:76-82`）；所以“仅在隐藏页不更新 UI”不足以覆盖本次场景。
5. Qt Widgets 正常情况下允许更新可见 `QListWidget`。现有证据能证明“该直接更新路径触发崩溃”，但**尚不足以把 Qt 内部延迟布局竞争断言为唯一根因**；第三轮必须保留可诊断证据，不能再把未经验证的推测写成已证实事实。

## 二、裁决

### 裁决 1：立即修复，维持严重等级

`BUG-TST-CLIENT-001` 维持 **严重**、**重新打开**。它阻塞 Client 登录后的首屏及后续动态测试，不能延后，也不能接受风险。

### 裁决 2：采纳“统一的缓存 + 页面激活 + 合并排队 UI 重建”模式

对 **Friend 和 File 两个列表组件同时实施同一行为契约**，而不是仅把 Friend 复制成 File 当前实现：

1. 网络响应处理函数只替换模块拥有的数据快照，不能直接写 `QListWidget`。
2. 列表页不可见时，仅保留“待刷新”状态；页面可见后再请求一次 UI 重建。
3. 列表页可见时，也必须通过**单次合并的事件队列 UI 重建**执行，不能在 PDU 响应调用栈内直接 `clear()` / `addItems()`。
4. 必须用 `m_uiRefreshQueued`（或等价状态）防止多个响应/显示事件排入重复重建；排队回调执行时重新检查可见性与待刷新标志。
5. 排队回调必须绑定组件生命周期（Qt 5.14 可使用 `QTimer::singleShot(0, this, lambda)` 或等价的 context-bound queued invocation），析构后不得访问悬空 `this`。
6. `showEvent()` 仅负责请求已缓存数据的重建，**不直接**重建列表。

这不是单纯的“再包一层 QTimer”时序掩盖：数据接管、可见性门控、合并调度与生命周期绑定共同构成 UI 边界；此前否决的是没有上述边界的 File 单点延时方案。

### 裁决 3：本轮不抽取公共基类/模板/工具类

**不建立公共 UI 列表基类。** Friend 保存 `QStringList`，File 持有并释放 `QList<FileInfo*>`，列表项的构造、图标及数据所有权不同。现在抽象会改变两个模块边界、扩大回归面且不能解决根因。

允许两个组件采用相同命名和状态机（`m_pendingUiRefresh`、`m_uiRefreshQueued`、`scheduleUiRefresh()`、`rebuild…List()`、`showEvent()`），把它作为明确的**局部模式**。待动态回归通过且第三个同类组件确有需求时，再由架构师评估抽取。

### 裁决 4：同步加固 File，避免第三轮把故障迁移到文件页

File 当前实现可在“可见”响应路径直接调用 `rebuildFileList()`。该路径尚未得到本轮动态验证，且与 Friend 崩溃模式相同。因此必须按本决策的统一调度模式改造 File；这属于对已验证 File 数据/UI 分离的**补全**，不是回退或替换。

## 三、工程实施边界

### 允许修改

| 文件 | 必须修改内容 |
|------|--------------|
| `Client/friend.h` | 引入 `QShowEvent`、好友缓存、pending/queued 标志、`showEvent()`、私有调度与重建方法声明。 |
| `Client/friend.cpp` | 将 `flushFriend_LW()` 改为只接管数据并请求 UI 刷新；实现合并排队的安全重建。 |
| `Client/file.h` | 增加 UI 重建排队状态与调度方法声明。 |
| `Client/file.cpp` | 保留文件数据接管与所有权释放；把可见路径的直接重建改为合并排队重建。 |

### 禁止修改

- 不修改 `Client/msghandler.cpp` 中“登录后只刷新好友列表”的 File 按需刷新策略。
- 不修改 `Client/index.cpp` 文件页激活后按需请求文件列表的策略。
- 不修改 PDU 枚举、`PDU` / `FileInfo` 布局、Client/Server 协议、Server 代码、数据库、`.ui` 文件或外部依赖。
- 不把两个组件重构为继承公共基类/模板；不进行无关格式化或命名清理。
- 不将 `flushFriend_LW()` / `updateCurrfile()` 变为在网络处理栈内直接操作 `QListWidget`。

## 四、必要的诊断与验证

工程师必须保留最小诊断日志（例如“缓存版本、页面是否可见、刷新请求入队、实际重建开始/结束”），不得输出用户名、密码、文件完整路径或 PDU 原始内容。日志用于测试师确认 UI 写入不再发生在响应调用栈内。

### 自检完成条件

1. Client Debug 构建成功；无新增编译警告或外部依赖。
2. 静态确认 `Friend::flushFriend_LW()` 与 `File::updateCurrfile()` 不包含 `clear()`、`addItem()` 或 `addItems()`。
3. 静态确认两组件的 `showEvent()` 不直接调用列表控件写入，而是请求合并排队刷新。
4. 静态确认每个排队回调均有组件 context/生命周期保护，且重复请求最多排入一个待执行 UI 重建。
5. 静态确认登录路径仍不触发 `FLUSHFILE_REQUEST`。
6. 交付 Bug 查找师二次审查时，提供结构化变更摘要、状态机说明和上述静态证据。

### 后续质量门禁

- Bug 查找师先进行严重缺陷的二次静态审查。
- 静态审查通过后，测试师与架构师更新/确认 `project-docs/test-strategy.md`，再执行：登录好友页、首次进入文件页、网络响应期间切页、好友/文件连续刷新、10 次好友/文件往返切换。
- 任何一次仍出现 `pure virtual method called`，停止补丁式循环，保存日志并进入调试器获取 C++ 栈；不得直接发起第四次猜测性修改。

## 五、影响与风险

- **直接影响**：Client Friend/File 列表的 UI 更新时机。
- **不影响**：登录协议、Server、数据模型和文件数据所有权规则。
- **风险等级**：严重；当前 Client 动态质量门禁不通过，项目不允许交付。
- **已关闭范围**：R05 File 的“登录后不发文件刷新请求”结论继续有效；但 `BUG-TST-CLIENT-001` 作为整体缺陷仍未修复。

## 六、相关事实来源

- `project-docs/defects/BUG-TST-CLIENT-001.md`
- `project-docs/defects/README.md`
- `project-docs/code-map.md`
- `project-docs/decisions/README.md`
- `project-docs/test-strategy.md`（静态审查通过后，由架构师与测试师共同更新）

## 裁决时间

2026-07-26
