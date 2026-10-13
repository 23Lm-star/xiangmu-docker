# BUG-BUILD-002 修复方案 — .gitignore 覆盖不充分

> **优先级**：P2（轻微）
> **裁决人**：等待项目架构师审阅
> **执行人**：项目工程师

---

## 问题

`.gitignore` 仅忽略 `build-*/` 和 `Client/_github_bundle_repo/`，大量构建产物可能被误提交。

当前示例未跟踪文件可能包含：
- `Makefile`、`Makefile.Debug`、`Makefile.Release`
- `*.o`、`*.obj`
- `*.pro.user`
- `*.qmake.stash`
- `moc_*.cpp`、`ui_*.h`、`qrc_*.cpp`
- `release/`、`debug/` 目录

## 影响范围

1 个文件：`.gitignore`

## 修改清单

### `.gitignore` — 完整覆盖所有构建产物

```diff
 build-*/
 Client/_github_bundle_repo/
+
+# 构建目录
+release/
+debug/
+
+# Makefile 产物
+Makefile
+Makefile.Debug
+Makefile.Release
+object_script.*.Debug
+object_script.*.Release
+*.o
+*.obj
+*.exe
+
+# Qt 自动生成
+*.pro.user
+*.qmake.stash
+moc_*.cpp
+ui_*.h
+qrc_*.cpp
+
+# 系统文件
+.DS_Store
+Thumbs.db
+*~
```

## 验证

```bash
# 查看当前未跟踪文件
git status --porcelain

# 确认 .gitignore 已加载
git check-ignore -v Makefile     # 应显示匹配 .gitignore 规则
git check-ignore -v debug/       # 应显示匹配
```

**通过标准**：`git status` 中不再显示 `Makefile`、`*.o`、`*.pro.user` 等构建产物。

## 手工检查

```bash
# 确认关键文件未被忽略（源码文件不受影响）
git check-ignore -v Server/msghandler.cpp   # 不应显示任何匹配
git check-ignore -v Client/client.cpp       # 不应显示任何匹配
```

---

## 附录：当前 .gitignore 效果验证

```bash
# 在修改前查看哪些文件被遗漏
git status | grep '^\?' | grep -E '\.(o|exe|user)$|Makefile|moc_|ui_|qrc_|release|debug'
```
