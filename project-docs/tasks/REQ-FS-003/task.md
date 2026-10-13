# 任务记录：REQ-FS-003 path-traversal-and-ownership

- **tier**: MEDIUM
- **tier 依据**: 触碰生产源码（`Server/msghandler.h/.cpp`）但不触碰 `frozen_paths`（两个 `protocol.h`）、不改对外协议、单分支单主题可回滚；基线 `b5d7bb3`，分支 `fix/path-traversal-and-ownership`
- **owner_agent**: claude-code
- **date**: 2026-09-29
- **状态**: **已完成并合并 master** @`f937c90`（no-ff，合并结果 build GREEN / test 0 注册项 / audit PASS）；审计 FAIL→修复→复审 PASS 全流程闭环；O-5 及 O-1–O-4 残余观察记录在案

## 需求（业务语言）

服务端多个接收文件路径的入口没有校验"路径是否落在文件沙箱内"：上传可以在任意目录建文件/追加内容，删除和重命名可以先对沙箱外文件动手再补救，另有四处入口（建目录、列目录、切目录、共享复制）完全零校验——其中"共享复制"能把服务器可读的任何文件（含数据库配置）复制进自己的目录再下载。本需求把所有路径入口统一收进一个校验函数。

## 验收标准

见 `project-docs/requirements/REQ-FS-003-path-traversal-and-ownership.md` §二（A1–A12，其中 A10–A12 为 2026-09-29 修订新增）。

## allowed paths

- `Server/msghandler.h`（新增 `isPathInSandbox` 声明）
- `Server/msghandler.cpp`（实现 + 新增拦截 + 去重替换）
- `project-docs/tasks/REQ-FS-003/task.md`
- `project-docs/evidence/REQ-FS-003/`
- `project-docs/requirements/REQ-FS-003-path-traversal-and-ownership.md`（需求修订同步）

## 不做的事（划界）

- 不改协议（`protocol.h` 是 frozen_paths）；不改客户端；不改 `ResumableUploadSession` 接口
- 不引入新的用户级隔离（A 不能删 B）——现有 movefile/listdir 的用户目录根**维持原状**
- 不顺手修其他缺陷（日志分级 REQ-LOG-007、密码加盐 REQ-AUTH-002 各自独立分支）

## 决策日志（append-only）

| 日期 | 决策 | 说明 |
|---|---|---|
| 2026-09-29 | 需求分析完成，档位定为 MEDIUM | 逐行复核基线：缺陷 1（uploadfileinit:743 dirPath 零校验）、缺陷 2（deldir:397-404 / renamefile:495-502 先动手后反推）属实 |
| 2026-09-29 | **盘点更正：路径入口共 10 处，不是 6 处** | 代码核实：`MKDIR`(372)、`changedir`(425)、`flushfile`(461)、`sharefilemove`(566) 同为客户端可控路径且零校验；原文第④步把 MKDIR 误记为"已有校验" |
| 2026-09-29 | **主人拍板①：isPathInSandbox 增加可选根参数**（默认 filesPath） | 原因：movefile/listdir 现有校验的根是用户目录，按原文"照抄替换"会放松成全树隔离，违反文档自身"不收紧不放松"约束；替换时传用户根保持原隔离级别 |
| 2026-09-29 | **主人拍板②：漏盘入口全部纳入本需求**（MKDIR/changedir/flushfile/sharefilemove 做新增拦截，即改动块⑤） | 仍只动 msghandler 两个文件、不碰协议，MEDIUM 可承受；sharefilemove 是读取逃逸链（可复制 connect.config），风险最高 |
| 2026-09-29 | 需求文档已按上述修订更新（§一表扩至 10 入口、§3.1 签名加根参数、§3.2 增至五块、§二增 A10–A12、§七增 3 场景、§九 filesPath 边界坐实） | 实现文档与实际冲突时同步修正 |
| 2026-09-29 | 实测坐实：`connect.config` 第 3 行 filesPath 为相对路径 `./filesys` | `isPathInSandbox` 内对根做 `QDir(root).absolutePath()` 归一为必选项 |
| 2026-09-29 | renamefile 陷阱记录：校验所在目录，不校验目标文件路径 | 新文件名 rename 前不存在，canonical 返回空，照目标路径校验会误拒所有正常重命名（A7 必挂）；现状旧/新名共用同一目录（489-490 行），校验 dirPath 一处即可 |
| 2026-09-29 | 最小必要安全改动集合调整为 ①②③⑤ | ④（去重替换）风险大时可留给后续需求；⑤ 为纯新增拦截，无回归面 |
| 2026-09-29 | 分支 `fix/path-traversal-and-ownership` 从 master `b5d7bb3` 切出，需求目录（11 份文档）随分支首次提交 `73d85d8` 入库 | 关闭 state.ps1 报告的 `?? project-docs/requirements/` 挂起状态 |
| 2026-09-29 | **主人指令：dev 完成后不得合并 master**，先由主人另开独立审计会话，审计通过并通知后才合并 | 对 MEDIUM 追加独立审计门（超出本档位默认流程，按主人指令执行）；goal/dev/pipeline 在本会话连续进行（MEDIUM 允许单会话推进） |
| 2026-09-29 | 实现注记：MKDIR 与 sharefilemove 的目标名增加分隔符检查（`'/'`/`'\\'`） | 属同一入口的同类逃逸面（name 段 `../` 可穿越），非"顺手改其他缺陷"；其余严格按修订文档 ①–⑤ |
| 2026-09-29 | dev 落地：`isPathInSandbox(path, root=filesPath)` + ①–⑤ 十入口全部接入；代码提交 `f808daf` | ④ 三步替换全部完成：movefile/listdir 传用户根（隔离级别不变），downloadfile 用默认根；未触发"回退④"出口 |
| 2026-09-29 | 三项门禁在代码提交后重新盖章：build GREEN / test 0 注册项不阻塞 / audit PASS（均 @f808daf，frozen path 无命中） | 证据 `project-docs/evidence/REQ-FS-003/pipeline-run.log` |
| 2026-09-29 | **§七 人工越权验证 = 端到端 TCP 实测**：本机无 MySQL 服务，但经核实分发层无登录门且全部拒绝路径不触 DB，Server.exe 直接起监听 127.0.0.1:5000，Python harness 构造真实 PDU | 优于 R19 报告的 Tier-2/3 回退：25/25 PASS（10 处入口越权全拒 + 合法功能全正常 + 续传 offset 正确 + victim 文件内容完好），`manual-verify.log` + 服务端 10 条拒绝日志 `server-reject-lines.log` |
| 2026-09-29 | 测试材料纪律：victim 等临时文件只建在 `build/reqfs003_out/`；沙箱工作目录测后移入 `build/reqfs003/trash_*`，未用 rm/del 手工删（禁止事项 #11） | changedir "C:/Windows" 仅作探测断言（isExist=false），无写入动作 |
| 2026-09-29 | 工具链注记：Server.exe 为 GUI 子系统，日志需 `QT_FORCE_STDERR_LOGGING=1` 才进管道且输出 GBK 编码；v1.7 脚本需 `py -3.13`（PATH 上的 Anaconda Python 3.6.5 语法不兼容） | 建议后续写入 manifest notes（非本需求范围，未动） |
| 2026-09-29 | **独立审计会话判定 FAIL，禁止合并**：F-1（High）`renamefile` 名称段（`oldFileName/newFileName`，caData 64 字节）无校验直接拼接，`../` 可绕过 dirPath 校验改名沙箱外文件，A6 被绕；其余 A1–A5/A7–A12 通过，范围合规通过 | 报告 `project-docs/evidence/REQ-FS-003/audit-report.md`（含残余观察 O-1–O-4）；需 owner 新会话修复（校验拼接后 `oldFilePath` 或名称段分隔符检查，同 MKDIR 模式）并重审 |
| 2026-09-29 | **修复会话：F-1 按审计方向②落地 @`a518e35`**：renamefile 名称段 old/new 拒绝 `'/'`、`'\'`（同 MKDIR），名称经 `fixedString`，分享链接联动段复用净化名 | 不选①：`newFilePath` 不存在无法 canonical 校验（A7 陷阱），方向②对源/目标两侧同时闭合且与既有同类修复一致 |
| 2026-09-29 | **根因定性：需求 §3.2③"校验 dirPath 一处即可"为规格盲区**（默认名称为裸文件名），已同步修订需求文档（§二 A6 扩名称段、§3.2③ 二次修订、§七 增至 10 场景） | 实现忠实于旧规格；冲突时同步修正文档 |
| 2026-09-29 | **重盖章 @`a518e35`**：端到端 harness 扩 S6b/S6c/S6d（旧名 `../`、新名 `../`、新名 `\`）= **28/28 PASS**；build GREEN / test 0 注册项 / audit PASS；服务端拒绝日志含 3 条"拒绝越权重命名名称段"，victim 完好 | 证据 `project-docs/evidence/REQ-FS-003/repair/`；handoff `req-fs-003-repair-2026-09-29.txt` |
| 2026-09-29 | **待独立复审会话（auditor ≠ developer）确认 A6 后由主人通知合并**；复审前合并禁令维持（强制门） | 本会话未合并 master |
| 2026-09-29 | **独立复审会话（只读 worktree @a518e35）判定 PASS**：F-1 关闭、A6 达标、范围合规、证据链核验无伪造；新增观察 O-5（Low，`.`/`..` 纯名依赖系统调用兜底，与 MKDIR 同模式，不阻塞） | 报告 `project-docs/evidence/REQ-FS-003/reaudit-report.md` |
| 2026-09-29 | **主人指令合并（"继续"）**：三事实线齐（复审 PASS / frozen-path 空 / pipeline GREEN）后 `fix/path-traversal-and-ownership` no-ff 合入 master @`f937c90`；合并结果 build GREEN / test 0 注册项 / audit PASS | push origin 未做，待主人指令；REQ-FS-003 关闭 |
