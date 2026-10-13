# 决策记录：BUG-TST-CLIENT-001 第三次回归失败后的证据化根因裁决

> 背景：R06 第三次动态回归确认列表重建完成后进程仍以 `pure virtual method called` / exit code 3 终止。R05 已规定再次失败后不得继续猜测性补丁。

## 一、裁决结论

1. `BUG-TST-CLIENT-001` 维持 **严重、重新打开**；Client 动态质量门禁继续不通过，不允许交付。
2. **否决直接实施 A–D**：
   - A `Qt::QueuedConnection` 与当前 context-bound `QTimer::singleShot(0, this, ...)` 都是排队调用，只换排队机制不能解释或消除纯虚调用；
   - B `QApplication::processEvents()` 会引入可重入，可能放大生命周期问题；
   - C 当前 `index.ui` 已明确 `currentIndex=0`，不存在“setupUi 前设置 UI 生成属性”的可靠实施点，且 Friend 本来就是当前页；
   - D `QListView + QStringListModel` 是较大 UI 重构，在根因未知时会扩大回归面，且不能修复列表之外的对象生命周期错误。
3. 先执行**证据化调试**，在 `pure virtual method called`/`__cxa_pure_virtual` 处取得完整原生调用栈；只有调用栈命中明确对象链后才允许修改代码。
4. 本轮优先验证 `Client::showLoadingAnimation()` 的 `QGraphicsOpacityEffect` 所有权链，而不是继续修改 Friend/File。

## 二、关键新事实

实际代码存在以下高危链：

```cpp
QGraphicsOpacityEffect* effect = new QGraphicsOpacityEffect(this);
ui->loadingLabel->setGraphicsEffect(effect);
...
connect(fadeOut, &QPropertyAnimation::finished, this, [this, effect]() {
    ui->loadingLabel->hide();
    ui->loadingLabel->setGraphicsEffect(nullptr);
    effect->deleteLater();
});
```

Qt 5 官方 `QWidget::setGraphicsEffect()` 契约明确：`QWidget` 接管 effect 的所有权；若已有 effect，设置新 effect 时会删除旧 effect。当前代码在移除 effect 后仍对同一裸指针调用 `deleteLater()`，存在重复销毁或析构后访问风险。

时序也与现象吻合：淡入 800ms + 停留 600ms + 淡出 400ms；主界面切换定时为 1500ms。好友列表刷新与 effect 销毁处于同一时间窗，因此“UI 刷新：完成”只能说明最后成功日志，不能证明下一条执行必然是 QListWidget 延迟布局。

## 三、事实边界修正

- 已证实：列表重建完成日志之后发生 pure-virtual abort。
- 未证实：`QListWidget::addItems()` 触发的 Qt 延迟布局是崩溃根因。
- 高优先级待证假设：登录动画 effect/animation 所有权错误在相同时序窗口触发终止。
- 若调用栈不命中动画链，应继续沿栈检查收包长度校验、对象生命周期及内存破坏；不得回到列表时序补丁。

## 四、工程执行规格

### 阶段 1：只诊断，不改业务代码

1. 使用 **Client Debug** 构建；保留符号，不以 Release 栈作最终证据。
2. 在 MinGW gdb 中对 `__cxa_pure_virtual` 设置断点；若符号不可解析，先在 `abort` 设置断点，并记录断点可解析情况。
3. 以原 TC-1 登录路径复现；中断后输出：
   - `bt full`
   - `thread apply all bt full`
   - 当前线程、最靠近用户代码的首个栈帧及其文件/行号
4. 同步保存从“登录动画开始”到崩溃的完整日志，必须包含动画阶段、Friend 刷新阶段和时间戳；不得记录密码或 PDU 原文。
5. 诊断证据归档到 `project-docs/bug-reports/R07-20260726-ENG-BUG-TST-CLIENT-001-调试证据.md`。

### 阶段 2：按栈条件修复

#### 若栈命中 `QGraphicsEffect` / `QPropertyAnimation` / `Client::showLoadingAnimation`

仅允许修改 `Client/client.cpp`（如声明持久成员确有必要，可连带 `Client/client.h`）：

- 删除 `effect->deleteLater()` 这一重复生命周期操作；effect 只由安装它的 widget 管理。
- 动画对象继续使用 `QAbstractAnimation::DeleteWhenStopped`，不得再手动删除同一动画。
- 避免在 lambda 中跨异步阶段持有不受保护的裸 effect 指针；完成回调只通过 `ui->loadingLabel->graphicsEffect()` 或受 Qt 上下文保护的对象访问完成必要操作。
- 若成功动画会重复触发，启动新动画前必须停止/清理旧动画状态，保证同一 label 同时只有一个 effect/动画链。
- 不修改 Friend/File、`QStackedWidget`、协议、Server、`.ui` 或外部依赖。

#### 若栈不命中动画链

- **不得实施任何代码修复。**
- 将真实首个用户代码栈帧、对象地址/类型和可能的破坏源写入证据报告，状态保持重新打开，重新提交架构师裁决。

## 五、验证要求

工程师仅执行构建和调试自检，不代替测试师宣布动态回归通过：

1. Client Debug 构建成功，保留调试符号。
2. 原 TC-1 在调试器下至少连续执行 3 次；若实施动画修复，3 次均不得命中 `__cxa_pure_virtual`/`abort`。
3. 登录动画必须完整结束；登录窗口隐藏、Index 显示、好友列表展示均正常。
4. 静态确认 Friend/File 第三轮数据/UI 边界未被回退。
5. 提交结构化变更摘要和调试证据；缺陷由工程师开始时置为“修复中”，完成后置为“待复查”。
6. 严重缺陷修复必须先交 Bug 查找师二次静态审查；通过后由架构师与测试师共同确认 `test-strategy.md`，再执行最后一轮动态回归。

## 六、风险与影响

- **当前风险等级**：严重。
- **直接影响**：Client 登录成功动画、主界面切换时序窗口。
- **禁止扩大影响**：Friend/File 列表架构、PDU 协议、Server、数据模型、UI 文件。
- **交付状态**：质量门禁不通过，不允许交付。

## 七、事实来源

- `project-docs/defects/BUG-TST-CLIENT-001.md`
- `project-docs/test-reports/R06-20260726-TST-BUG-TST-CLIENT-001.md`
- `project-docs/decisions/R05-20260726-ARC-BUG-TST-CLIENT-001-第二次回归裁决.md`
- `Client/client.cpp`
- Qt 5.15 QWidget 官方文档：`QWidget::setGraphicsEffect()`（Qt 5.14 同系列所有权契约）

## 裁决时间

2026-07-26
