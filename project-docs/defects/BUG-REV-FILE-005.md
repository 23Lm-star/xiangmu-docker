# BUG-REV-FILE-005：目标父目录不存在时静默失败

| 属性 | 值 |
|------|-----|
| 缺陷编号 | BUG-REV-FILE-005 |
| 严重等级 | 轻微 |
| 涉及模块 | Server/msghandler.cpp |
| 代码位置 | Lines 465-469 |
| 当前状态 | 已修复 |
| 发现日期 | 2026-07-27 |
| 关联缺陷 | BUG-FILE-003 |

## 触发条件

1. 用户根目录 (`filesPath/username`) 因某种原因不存在且未自动创建
2. 或目标文件名包含路径分隔符导致 `dstInfo.dir()` 指向不存在的目录

## 回归证据 (2026-07-27)

1. **诊断核验**：`Server/msghandler.cpp:517` 已添加 `movefile 目标路径校验失败` 诊断日志，包含实际的 `canonicalDstDir` 状态。
2. **逻辑核验**：代码不再静默返回 false，符合 R11 裁决要求的“诊断先行”。

## 问题说明

```cpp
QDir dstDir = dstInfo.dir(); // 获取父目录
QString canonicalDstDir = dstDir.canonicalPath(); // 父目录必定存在
if (!canonicalDstDir.isEmpty() && 
    (canonicalDstDir == canonicalRoot || canonicalDstDir.startsWith(canonicalRootPrefix))) {
    // ... 执行移动
}
```

当 `dstDir` 不存在时，`canonicalPath()` 返回空字符串。此时 `!canonicalDstDir.isEmpty()` 为 false，跳过整个移动逻辑，`ret` 保持 false。函数最终返回失败 PDU。

**但没有打印任何诊断日志**说明为什么会跳过移动逻辑。

## 预期行为

(a) 路径校验失败时输出日志说明原因，或  
(b) 确保父目录存在（如通过 `QDir::mkpath()` 自动创建）

## 实际行为

静默返回 false，无日志。

## 影响范围

- 调试困难：问题难以定位
- 正常场景下不应触发（用户目录由注册流程创建），但异常恢复时可能遇到

## 修正建议

```cpp
if (canonicalDstDir.isEmpty()) {
    qDebug() << "movefile 目标父目录不存在或不可解析:" << dstInfo.dir().path();
} else if (!(canonicalDstDir == canonicalRoot || canonicalDstDir.startsWith(canonicalRootPrefix))) {
    qDebug() << "movefile 目标路径越界:" << canonicalDstDir;
} else {
    // ... 执行移动
}
```

## 建议验证方式

代码审查即可验证。

## 修复历史

| 日期 | 操作 | 执行人 |
|------|------|--------|
| 2026-07-27 | 发现 | Bug 查找师 |
