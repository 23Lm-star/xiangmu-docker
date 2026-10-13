# R10：BUG-FILE-003及BUG-SEC-002复核裁决

> 日期：2026-07-27
> 决策人：项目架构师
> 状态：最终裁定

## 1. 决策对象
- **BUG-SEC-002（验证码过期策略）**：允许在有效期内输入错误后继续重试。
- **BUG-FILE-003（移动重名冲突）**：自动追加 (n) 后缀的重命名逻辑实测失败。

## 2. 状态判定
- **BUG-SEC-002**：测试通过，标记为 **✅ 已关闭**。
- **BUG-FILE-003**：自动重命名功能未达到预期，返回 **修复中**，打回项目工程师。

## 3. 技术决策：BUG-FILE-003 进一步修复方案
针对移动重命名实测失败的情况，采用 **方案 A 与 方案 C 结合** 的路线：

1. **统一的路径拼接机制**：
   抛弃手动使用 `QString("%1/%2")` 的拼接方式，必须使用 `QDir(dirPath).filePath(newName)` 让 Qt 自行处理系统平台相关的路径分隔符。
2. **实例化的错误输出（获取真实报错）**：
   静态方法 `QFile::rename(src, dst)` 和 `QFile::copy(src, dst)` 返回 `bool`，无法获知失败根因（如权限不足、占用、跨盘不支持等）。要求工程师必须通过实例化 `QFile srcFile(srcPath);` 调用 `srcFile.rename(dstPath)`，在失败时调用 `srcFile.errorString()` 打印详细日志。
3. **退化降级策略（防线）**：
   如果 `rename` 失败，继续尝试 `copy` + `remove`。若二次尝试依旧失败，必须打印 `copy` 失败的具体错误码/原因，并确保返回 `false`。由于 PDU 层限制只能返回 bool，这里不进行协议扩充弹框，将重名容错交由上述完善的底层 IO 处理实现。

## 4. 详细根因分析（补充：2026-07-27 架构师确认）

审查 `Server/msghandler.cpp:470-494` 发现以下技术缺陷：

### 根因嫌疑 #1：`QFileInfo::absolutePath()` 路径格式（第 474 行）
```cpp
QString dirPath = checkInfo.absolutePath();
```
在 Windows 上可能返回不含尾部路径分隔符的路径，或使用 `/` 而非 `\`。

### 根因嫌疑 #2：手动路径拼接使用 POSIX 分隔符（第 481 行）
```cpp
finalDstPath = QString("%1/%2(%3)%4").arg(dirPath).arg(baseName).arg(count++).arg(suffix);
```
`QFile::rename()` 底层调用 Windows `MoveFileW`，可能对路径分隔符敏感。

### 附加嫌疑：QFileInfo 缓存（第 472+480 行）
```cpp
QFileInfo checkInfo(finalDstPath);    // 循环外创建
while (QFileInfo::exists(finalDstPath)) { ... }  // 循环内静态调用
```
`checkInfo` 与 `QFileInfo::exists()` 可能返回不一致结果。

## 5. 下游执行指令

### 5.1 本裁决最终决定

采用 **方案 C（日志诊断先行）→ 方案 A（精准修复）** 路线。BUG-FILE-003 状态更新为 **"已确认，待修复"**（原"修复中"状态随上一轮修复未通过而失效）。

### 5.2 向项目工程师下发任务

已在缺陷记录 `defects/BUG-FILE-003.md` 中详细记录，工程师必须在下一次修复中同步添加诊断日志（输出 QFile::error()、路径对比、文件系统类型等），完成后移交测试师采集反馈。
