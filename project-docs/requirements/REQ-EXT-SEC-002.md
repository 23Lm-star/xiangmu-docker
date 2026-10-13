# REQ-EXT-SEC-002 重命名/移动：声明字段移交服务端确认

## 1. 分支信息

- 分支：`fix/req-ext-sec-002-server-confirm-rename-move`
- 层级：**MEDIUM**（改生产源码、有验收标准；**不触碰 frozen_paths**，协议头与线格式零改动）
- 依赖：REQ-EXT-FILE-001（元对象/逻辑条目分离，D5 条目权威已建立）；REQ-EXT-SEC-001（actor 身份收敛模式，本需求沿用其"声明字段只做定位、权威在服务端"范式）

## 2. 目标

FILE-001 之后条目库是权威，但 rename(27)/move(38) 两条消息里仍剩两处**采信客户端声明做分支/落键**：
rename 的 `fileType`、move 的 `fileName`。本需求把它们移交服务端确认。

用户可见目标：**伪造的类型与文件名不再能改变服务端行为**——改名/移动的结果只由服务端事实（条目 `is_dir`、文件系统、srcRel 末段）决定。

## 3. 根因（改前事实）

- `Server/msghandler.cpp:749`：条目分支的物理同步改名用客户端 fileType 选 `QDir/QFile`，而无视已查到的 `e.isDir`。
- `:757-770`：未迁移回退分支完全按 fileType==0/1 分支；**未知 fileType → ret=1，谎报成功且不执行**。
- `:955`：movefile 的 fileName 取自 caData（客户端短名），服务端不校验分隔符、不校验与 srcRel 末段一致；
  条目分支可把 display_name 落成含 `\` 的脏键（relocateEntry 只做非空+clampUtf8），
  且 srcRel 指 X、caData 指 Y 时**逻辑移 Y 条目、物理改 X 实体**；
  回退分支 `dstPath=dstDir+fileName` 可用 `"../x"` 逃出已校验的 dstDir。
- `:959`：caMsg 无 `'\0'` 时 `indexOf` 返回 -1，`left(-1)` 返回整串，靠 `dstInfo.isDir()==false` 碰巧拒掉（非显式）。
- renamefile 新名==旧名：仅客户端拦截（`Client/file.cpp:374`）；服务端 relocateEntry 重名检查命中**自身条目**→ 误报"目标名已占用"。
- renamefile 条目分支成功路径不检查**物理**占用（movefile `:1008-1009` 有混合检查）：条目已改、物理改名撞车失败仅留告警。

## 4. 设计（全部落在 Server/msghandler.cpp 两函数内）

1. **renamefile 类型服务端派生**：条目分支物理同步用 `e.isDir`；回退分支用 `QFileInfo(oldFilePath).isDir()/exists()` 推导目录/文件/不存在（不存在→ret=0）。客户端 fileType 只读不采信，与服务端推导不一致时打告警（sharefile 同式）。
2. **renamefile 同名幂等**：old==new 且源存在（条目有效或物理存在）→ ret=1 空操作；源不存在→ ret=0。
3. **renamefile 物理占用前置**：条目分支 relocate 前，若 `QFileInfo(newFilePath).exists()` → ret=0 拒绝（与 movefile 混合检查同口径）。
4. **movefile 名字服务端派生**：`fileName = QFileInfo(srcPath).fileName()`（srcRel 末段）；空名或含分隔符→拒绝。caData 声称名只对照，不一致告警并按派生名执行（sharefile "按连接身份改写" 同式）。回退分支 dstPath 因此不再受未校验字段影响。
5. **movefile 显式拒畸形 caMsg**：`sep < 0`（无 `'\0'`）→ ret=0。

## 5. 验收标准（业务语言）

| # | 断言 | 判据 |
|---|---|---|
| A1 | 未建条目旧文件改名，谎报 fileType=7 → 实际改名成功（改前：回"成功"但不动） | 响应 ret=1 且物理文件已改名 |
| A2 | 条目改名 old==new → 成功且条目原样（改前：误报失败） | 响应 ret=1，DB 条目未变 |
| A3 | 条目改名为一个**物理幽灵名**（无条目的同名旧文件）→ 前置拒绝（改前：条目改了、物理没改） | 响应 ret=0，DB 条目未变 |
| A4 | 谎报 fileType=0 的文件条目改名 / fileType=1 的目录条目改名 → 仍正确改名 | 响应 ret=1，条目+物理一致 |
| A5 | move：caData 短名与 srcRel 末段错位 → 按服务端派生名移动（改前：逻辑/物理解体） | 响应名=末段，DB+磁盘一致 |
| A6 | move：caData 文件名含 `../` 且未建条目 → 不逃逸（改前：dstPath 逃出 dstDir） | 上级目录无新文件 |
| A7 | move：caMsg 无 `'\0'` → 显式拒绝 | 响应 ret=0 |
| A8 | 回归：entry-ops/dedup/cross-tree/sec001×2 全绿；全量手动扫 11 套=改前基线（3 红既有已归因） | 套件输出 |

## 6. 允许路径

- `Server/msghandler.cpp`（renamefile/movefile 两函数）
- `project-docs/requirements/REQ-EXT-SEC-002.md`、`project-docs/evidence/REQ-EXT-SEC-002/`、`project-docs/handoffs/`

## 7. 不做什么

- 不改协议头/线格式（fileType 字段保留在线格式中，仅不再采信）；不改客户端；
- 不动 relocateEntry/normalizeParent 本体；
- deldir/mkdir/changedir 不在本需求面内（其声明字段问题另立需求）。
