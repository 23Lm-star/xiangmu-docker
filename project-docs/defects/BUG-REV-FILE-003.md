# BUG-REV-FILE-003：重名循环 count 无上限保护

| 属性 | 值 |
|------|-----|
| 缺陷编号 | BUG-REV-FILE-003 |
| 严重等级 | 轻微 |
| 涉及模块 | Server/msghandler.cpp |
| 代码位置 | Line 482 |
| 当前状态 | 已修复 |
| 发现日期 | 2026-07-27 |
| 关联缺陷 | BUG-FILE-003 |

## 触发条件

极端情况下，文件被重命名到 `file(1).txt` 到 `file(INT_MAX).txt` 全部已存在。

## 回归证据 (2026-07-27)

1. **静态验证**：代码已添加 `const int MAX_RETRIES = 1000;` 且在 `do-while` 循环中检查 `count > MAX_RETRIES`。
2. **逻辑核验**：确认在达到上限后会执行 `ret = false; break;`，消除了无限死循环风险。

## 问题说明

```cpp
int count = 1;
do {
    QString numberedName = QString("%1(%2)%3").arg(baseName).arg(count++).arg(suffix);
    finalDstPath = parentDir.filePath(numberedName);
} while (QFileInfo::exists(finalDstPath));
```

`count` 无上限。若所有编号都被占用（理论上由外部恶意行为触发），循环永不终止，导致线程阻塞。

## 预期行为

尝试有限次数后（如 1000），停止循环并返回失败。

## 实际行为

无限循环直至 INT_MAX 溢出为负数后发生未定义行为。

## 影响范围

- 服务端线程阻塞（极端情况）
- 实际风险极低，但属防御性编码缺失

## 修正建议

```cpp
int count = 1;
const int MAX_RETRIES = 1000;
do {
    QString numberedName = QString("%1(%2)%3").arg(baseName).arg(count++).arg(suffix);
    finalDstPath = parentDir.filePath(numberedName);
    if (count > MAX_RETRIES) {
        qDebug() << "重命名尝试超过上限" << MAX_RETRIES;
        ret = false;
        break;
    }
} while (QFileInfo::exists(finalDstPath));
```

## 建议验证方式

代码审查即可验证。

## 修复历史

| 日期 | 操作 | 执行人 |
|------|------|--------|
| 2026-07-27 | 发现 | Bug 查找师 |
