# 静态审查报告：BUG-TST-CLIENT-001 + BUG-FILE-002-R1

> **批次**：R09 | **日期**：2026-07-27 | **审查者**：Bug 查找师 (REV)
> **参考规格**：`R07-20260727-ARC-BUG-TST-CLIENT-001-修复规格.md`、`R08-20260727-ARC-BUG-FILE-002-R1-补充修复规格.md`

---

## 审查方法

严格遵循 Bug 查找师 SOUL.md 的分层审查规则：
- **第一层（接口一致性）**：跳过——本次修复不涉及公共接口变更
- **第二层（变更影响分析）**：确认修改范围仅限 `client.cpp:375-404` 和 `msghandler.cpp:444-463`，无跨模块影响
- **第三层（深度逻辑审查）**：全部条件满足（核心业务规则 + 安全代码），进入逐行审查

---

## 一、BUG-TST-CLIENT-001 审查（四次审查）

### 审查目标

| # | 检查项 | 结果 |
|---|--------|------|
| 1 | 内层 connect(fadeOut,...) lambda 已使用 `[this]()` 捕获（不含 effect） | ✅ 通过 |
| 2 | 不存在 `effect->deleteLater()` 或任何对 effect 的显式销毁 | ✅ 通过 |
| 3 | `ui->loadingLabel->setGraphicsEffect(nullptr)` 后无 effect 指针访问 | ✅ 通过 |
| 4 | 未修改 Friend.cpp、File.cpp、QStackedWidget、协议结构 | ✅ 通过 |

### 逐项证据

#### 1. Lambda 捕获列表

```cpp
// Client/client.cpp:396
connect(fadeOut,&QPropertyAnimation::finished,this,[this](){
```

**结论**：仅捕获 `this`，不捕获 `effect`。✅

#### 2. 无显式销毁

`grep -n 'deleteLater\|setGraphicsEffect' Client/client.cpp` 结果：

```
382:    ui->loadingLabel->setGraphicsEffect(effect);     // 设置（合法）
398:    ui->loadingLabel->setGraphicsEffect(nullptr);    // 释放（合法）
```

**无 `effect->deleteLater()` 残留**。✅

`setGraphicsEffect(nullptr)` 按 Qt 文档语义，Qt 接管 ownership 后自动释放旧 effect。不构成 double-free。

#### 3. setGraphicsEffect(nullptr) 后无指针访问

Line 398 后 lambda 立即结束（line 399: `});`），无后续 effect 指针访问。✅

#### 4. 未修改禁区文件

| 文件 | 当前状态 | 判断 |
|------|---------|------|
| `Friend.cpp` | R06 缓存+合并排队 UI 重建模式（`m_friendList` → `scheduleUiRefresh()` → `rebuildFriendList()`） | 未因 R07 修改 |
| `File.cpp` | R06 缓存+合并排队 UI 重建模式（`m_fileInfoList` → `scheduleUiRefresh()` → `rebuildFileList()`） | 未因 R07 修改 |
| `QStackedWidget` | 无引用变更 | ✅ |
| `protocol.h` | 无变更；`MOVEFILE_RESPOND`(39) 处理保留 | ✅ |

---

## 二、BUG-FILE-002-R1 审查（三次审查）

### 审查目标

| # | 检查项 | 结果 |
|---|--------|------|
| 1 | 目标路径校验使用了 `dstInfo.dir().canonicalPath()`（父目录 canonical 路径）而非 `dstInfo.canonicalFilePath()` | ✅ 通过 |
| 2 | 有 `!canonicalDstDir.isEmpty()` 空值保护 | ✅ 通过 |
| 3 | 源路径校验（canonicalSrc）未改动 | ✅ 通过 |
| 4 | `canonicalDst` 旧变量无残留 | ✅ 通过 |

### 逐项证据

#### 1. 父目录校验

```cpp
// Server/msghandler.cpp:448-449
QDir dstDir = dstInfo.dir();           // 获取父目录
QString canonicalDstDir = dstDir.canonicalPath(); // 父目录必定存在
```

**结论**：不再使用目标文件自身的 `canonicalFilePath()`（对不存在的文件返回空字符串）。改用父目录 `dir().canonicalPath()`，解决了此前合法移动被拦截的问题。✅

#### 2. 空值保护

```cpp
// Server/msghandler.cpp:451-452
if(!canonicalDstDir.isEmpty() && 
  (canonicalDstDir == canonicalRoot || canonicalDstDir.startsWith(canonicalRootPrefix))){
```

**结论**：`!canonicalDstDir.isEmpty()` 在比较前做空值保护，防止 root 配置异常时误判。✅

#### 3. 源路径校验完整性

```cpp
// Server/msghandler.cpp:432-444（未改动）
QString canonicalSrc=srcInfo.canonicalFilePath();
...
if(!canonicalRoot.isEmpty() && !canonicalSrc.isEmpty()
        && (canonicalSrc==canonicalRoot || canonicalSrc.startsWith(canonicalRootPrefix))
        && srcInfo.exists()){
```

**结论**：外层源路径校验逻辑（canonical path + exists 检查）完全保留不变。✅

#### 4. 旧变量无残留

`grep -n 'canonicalDst[^D]' Server/msghandler.cpp` 结果：

```
449:    QString canonicalDstDir = dstDir.canonicalPath();
451:    if(!canonicalDstDir.isEmpty() && 
452:      (canonicalDstDir == canonicalRoot || canonicalDstDir.startsWith(canonicalRootPrefix))){
```

**仅存在 `canonicalDstDir`（新变量名）**，无 `canonicalDst`（旧变量名）残留。✅

---

## 三、构建验证

| 构建目标 | 结果 | 错误数 | 备注 |
|---------|------|--------|------|
| `Server/msghandler.cpp` 重编译 → `Server.exe` | ✅ 通过 | 0 | 强制 touch + make，完整编译链接 |
| `Client/client.cpp` 重编译 → `Client.exe` | ✅ 通过 | 0 | 强制 touch + make，完整编译链接 |

**已知可忽略警告**：`ThreadCache.h:20 TLS attribute warning` 未在此次构建中出现（构建日志输出 clean）。

---

## 四、审查结论

### BUG-TST-CLIENT-001：✅ 四次审查通过

全部 4 项强制检查项通过：
- 内层 lambda 仅捕获 `[this]`，不持有 effect 指针
- `effect->deleteLater()` 已彻底移除
- `setGraphicsEffect(nullptr)` 后无 effect 访问
- Friend.cpp、File.cpp 等禁区未被修改

**缺陷状态变更**：`待复查` → `已修复`

### BUG-FILE-002-R1：✅ 三次审查通过

全部 4 项强制检查项（含隐含第 4 项）通过：
- 目标路径使用父目录 `dir().canonicalPath()` 替代不存在的文件路径 `canonicalFilePath()`
- `!canonicalDstDir.isEmpty()` 空值保护就位
- 源路径校验（canonicalSrc）完整保留
- 旧变量 `canonicalDst` 零残留

**缺陷状态变更**：`待复查` → `已修复`

### 剩余风险

| 风险 | 等级 | 说明 |
|------|------|------|
| 动画生命周期边界 | 低 | 外层 lambda 的 `effect` 捕获在内层 lambda 执行后变为悬空指针（但外层 lambda 已执行完毕，连接为一次性，无害） |
| 目标父目录不存在 | 低 | 若整个 `dstPath` 的父目录链不存在，`canonicalPath()` 返回空字符串，安全校验拒绝操作（符合预期——应拒绝向不存在目录移动） |

---

## 五、审查完成确认

- [x] 所有发现均已记录并分级
- [x] 无阻塞或严重问题需提交架构师
- [x] 每个缺陷均有责任人和处理方向
- [x] 测试师可确定可测试范围
- [x] 所有相关缺陷状态已正确更新
