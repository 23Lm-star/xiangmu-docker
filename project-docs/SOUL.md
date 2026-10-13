# 项目工程师 SOUL.md（本地配套文档）

> 本文是 `project_engineer` SOUL 的本地配套文档，与 Hermes Profile 中的正式 SOUL 保持同步。
>
> **正式版本位于**：`C:\Users\王晨扬\AppData\Local\hermes\profiles\project_engineer\SOUL.md`

---

## 说明

本文档原为独立的 Claude Code 编码操作指南，现已合并到项目工程师的正式 SOUL 文档中。

变更记录：

| 日期 | 变更 | 说明 |
|------|------|------|
| 2026-07-24 | 内容合并至 `project_engineer` SOUL | Claude Code 操作指南移至 Hermes Profile，此文件保留为本地索引 |

---

## 快速参考

以下是从正式 SOUL 中摘录的常用命令，完整内容请参见正式版本。

### 环境准备

```bash
CLAUDE="/c/Users/王晨扬/AppData/Roaming/npm/node_modules/@anthropic-ai/claude-code/bin/claude.exe"
```

### 基础编码任务

```bash
cd /e/qt/xiangmu && "$CLAUDE" -p "任务描述" --allowedTools "Read,Edit" --max-turns 5
```

### 需要运行测试的任务

```bash
cd /e/qt/xiangmu && "$CLAUDE" -p "任务描述" --allowedTools "Read,Edit,Bash" --max-turns 10
```

### 仅审查代码

```bash
cd /e/qt/xiangmu && "$CLAUDE" -p "审查任务描述" --allowedTools "Read" --max-turns 3
```
