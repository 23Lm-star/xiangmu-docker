# 二次静态审查报告 — UI/QSS 重构修复核查

> **审查时间**：2026-07-27  
> **审查人**：Bug 查找师  
> **批次号**：REV-20260727-R2-UI-QSS-RECHECK  
> **上游**：项目工程师  
> **审查类型**：二次审查（8 项缺陷修复确认）

---

## 审查结论总览

| 项目 | 结果 |
|------|------|
| 审查缺陷数 | 8 项 |
| 通过 | **8 项** ✅ |
| 发现问题 | **0 项** |
| 需要深度审查 | 1 项（BUG-REV-CLIENT-005 — 指针安全，进入第三层分析后确认安全） |

---

## 逐项审查记录

### 1. BUG-REV-BUILD-001 — ✅ 已修复

| 项目 | 内容 |
|------|------|
| 原始问题 | 构建产物(11:39)滞后源文件(16:02)，exe 未含 QSS 变更 |
| 修复方式 | 全量重建 |
| 验证结果 | `Client.exe` 16:26 > 所有源文件（client.ui 16:21, client.cpp 16:25, bg_main.jpg 16:21, style.qss 16:02, 等） |
| 判断 | **✅ 通过** |

### 2. BUG-REV-QSS-001 — ✅ 已修复

| 项目 | 内容 |
|------|------|
| 原始问题 | bg_main.jpg(800×550) 在 600×420 窗口中居中裁剪 |
| 修复方式 | PIL 居中裁剪为 600×420 |
| 验证结果 | `python -c "from PIL import Image; img=Image.open('...'); print(img.size)"` → `(600, 420)` |
| 判断 | **✅ 通过** — 图片尺寸精确匹配窗口尺寸，QSS 中 `background-position: center` 无副作用 |

### 3. BUG-REV-CLIENT-001 — ✅ 已修复

| 项目 | 内容 |
|------|------|
| 原始问题 | client.ui geometry(800×550) 与 setFixedSize(600,420) 不匹配 |
| 修复方式 | 同步主 widget 和 loadingLabel geometry 为 600×420 |
| 验证结果 | client.ui:7-11 → `<width>600</width><height>420</height>`; L182-188 → loadingLabel 同尺寸; client.cpp:41 → `setFixedSize(600,420)` |
| 判断 | **✅ 通过** — 三者完全一致 |

### 4. BUG-REV-CLIENT-003 — ✅ 已修复

| 项目 | 内容 |
|------|------|
| 原始问题 | userinformation.ui windowIcon 使用 `C:/Users/王晨扬/Downloads/...` 绝对路径 |
| 修复方式 | 删除整个 `<property name="windowIcon">` 块 |
| 验证结果 | userinformation.ui 全文无 `windowIcon` 属性；`<resources/>` 为空 |
| 判断 | **✅ 通过** — 无任何外部路径引用，跨机安全 |

### 5. BUG-REV-CLIENT-004 — ✅ 已修复

| 项目 | 内容 |
|------|------|
| 原始问题 | SimpleCaptcha 硬编码 `#f0f0f0` 背景、`#333` 文字色、`#ccc` 边框，与深色主题冲突 |
| 修复方式 | 仅保留 font-family/font-size/font-weight 字体设置 |
| 验证结果 | simplecaptcha.cpp:13-19 → 样式表仅含 `font-family`, `font-size`, `font-weight` |
| 判断 | **✅ 通过** — 无 background-color/border/color，正确继承全局 QSS |

### 6. BUG-REV-CLIENT-005 — ✅ 已修复（深度审查通过）

| 项目 | 内容 |
|------|------|
| 原始问题 | Lambda 值捕获 effect/anim 指针，setGraphicsEffect(nullptr) 删除后悬空 |
| 修复方式 | Lambda 仅捕获 `this`，通过 `qobject_cast` 从 widget 实时获取效果指针 |
| 触发原因 | 该缺陷涉及指针生命周期管理，进入第三层深度审查 |
| 验证结果 | **生命周期追溯**：
1. `anim` finished → Lambda(仅`this`) → QTimer::singleShot 600ms
2. 回调中 `qobject_cast<QGraphicsOpacityEffect*>(ui->loadingLabel->graphicsEffect())` 获取当前效果
3. nullptr 安全检查 → 创建 fadeOut → fadeOut finished → hide + setGraphicsEffect(nullptr)
4. setGraphicsEffect(nullptr) 删除 effect → DeleteWhenStopped 删除 fadeOut
**无悬空指针路径** ✅ |
| 判断 | **✅ 通过** — qobject_cast 模式正确，生命周期安全 |

### 7. BUG-REV-CLIENT-006 — ✅ 已修复

| 项目 | 内容 |
|------|------|
| 原始问题 | images.qrc 仅 `<RCC version="1.0"/>` 骨架 |
| 修复方式 | 填充所有 13 个资源文件；Client.pro RESOURCES 中添加 images.qrc |
| 验证结果 | images.qrc L2-16 → 13 个 `<file>` 条目；全部文件在磁盘上存在（ls 逐一确认）；Client.pro:62-64 → `RESOURCES += config.qrc images.qrc` |
| 判断 | **✅ 通过** — 资源文件和 .pro 引用均正确 |

### 8. BUG-REV-CLIENT-007 — ✅ 已修复

| 项目 | 内容 |
|------|------|
| 原始问题 | main.cpp QSS 加载失败时静默继续，无提示 |
| 修复方式 | else 分支添加 qWarning() |
| 验证结果 | main.cpp:19 → `qWarning() << "QSS 加载失败: 未找到资源 :/style.qss";` |
| 判断 | **✅ 通过** |

---

## 已知未修复项（与本轮无关）

| 缺陷 ID | 级别 | 描述 | 状态 |
|---------|------|------|------|
| BUG-REV-CLIENT-002 | 阻塞 | index.ui 尺寸(1200×800)与 setFixedSize(1000×650) 不匹配 | 已确认，待修复 |
| BUG-TST-CLIENT-001 | 严重 | Client 登录后 pure-virtual 崩溃 | 修复中 |
| BUG-SEC-001 | 一般 | 无盐 MD5 密码 | 修复中 |
| BUG-NET-001 | 一般 | 无应用层心跳 | 修复中 |
| BUG-DB-003 | 一般 | 数据库断线重连 | 修复中 |

---

## 剩余风险

1. **bg_index.jpg (173KB, 1200×800)** — 未修改，配套 index.ui 尺寸。待 BUG-REV-CLIENT-002 修复时需同步裁剪。
2. **style.qss (16:02)** — 时间戳早于本轮其他修改（16:21~16:25），但本次 QSS 内容无需修改，非风险项。
3. 构建工具链（Qt 5.14.2 MinGW）不在当前审查环境 PATH 中，已通过工程师前置构建验证 + 静态分析确认。
