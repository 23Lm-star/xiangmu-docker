# Client UI 尺寸重构 + 全局 QSS 深色主题 —— 静态审查报告

> **审查日期**：2026-07-27
> **审查 Agent**：Bug 查找师
> **上游 Agent**：项目工程师
> **审查范围**：8 个 .ui 文件、style.qss、config.qrc、main.cpp、client.cpp、index.cpp、simplecaptcha.cpp、bg_main.jpg、bg_index.jpg

---

## 审查方法

按分层审查规则执行三层递进：

| 层次 | 审查焦点 | 结果 |
|------|---------|------|
| 第一层 | 接口一致性（.ui XML 结构、资源引用、与 interfaces.md 对齐） | ⚠️ 发现 2 处阻塞 |
| 第二层 | 变更影响分析（QSS→运行时样式冲突、资源完整性） | ⚠️ 发现 1 处严重 + 2 处一般 |
| 第三层 | 深度逻辑审查（尺寸匹配、内存安全、加载动画完整性） | ⚠️ 发现 1 处轻微 |

**第三层触发条件**：第一、二层发现多处可疑问题，涉及核心 UI 尺寸匹配逻辑。

---

## 缺陷清单（按严重程度降序）

### 🔴 阻塞级

---

#### BUG-REV-CLIENT-001：client.ui 尺寸与构造函数 setFixedSize 严重不匹配

- **缺陷编号**：`BUG-REV-CLIENT-001`
- **严重等级**：**阻塞**
- **涉及模块**：Client 端，`client.ui` + `client.cpp`
- **代码位置**：
  - `client.ui:9-10` → geometry 为 **800×550**
  - `client.cpp:41` → `setFixedSize(600,420)`
  - `client.ui:184-187` → loadingLabel geometry 为 **800×550**
- **触发条件**：程序启动即触发（构造函数第 41 行）
- **问题说明**：

  `client.ui` 设计尺寸 800×550，但 `Client` 构造函数第 41 行 `setFixedSize(600,420)` 将实际窗口强制缩为 600×420。这导致三重后果：
  1. **窗口内容截断**：UI 布局按 800×550 设计（左右 margin 各 60，总宽度 = 60 + 控件 + 60 ≈ 800），在 600×420 窗口中控件被挤压/截断
  2. **loadingLabel 溢出**：loadingLabel 几何固定为 800×550，超出实际窗口边界，加载动画无法正确覆盖窗口
  3. **背景图浪费**：`bg_main.jpg` 分辨率为 800×550，在 600×420 窗口中会被缩放，且上下边距不协调

- **预期行为**：`setFixedSize` 应与 `.ui` 中 geometry 一致（800×550），或两者统一为同一尺寸
- **实际行为**：UI 文件与代码存在 200px 宽度差、130px 高度差
- **影响范围**：Client 登录/注册界面整体布局、登录加载动画显示、背景图渲染
- **判断依据**：UI 尺寸重构的目标即为统一尺寸，但工程师未同步更新 `client.cpp` 中的 `setFixedSize`
- **修正建议**：将 `client.cpp:41` 修改为 `setFixedSize(800, 550)`，使三者（.ui geometry、setFixedSize、bg_main.jpg）对齐
- **建议验证方式**：启动 Client，检查窗口大小是否为 800×550；触发登录/注册，确认 loadingLabel 完整覆盖窗口
- **当前状态**：已确认，待修复

---

#### BUG-REV-CLIENT-002：index.ui 尺寸与构造函数 setFixedSize 严重不匹配

- **缺陷编号**：`BUG-REV-CLIENT-002`
- **严重等级**：**阻塞**
- **涉及模块**：Client 端，`index.ui` + `index.cpp`
- **代码位置**：
  - `index.ui:9-10` → geometry 为 **1200×800**
  - `index.cpp:10` → `setFixedSize(1000,650)`
- **触发条件**：登录成功后跳转到 Index 界面时触发
- **问题说明**：与 CLIENT-001 同类问题。`index.ui` 设计 1200×800，但代码强制为 1000×650，差 200×150 像素。`bg_index.jpg` 分辨率 1200×800 与窗口不匹配。
- **预期行为**：`setFixedSize` 应为 1200×800
- **实际行为**：1000×650
- **影响范围**：Index 主界面整体布局、侧边导航按钮和 QStackedWidget 显示区域、背景图
- **判断依据**：同 CLIENT-001
- **修正建议**：将 `index.cpp:10` 修改为 `setFixedSize(1200, 800)`
- **建议验证方式**：登录后检查 Index 窗口大小是否为 1200×800；确认好友列表和文件列表布局正常
- **当前状态**：已确认，待修复

---

### 🟠 严重

---

#### BUG-REV-CLIENT-003：userinformation.ui windowIcon 使用本地绝对路径

- **缺陷编号**：`BUG-REV-CLIENT-003`
- **严重等级**：**严重**
- **涉及模块**：Client 端，`userinformation.ui`
- **代码位置**：`userinformation.ui:18` → `C:/Users/王晨扬/Downloads/个人信息_o.png`
- **触发条件**：在其他机器上部署运行
- **问题说明**：`windowIcon` 引用了一个硬编码的本机绝对路径，该图标未嵌入 `.qrc` 资源文件。部署到其他机器或用户目录结构不同时，图标将丢失，Qt 会静默使用默认图标。
- **预期行为**：图标应嵌入 `config.qrc` 并通过 `:/` 前缀引用
- **实际行为**：引用本机临时下载目录的图标文件
- **影响范围**：UserInformation 窗口图标缺失
- **判断依据**：
  - `interfaces.md` 未定义图标规范（属于客户端 UI 约定）
  - 部署兼容性是基本质量要求
  - 其余 7 个 .ui 文件的 windowIcon 均使用 `:/` 资源路径
- **修正建议**：
  1. 将 `个人信息_o.png` 复制到 `Client/` 目录
  2. 在 `config.qrc` 中注册 `<file>个人信息_o.png</file>`
  3. 将 icon 引用改为 `<normaloff>:/个人信息_o.png</normaloff>`
- **建议验证方式**：在另一台机器或不同用户目录下运行，确认 UserInformation 窗口图标正常显示
- **当前状态**：已确认，待修复

---

### 🟡 一般

---

#### BUG-REV-CLIENT-004：SimpleCaptcha 硬编码浅色样式表与全局深色主题冲突

- **缺陷编号**：`BUG-REV-CLIENT-004`
- **严重等级**：一般
- **涉及模块**：Client 端，`simplecaptcha.cpp`
- **代码位置**：`simplecaptcha.cpp:13-22`
- **触发条件**：用户在任何界面使用本地验证码控件时
- **问题说明**：`SimpleCaptcha` 构造函数中硬编码了浅色样式：
  ```cpp
  m_label->setStyleSheet(
      "QLabel {"
      "   background-color: #f0f0f0;"       // 浅灰底色 — 与深色主题冲突
      "   border: 1px solid #ccc;"           // 浅灰边框
      "   font-size: 18px;"
      "   color: #333;"                      // 深灰文字 — 在深色背景下不可见
      "}"
  );
  ```
  全局 QSS 主题设定所有 QLabel 为 `color: #c8d8f0; background: transparent;`。此处 `setStyleSheet` 直接覆写了全局样式，使用了 `#f0f0f0` 浅底色和 `#333` 暗文字。虽然 `SimpleCaptcha` 当前未在登录界面中使用（注释中 h 代码有编译但未实例化），但一旦使用将造成视觉冲突。
- **预期行为**：验证码控件样式应与全局深色主题协调
- **影响范围**：SimpleCaptcha 控件外观（如果在未来启用）
- **判断依据**：全局 QSS 主题要求所有控件使用深色调，硬编码浅色样式破坏主题一致性
- **修正建议**：将验证码样式改为深色主题友好配色，例如：
  ```qss
  background: rgba(15, 25, 45, 200);
  border: 2px solid #2a4a6a;
  border-radius: 6px;
  color: #c8d8f0;
  ```
  或者移除 `setStyleSheet`，让其继承全局 QSS（但全局 QSS 的 `QLabel` 是透明背景，可能需调整全局 QSS 的 `QLabel` 选择器粒度）
- **建议验证方式**：创建 SimpleCaptcha 实例并嵌入界面，确认颜色与整体深色主题协调
- **当前状态**：已确认，待修复

---

#### BUG-REV-CLIENT-005：client.cpp showLoadingAnimation 中 effect 对象内存泄漏

- **缺陷编号**：`BUG-REV-CLIENT-005`
- **严重等级**：一般
- **涉及模块**：Client 端，`client.cpp`
- **代码位置**：`client.cpp:375-404`（`showLoadingAnimation` 函数）
- **触发条件**：多次触发登录/注册操作（每次调用 `showLoadingAnimation` 创建新的 `QGraphicsOpacityEffect`）
- **问题说明**：

  ```cpp
  QGraphicsOpacityEffect* effect = new QGraphicsOpacityEffect(this);  // parent = Client (单例)
  ui->loadingLabel->setGraphicsEffect(effect);
  // ... 动画 ...
  // fadeOut finished:
  ui->loadingLabel->setGraphicsEffect(nullptr);  // 解除关联，但 effect 对象仍存活
  ```

  `effect` 的 parent 是 `this`（Client 单例，生命周期为整个应用），`setGraphicsEffect(nullptr)` 只是解除与 loadingLabel 的关联，`effect` 对象本身不会被 delete。每次调用 `showLoadingAnimation` 都会泄漏一个 effect 对象。Client 单例存活期间，泄漏对象累积。
- **预期行为**：动画完成后 effect 对象应被释放
- **影响范围**：长期运行后内存逐步增长（每次登录/注册操作泄漏约 1 个 `QGraphicsOpacityEffect`）
- **判断依据**：Qt 对象树中 parent=this 的对象只在父对象析构时删除，此处手动解除但不删除
- **修正建议**：方案一：在 fadeOut lambda 的 `setGraphicsEffect(nullptr)` 之后调用 `effect->deleteLater()`。方案二：fadeOut 连接 `effect, &QObject::deleteLater` 作为清理信号。推荐方案一：
  ```cpp
  ui->loadingLabel->setGraphicsEffect(nullptr);
  effect->deleteLater();
  ```
- **建议验证方式**：使用 Valgrind 或 Qt Creator 内存分析器，连续触发 10 次登录操作，确认无持续内存增长
- **当前状态**：已确认，待修复

---

### 🔵 轻微

---

#### BUG-REV-CLIENT-006：images.qrc 为空白骨架文件

- **缺陷编号**：`BUG-REV-CLIENT-006`
- **严重等级**：轻微
- **涉及模块**：Client 端，`images.qrc`
- **代码位置**：`images.qrc:1-2` → `<!DOCTYPE RCC><RCC version="1.0"/>`
- **触发条件**：其他开发者查看项目结构时
- **问题说明**：`images.qrc` 是一个空骨架文件，不含任何资源声明。所有实际资源均在 `config.qrc` 中。该文件容易造成混淆，且 `.pro` 文件中 `RESOURCES` 仅引用了 `config.qrc`。
- **预期行为**：要么删除空文件，要么将其内容迁移到 `config.qrc` 并移除
- **影响范围**：无运行时影响，仅代码维护可读性问题
- **判断依据**：空资源文件不应保留在项目中
- **修正建议**：删除 `images.qrc` 或在其中声明图片资源并加入 `.pro` 的 `RESOURCES`
- **建议验证方式**：编译通过无误
- **当前状态**：已确认，待修复

---

#### BUG-REV-CLIENT-007：main.cpp QSS 加载失败时静默继续

- **缺陷编号**：`BUG-REV-CLIENT-007`
- **严重等级**：轻微
- **涉及模块**：Client 端，`main.cpp`
- **代码位置**：`main.cpp:13-18`
- **触发条件**：`style.qss` 未正确嵌入到 `config.qrc` 或资源文件损坏
- **问题说明**：
  ```cpp
  QFile qssFile(":/style.qss");
  if (qssFile.open(QFile::ReadOnly)) {
      // 加载成功
  }
  // 打开失败：静默继续，无任何日志
  ```
  QSS 是本次重构的核心交付物，加载失败意味着整个深色主题丢失，但没有日志提示。调试时无法快速定位原因。
- **预期行为**：加载失败时至少输出 `qWarning() << "QSS 主题加载失败"` 日志
- **影响范围**：调试和问题排查效率
- **判断依据**：关键资源加载失败应产生可观测的诊断信息
- **修正建议**：添加 else 分支：
  ```cpp
  } else {
      qWarning() << "无法加载全局样式表 :/style.qss";
  }
  ```
- **建议验证方式**：临时改名 style.qss 后运行，确认 qWarning 日志输出
- **当前状态**：已确认，待修复

---

## 无缺陷项确认（正面审计）

| 检查项 | 状态 | 备注 |
|--------|------|------|
| 8 个 .ui 文件 XML 结构完整性 | ✅ 全部合法 | 均有 `<?xml>`, `<ui>`, `<class>`, `<widget>`, `</ui>` |
| 旧 styleSheet 清除 | ✅ | client.ui 的 styleSheet 设为空字符串 `<string notr="true"/>`；其余 7 个无 styleSheet |
| style.qss 语法 | ✅ | 语法正确，无多余逗号或未闭合括号 |
| style.qss 注册到 config.qrc | ✅ | `config.qrc:15` 已注册 |
| main.cpp QSS 加载 | ✅ | 通过 `QApplication::setStyleSheet` 正确应用全局样式 |
| bg_main.jpg 存在 | ✅ | 76,901 bytes，分辨率 800×550 |
| bg_index.jpg 存在 | ✅ | 173,011 bytes，分辨率 1200×800 |
| 背景图在 QSS 中引用正确 | ✅ | `url(:/bg_main.jpg)` 和 `url(:/bg_index.jpg)` |
| loadingLabel 保留 | ✅ | `client.ui:181-196` 保留 |
| QSS 与验证码冷却倒计时 setText 无冲突 | ✅ | `resetCaptchaButton`/`onCaptchaCooldownTick` 仅使用 setText/setEnabled |
| .pro 文件 FORMS 和 RESOURCES 声明 | ✅ | 8 个 .ui 均在 FORMS 中；config.qrc 在 RESOURCES 中 |
| config.qrc 资源完整性 | ✅ | 13 个文件全部使用相对路径引用 |

---

## 样式冲突分析

| 冲突源 | 冲突目标 | 评估 |
|--------|---------|------|
| `showLoadingAnimation` → `ui->loadingLabel->setStyleSheet()` | 全局 QSS `QLabel#loadingLabel` | **无实质冲突**：动态样式表覆盖全局样式是预期行为（动画需动态变更颜色）。但硬编码颜色值而非使用主题色变量，后续主题变更时需同步修改两处。 |
| `SimpleCaptcha` → `m_label->setStyleSheet()` | 全局 QSS `QLabel` | **有冲突**（已记录为 BUG-REV-CLIENT-004） |

---

## 编译结果验证

根据提交信息：构建日志显示零新增错误，仅预存在的警告（`msghandler.cpp` sign-compare, `file.cpp` unused-parameter）。审查未发现引入编译错误的新增代码。

---

## 总体结论

**审查结论：不通过，需修复 2 个阻塞缺陷后方可移交测试。**

核心问题在于 .ui 文件尺寸已按照「UI 尺寸重构」目标修改，但对应的 `.cpp` 构造函数中的 `setFixedSize` 未同步更新，导致窗口实际尺寸与设计尺寸不一致。这直接影响背景图渲染、布局完整性和加载动画覆盖层。

**阻塞项修复后预期可达到测试移交标准。**

---

## 剩余风险

1. **高 DPI 适配**：未检查在不同缩放比例下的表现（100%/125%/150% DPI）
2. **cross-platform 字体回退**：QSS 指定了 `"Microsoft YaHei"` 字体，Linux 平台无此字体时依赖 Qt 字体回退机制
3. **主题可切换性**：当前通过 `QApplication::setStyleSheet` 全局应用，无法运行时切换。如果未来添加"日间模式"，需要重构为可动态替换的样式表管理

---

## 下游移交

缺陷记录已更新至 `project-docs/defects/README.md`。修复完成后，测试师可按以下可测试范围进行验证。
