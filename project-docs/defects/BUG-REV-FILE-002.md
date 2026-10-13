# BUG-REV-FILE-002：copy+remove 降级后 remove 返回值未检查

| 属性 | 值 |
|------|-----|
| 缺陷编号 | BUG-REV-FILE-002 |
| 严重等级 | 一般 |
| 涉及模块 | Server/msghandler.cpp |
| 代码位置 | Line 502 |
| 当前状态 | 已修复 |
| 发现日期 | 2026-07-27 |
| 关联缺陷 | BUG-FILE-003 |

## 触发条件

1. `movefile()` 中 `rename()` 失败
2. 降级 `QFile::copy()` 成功
3. 但随后 `QFile::remove(srcPath)` 失败（如源文件被占用、权限不足）

## 回归证据 (2026-07-27)

1. **逻辑验证**：`Server/msghandler.cpp:507-513` 已增加 `QFile::remove(srcPath)` 返回值检查。
2. **失败处理测试**：模拟 remove 失败场景，代码成功进入清理目标逻辑 `QFile::remove(finalDstPath)` 并返回 `false`，确保了移动语义的原子性，防止了伪成功。

## 问题说明

```cpp
// Lines 497-504
ret = QFile::copy(srcPath, finalDstPath);
if (!ret) {
    // ...
} else {
    QFile::remove(srcPath);     // 返回值被丢弃！
    qDebug() << "copy+remove 降级成功:" << finalDstPath;
}
```

`QFile::remove()` 返回 `bool`，但代码未检查返回值。若 remove 失败：
- 源文件仍然存在（未被"移动"，实为"复制"）
- 但 `ret` 已设为 `true`（来自 `copy` 成功）
- 函数返回 `true` 声称移动成功
- 客户端认为文件已移走，但服务端源文件残留 — **数据一致性问题**

## 预期行为

remove 失败时应报告移动失败（`ret = false`），并记录错误日志。

## 实际行为

remove 失败时函数仍返回 true，客户端被告知移动成功但源文件末删除。

## 影响范围

- 数据一致性：文件"移动"实际变为"复制"
- 用户体验：客户端认为文件已移动，但源位置文件仍存在
- 存储空间：残留文件占用磁盘

## 判断依据

- 文件操作的原子性语义：移动 = copy + remove，两者都成功才算移动成功
- 架构师裁决未明确此项，但属于基本的操作语义正确性

## 修正建议

```cpp
} else {
    if (QFile::remove(srcPath)) {
        qDebug() << "copy+remove 降级成功:" << finalDstPath;
    } else {
        qDebug() << "copy+remove 降级后 remove 失败:" << srcPath;
        // 可选：尝试清理已复制的目标文件
        QFile::remove(finalDstPath);
        ret = false;  // 报告移动失败
    }
}
```

## 建议验证方式

1. 模拟跨盘移动（rename 失败，copy 成功）
2. 在 copy 成功后、remove 前，用另一个进程锁定源文件
3. 验证函数返回 false，且日志包含 remove 失败信息

## 修复历史

| 日期 | 操作 | 执行人 |
|------|------|--------|
| 2026-07-27 | 发现 | Bug 查找师 |
