# BUG-REV-FILE-001：copy 降级失败时错误日志输出错误的 errorString

| 属性 | 值 |
|------|-----|
| 缺陷编号 | BUG-REV-FILE-001 |
| 严重等级 | 一般 |
| 涉及模块 | Server/msghandler.cpp |
| 代码位置 | Line 499 |
| 当前状态 | 已修复 |
| 发现日期 | 2026-07-27 |
| 关联缺陷 | BUG-FILE-003 |

## 触发条件

1. `movefile()` 中 `QFile::rename()` 失败（如跨文件系统）
2. 降级 `QFile::copy()` 也失败

## 回归证据 (2026-07-27)

1. **静态代码核验**：`Server/msghandler.cpp:505` 已改为输出 `errno` 和 `strerror(errno)`。
2. **逻辑一致性**：已确认 copy 失败时不再错误引用 `srcFile.errorString()`（该实例属于 rename 的上下文），而是直接使用系统 errno，诊断信息准确。

## 问题说明

```cpp
// Line 491-500
QFile srcFile(srcPath);
ret = srcFile.rename(finalDstPath);           // rename 失败
if (!ret) {
    qDebug() << "rename 失败, errorString:" << srcFile.errorString()
             << "src:" << srcPath << "dst:" << finalDstPath;
    ret = QFile::copy(srcPath, finalDstPath); // copy 也失败
    if (!ret) {
        qDebug() << "copy 降级也失败, errorString:" << srcFile.errorString()
                 // ^^^^^ 这里打印的还是 rename 的错误！不是 copy 的错误！
                 << "src:" << srcPath << "dst:" << finalDstPath;
    }
}
```

`srcFile` 绑定到 `srcPath`，`errorString()` 反映的是最后一次**对该实例**的操作错误，即 `rename` 失败的错误。`QFile::copy()` 是静态方法，不与 `srcFile` 实例关联，其错误不会反映在 `srcFile.errorString()` 中。

## 预期行为

copy 失败时输出 copy 操作的实际错误原因。

## 实际行为

copy 失败时输出的仍是 rename 失败的错误信息，误导诊断。

## 影响范围

- 诊断效率：出问题时无法定位 copy 失败的真实根因（权限、磁盘空间、文件占用等）

## 判断依据

- 架构师裁决 R10 要求"copy 失败时打印 copy 失败的具体错误码/原因"
- Qt 文档：`QFile::errorString()` 返回对该文件对象最后一次操作的错误

## 修正建议

```cpp
// 方案：使用目标文件对象获取 copy 后的错误
if (!ret) {
    QFile dstFile(finalDstPath);
    if (dstFile.exists()) dstFile.remove();  // 清理可能的不完整副本
    qDebug() << "copy 降级也失败"
             << "src:" << srcPath << "dst:" << finalDstPath;
    // 对于静态 copy，可通过检查 errno 获取系统错误
    qDebug() << "系统错误码:" << errno << strerror(errno);
}
```

## 建议验证方式

1. 模拟跨盘移动：rename 必然失败
2. 使 copy 也失败（如目标盘满）
3. 检查日志输出包含正确的 copy 错误信息，而非 rename 错误

## 修复历史

| 日期 | 操作 | 执行人 |
|------|------|--------|
| 2026-07-27 | 发现 | Bug 查找师 |
