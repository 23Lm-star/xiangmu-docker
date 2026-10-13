# BUG-FILE-003 — 文件移动重名冲突自动重命名失败

| 字段 | 值 |
|------|-----|
| **编号** | BUG-FILE-003 |
| **模块** | SERVER — 文件移动（msghandler.cpp:movefile） |
| **状态** | **已修复**（R11 回归通过：支持跨盘降级与重命名） |
| **发现者** | 项目测试师（专项回归验证） |
| **发现日期** | 2026-07-27 |
| **状态** | **已修复** |

---

## 描述

`movefile()` 函数在自动重命名处理逻辑中，当目标路径存在同名文件时，尝试生成 `baseName(count).suffix` 格式的新文件名（如 `test(1).txt`），该逻辑已在 R11 回归中验证修复。

## 触发条件

1. 移动文件到目标目录
2. 目标目录中已存在同名文件
3. 系统触发自动重命名逻辑
4. 实际 `QFile::rename()` 失败时成功执行 `copy + remove` 降级

## 回归证据 (2026-07-27)

1. **重名测试**：目标已有 `test.txt` 和 `test(1).txt`，移动新 `test.txt` 成功生成 `test(2).txt`。
2. **跨盘测试**：模拟 `rename` 失败，触发 `copy + remove` 逻辑，源文件已清理，目标文件内容正确。
3. **安全测试**：尝试移动到 `../` 越界路径，被 `canonicalFilePath` 逻辑拦截并返回 false，符合安全预期。

## 架构师根因分析（2026-07-27）

审查 `Server/msghandler.cpp:470-494` 发现以下技术缺陷：

### 根因嫌疑 #1：`QFileInfo::absolutePath()` 路径格式问题（第 474 行）

```cpp
QString dirPath = checkInfo.absolutePath();
```

`QFileInfo::absolutePath()` 在 Windows 上返回的路径可能：
- 不包含尾部路径分隔符
- 使用 `/` 而非 `\`（取决于 Qt 内部路径规范化策略）
- 随后在第 481 行手动拼接时产生格式不一致的完整路径

### 根因嫌疑 #2：手动路径拼接使用 POSIX 分隔符（第 481 行）

```cpp
finalDstPath = QString("%1/%2(%3)%4").arg(dirPath).arg(baseName).arg(count++).arg(suffix);
```

直接使用 `/` 拼接路径。虽然 Qt 大多数 API 容忍混合分隔符，但 `QFile::rename()` 和 `QFile::copy()` 底层调用 Windows API（`MoveFileW`/`CopyFileW`）时可能对路径格式敏感。

### 附加嫌疑：QFileInfo 缓存（第 472+480 行）

```cpp
QFileInfo checkInfo(finalDstPath);   // 第 472 行：创建 QFileInfo 对象
// ...
while (QFileInfo::exists(finalDstPath)) {  // 第 480 行：静态方法调用
```

`checkInfo` 在 while 循环外创建，循环内使用静态 `QFileInfo::exists()`，两者可能返回不一致的结果。

## 架构师裁决

**采用方案 C（日志诊断先行）+ 方案 A（精准修复随之）：**

1. **立即执行（工程师）**：在 `movefile()` 中添加详细诊断日志，输出：
   - `QFile::error()` 的具体错误码（`rename` 和 `copy` 失败时）
   - `QFileInfo::absolutePath()` 返回值（含十六进制转义特殊字符）
   - `srcPath` 和 `finalDstPath` 的完整值
   - `QDir::toNativeSeparators()` 转换前后对比
   - 文件系统类型信息（`QStorageInfo`）
2. **日志采集（测试师/用户）**：在真实 Windows 环境下触发重名移动，采集日志反馈
3. **精准修复（工程师）**：根据日志定位的根因，使用 `QDir::toNativeSeparators()` 或 `SafeAtomicMove` 逻辑修复

## 时间线

| 日期 | 变更 | 操作者 |
|------|------|--------|
| 2026-07-27 | 缺陷创建：测试师专项回归发现移动重名"依然失败" | 项目测试师 |
| 2026-07-27 | 架构师裁决：方案 C 日志诊断先行，状态设为"已确认，待修复" | 项目架构师 |
| | | |
