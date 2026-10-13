# BUG-REV-FILE-004：最终调试日志使用原始 dstPath 而非 finalDstPath

| 属性 | 值 |
|------|-----|
| 缺陷编号 | BUG-REV-FILE-004 |
| 严重等级 | 轻微 |
| 涉及模块 | Server/msghandler.cpp |
| 代码位置 | Line 509 |
| 当前状态 | 已修复 |
| 发现日期 | 2026-07-27 |
| 关联缺陷 | BUG-FILE-003 |

## 触发条件

文件移动时发生重命名（目标路径已有同名文件，自动追加 (1) 后缀）。

## 回归证据 (2026-07-27)

1. **静态验证**：`Server/msghandler.cpp:521` 的最终日志已更正为打印 `finalDstPath`。
2. **逻辑一致性**：确认日志输出与实际业务执行的最终路径一致，消除了诊断歧义。

## 问题说明

```cpp
qDebug()<<"movefile src:"<<srcPath<<"dst:"<<dstPath<<"ret:"<<ret;
```

日志输出原始 `dstPath`（如 `/files/user/file.txt`），而非实际使用的 `finalDstPath`（如 `/files/user/file(1).txt`）。当发生重命名时，日志显示的目标路径与实际写入路径不一致，调试时造成困惑。

## 预期行为

日志输出实际的目标路径 `finalDstPath`。

## 实际行为

日志始终输出原始 `dstPath`。

## 影响范围

- 调试效率降低

## 修正建议

```cpp
qDebug()<<"movefile src:"<<srcPath<<"dst:"<<finalDstPath<<"ret:"<<ret;
```

## 建议验证方式

代码审查即可验证。

## 修复历史

| 日期 | 操作 | 执行人 |
|------|------|--------|
| 2026-07-27 | 发现 | Bug 查找师 |
