# REQ-FS-003 F-1 复审报告 —— 结论：PASS（F-1 关闭，建议放行合并）

- 复审对象：分支 `fix/path-traversal-and-ownership`，代码提交 `a518e3520a9b21869c07c141039398ff4a2da9a0`
- 复审方式：只读 worktree @a518e35（用毕即删）；逐行审查 `git diff f808daf..a518e35`；独立复跑 `pipeline/audit.ps1`（补跑 test.ps1 后 `audit PASS (a518e35)`，frozen-path/hygiene 零命中）
- 独立性：复审会话与开发/修复会话分离；未改动任何产品源码
- 范围：仅复核 F-1 关闭、A6 达标、修复有无引入新问题/回归；首轮已通过的 A1–A5/A7–A12 抽查无回归

## 判定

| 项 | 结果 |
|---|---|
| F-1（名称段分隔符逃逸） | **已关闭**：两个名称段在 rename 执行前均被拒绝（msghandler.cpp:539-540 fixedString，:553-558 分隔符拒绝块先于 :560-566 rename），拼接不可能离开 dirPath；与 MKDIR 同模式 |
| A6 | **达标**：S6b/S6c/S6d 构造真实对应 F-1 逃逸面（dirPath 合法、逃逸全由名称段承载），断言含 ret=False + 目标/源文件完好；28/28 与逐项核对一致 |
| 联动段 | 复用净化名（:588-592），仅 ret=true 可达，DB 写入面不可能收到含分隔符名称；参数绑定 SQL 无注入面 |
| 范围合规 | 代码仅 renamefile 函数体（15+/6-）；两 `protocol.h`、`ResumableUploadSession` 未动；S1–S5/S8–S12 复跑全绿，S7 正常不受影响 |
| 证据链 | build 章戳 20260929-130027 晚于提交 9s，证据提交 ed7bd6f 直接位于 a518e35 之上，拒绝日志参数与 harness 逐字符匹配，无伪造迹象 |

## 发现

- 阻塞项：**无**。
- O-5（Low，观察，非阻塞）：无分隔符纯名 `.`/`..` 及 drive 相对名不做字符串级拒绝，依赖 rename 系统调用失败兜底（Windows 平台上均失败且目标仍在沙箱树内）；与 MKDIR/sharefilemove 既有模式一致。若未来移植 POSIX，建议在 isPathInSandbox 层统一加名称段黑名单。
- O-1–O-4（首轮残余观察）：本次复审未改变其风险面（O-3 在两个名称字段使用点被 fixedString 自然收窄），处置不变。

## 处置

合并三事实线已齐（复审报告存在且接受 / frozen-path diff 空 / pipeline GREEN @a518e35）→ 待主人通知后合并 master（主人既定规则）。
