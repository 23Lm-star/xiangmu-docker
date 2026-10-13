# 修复规格：BUG-TST-CLIENT-001 动画所有权修复

## 1. 总则

基于 `project-docs/bug-reports/R07-20260726-ENG-BUG-TST-CLIENT-001-调试证据.md` 的发现，崩溃根因为 `QGraphicsOpacityEffect` 发生双重释放（double-free）。Qt 文档明确指出，`QWidget::setGraphicsEffect()` 会接管传入的 effect 所有权；若传入 nullptr，则会自动释放先前的 effect。

因此，代码中 `ui->loadingLabel->setGraphicsEffect(nullptr);` 已经销毁了 effect，紧接着调用的 `effect->deleteLater();` 是非法的，引发未定义行为并在时序上和好友列表加载冲突，表现为 `pure virtual method called` 崩溃。

## 2. 变更清单总览

| 文件 | 变更类型 | 范围 | 说明 |
|---|---|---|---|
| `Client/client.cpp` | 修改 | `showLoadingAnimation` | 移除重复的 `deleteLater()` 调用 |

## 3. 执行顺序与详细规格

### 阶段 1：修改 `Client/client.cpp`

**目标文件**：`Client/client.cpp`

**修改位置**：`showLoadingAnimation` 方法内部，`connect` 的 lambda 表达式。

**要求**：
- 删除 `effect->deleteLater();` 这一行。
- 确保不再对 `effect` 进行任何显式的销毁。
- 不修改动画的生成逻辑。

**代码参考**（供比对）：
修改前（约 396-401 行）：
```cpp
    connect(fadeOut, &QPropertyAnimation::finished, this, [this, effect]() {
        ui->loadingLabel->hide();
        ui->loadingLabel->setGraphicsEffect(nullptr);
        effect->deleteLater();  // ❌ 删除此行
        // ... 其他界面切换代码 ...
    });
```
修改后：
```cpp
    connect(fadeOut, &QPropertyAnimation::finished, this, [this]() {
        ui->loadingLabel->hide();
        ui->loadingLabel->setGraphicsEffect(nullptr); // 已自动处理旧的 effect 内存释放
        // ... 其他界面切换代码 ...
    });
```

### 阶段 2：编译验证

**要求**：
- 在 `Client` 目录执行 `qmake && make` 编译通过。

## 4. 禁测与禁改区域

- **绝对禁止修改**：`Friend.cpp`、`File.cpp` 中的列表逻辑。
- **绝对禁止修改**：`QStackedWidget`、协议结构、服务端代码。

## 5. 提交要求
完成修复后，更新缺陷状态为“待复查”，输出交接指令交由 Bug 查找师进行四次审查。
