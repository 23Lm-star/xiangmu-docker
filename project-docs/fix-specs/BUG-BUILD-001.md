# BUG-BUILD-001 修复方案 — Server.pro 中 config.qrc 重复引用

> **优先级**：P2（轻微）
> **裁决人**：等待项目架构师审阅
> **执行人**：项目工程师

---

## 问题

`Server/Server.pro:59-62` 中 `config.qrc` 在 `RESOURCES` 中被列出 3 次。

```pro
RESOURCES += \
    config.qrc \
    config.qrc \
    config.qrc
```

Qt qmake 会去重，无编译错误，但属于冗余。

## 影响范围

1 个文件：`Server/Server.pro`

## 修改清单

### `Server/Server.pro`（第 59-62 行）

```diff
-RESOURCES += \
-    config.qrc \
-    config.qrc \
-    config.qrc
+RESOURCES += config.qrc
```

## 验证

```bash
cd Server && qmake
```

**通过标准**：qmake 运行无警告，`Makefile` 中 `config.qrc` 仅出现一次（`grep -c config.qrc Makefile` 应为 1）。

## 手工测试

编译 Server 端，确认功能不受影响：

```bash
cd Server && make clean && make
```

**通过标准**：编译成功，运行时数据库连接正常。
