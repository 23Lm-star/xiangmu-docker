# R07 调试证据报告：BUG-TST-CLIENT-001

> **编号**：R07-20260726-ENG-BUG-TST-CLIENT-001
> **角色**：项目工程师
> **阶段**：证据化调试（R07 裁决第一阶段）
> **日期**：2026-07-26

---

## 一、调试环境

| 项目 | 值 |
|------|-----|
| 调试器 | GDB 8.1 (MinGW 7.3.0, x86_64-w64-mingw32) |
| 调试目标 | `Client/debug/Client.exe` (Debug 构建, `-g` 符号) |
| 可执行文件路径 | `E:\qt\xiangmu\Client\debug\Client.exe` |
| Qt 版本 | 5.14.2 mingw73_64 |
| 服务端 | Release 构建, `Server/release/Server.exe` |

## 二、断点设置与符号可用性

### `__cxa_pure_virtual` 符号
- **结果**：❌ 不可用作命名断点
- GDB 输出：`Function "__cxa_pure_virtual" not defined.`
- 原因：MinGW 的 libstdc++ 中 `__cxa_pure_virtual` 为内联标记，不作为可命名的独立函数符号导出
- 替代方案：使用 `catch signal SIGABRT` 和 `break abort`

### `break abort` 断点
- **结果**：✅ 可设置
- 地址：`Breakpoint at 0x417d68`

### `catch signal SIGABRT`
- **结果**：✅ 可设置
- 成功捕获到 SIGABRT 信号（见第三部分）

### `Client::showLoadingAnimation` 断点
- **结果**：✅ 可设置
- 地址：`Breakpoint 2 at 0x40a747: file client.cpp, line 376`

## 三、调试运行结果

### 运行 1（GDB v1 脚本）
- **触发**：break abort + run
- **结果**：❌ 未触及 abort 断点
- **退出**：进程正常退出（`exited normally`）
- **分析**：用户未登录，Client 窗口被关闭，无崩溃

### 运行 2（GDB v2 脚本，含 catch signal SIGABRT）
- **触发**：catch signal SIGABRT + break abort
- **结果**：✅ **捕获到 SIGABRT 信号**，但非登录崩溃
- **崩溃调用栈**：
  ```
  #0  QAbstractSocket::abort()                         Qt5Network.dll
  #1  QAbstractSocket::~QAbstractSocket()              Qt5Network.dll
  #2  Client::~Client()                                client.cpp:58  (delete ui)
  #3  __tcf_0()                                        client.cpp:86
  #4  msvcrt!_initterm_e
  #5  __tmainCRTStartup
  #6  WinMainCRTStartup
  ```
- **根因**：**静态析构崩溃**——Client 单例在程序退出时析构，`delete ui` 触发 `QTcpSocket sorket` 成员析构，该析构调用 `QAbstractSocket::abort()`
- **首个用户代码帧**：`Client::~Client()` at `client.cpp:58`
- **与登录崩溃的关系**：此栈不涉及 `showLoadingAnimation`、`QGraphicsOpacityEffect`、`QPropertyAnimation`，是一个独立的析构路径问题

### 运行 3（GDB v3 脚本，含 showLoadingAnimation 断点）
- **触发**：catch signal SIGABRT + break Client::showLoadingAnimation
- **结果**：❌ 两个断点均未命中
- **退出**：进程以 exit code 1 正常退出
- **分析**：`showLoadingAnimation` 断点从未触发，确认用户未完成登录流程

## 四、关键发现

### 4.1 登录崩溃调用栈无法获取（非技术原因）
- 三次调试运行中，用户均未完成 GUI 登录操作
- Debug Client 可正常启动、连接 Server，但未触发登录流程
- `Client::showLoadingAnimation()` 断点从未命中

### 4.2 静态分析确认的高危代码
`Client/client.cpp:375-405` (`showLoadingAnimation`) 存在双重删除：

```cpp
// 第 398-399 行：
ui->loadingLabel->setGraphicsEffect(nullptr);  // Qt 接管所有权，此行 DELETE effect
effect->deleteLater();                          // 再次删除已释放的 effect ← 双重删除！
```

Qt 5 官方文档明确：`QWidget::setGraphicsEffect()` 接管 effect 所有权；
设置新 effect（或 nullptr）时会删除旧 effect。[1]

### 4.3 时序重叠
- 登入动画：淡入 800ms + 停留 600ms + 淡出 400ms = **1800ms**
- 主界面切换定时器：**1500ms**
- 好友列表刷新与动画销毁处于同一时间窗（测试报告中 `rebuildFriendList()` 完成日志后崩溃）

### 4.4 析构路径新发现
- 第二个 GDB 运行发现静态析构崩溃：`Client::~Client()` → `QAbstractSocket::abort()`
- 这是程序退出时的析构问题，与登录流程独立
- 与 BUG-TST-CLIENT-001 的登录崩溃现象不同（退出时机不同）

## 五、根因判断

### 主要判断
`Client::showLoadingAnimation()` 中的 `effect->deleteLater()` (client.cpp:399) 是明确的重复生命周期操作。`QWidget::setGraphicsEffect(nullptr)` 在第 398 行已删除 effect 对象，第 399 行的 `deleteLater()` 对已释放内存进行操作，触发未定义行为。

该动画的销毁时序（1800ms 总长）与好友列表刷新（登录后 1500ms 切换主界面）重叠，当 `effect->deleteLater()` 在 effect 已被 widget 删除后执行时，可能导致 `pure virtual method called` (vtable 破坏) 或 SIGABRT。

### 证据强度
- **静态代码审查**：确认 (client.cpp:397-399)
- **Qt 文档**：`QWidget::setGraphicsEffect()` 明确接管所有权 [1]
- **时序证据**：动画时长与崩溃窗口重叠（来自 R06 测试报告）
- **缺失项**：因用户未登录，未取得登录崩溃的实时调用栈

## 六、修复建议

### 最小修复
仅修改 `Client/client.cpp` showLoadingAnimation() — 删除第 399 行的 `effect->deleteLater()`，
在 `setGraphicsEffect(nullptr)` 前保存 `loadingLabel` 指针用于 lambda 内访问。

### 禁止扩展修改
不得修改 Friend/File、QStackedWidget、协议、Server、.ui 或任何其他文件。

## 七、参考文献

[1] Qt 5.15 文档: QWidget::setGraphicsEffect()
    "QWidget takes ownership of the effect."
    https://doc.qt.io/qt-5/qwidget.html#setGraphicsEffect

---

*报告完成时间：2026-07-26*
