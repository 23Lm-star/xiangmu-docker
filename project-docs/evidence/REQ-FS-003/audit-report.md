# REQ-FS-003 独立审计报告 —— 结论：FAIL（禁止合并）

- 审计对象：分支 `fix/path-traversal-and-ownership`，代码提交 `f808daf907602efecd6b9e9d2fc523dd1250d96f`
- 审计方式：只读 worktree @f808daf（用毕已删）；manifest sha256 全匹配（cpp 86693d9b…/h f46e984e…）；独立运行 `pipeline/audit.ps1` → PASS；逐行审查 b5d7bb3..f808daf diff 对照 A1–A12
- 独立性：审计会话与 dev 会话分离；未改动任何产品源码

## 判定

| 项 | 结果 |
|---|---|
| A1 A2 A3 A4 A5 A7 A8 A9 A10 A11 A12 | **通过**（代码审查 + 哈希绑定的 25/25 端到端证据 + 门禁独立复跑） |
| A6（重命名越权被拒且目标完好） | **不通过** —— 见 F-1 |
| 范围合规 | 通过：仅 §4 两文件；`resumableuploadsession.*`、两 `protocol.h` 未动；frozen-path 无命中 |

## F-1（High，阻塞合并）：renamefile 名称段未校验，`../` 绕过目录校验

`Server/msghandler.cpp:538-539`：`oldFilePath = dirPath + "/" + oldFileName`、`newFilePath = dirPath + "/" + newFileName`。`dirPath`（caMsg）过了 `isPathInSandbox`，但 `oldFileName/newFileName` 来自 caData 64 字节，客户端完全可控，**无分隔符/`..` 检查**。攻击样例：`dirPath=./filesys/<user>`（合法），`oldFileName=../../Server/msghandler.cpp` → 拼接串在 Windows 路径归一后指向沙箱外，`QFile::rename` 对沙箱外文件动手——正是需求 §一缺陷 2 要关闭的"先动手"面。

- 证据链：dev 测试 S6 仅以 dirPath 字段构造逃逸（`server-reject-lines.log` 无任何名称段拒绝行）；同类面已在 MKDIR（S8c `evil\..\out` 被拒）与 sharefilemove（`\` 检查）修复，renamefile 为唯一遗漏。
- 根因：需求 §3.2③ 的前提"旧名/新名共用同一目录⇒校验 dirPath 一处即可"默认了名称是裸文件名，对恶意 PDU 不成立——属规格盲区，实现忠实于规格。
- 修复方向（供 owner 决策，二选一）：① 对拼接后的 `oldFilePath` 做 `isPathInSandbox`（rename 源必存在，canonical 有效，不触发 A7 陷阱；newFilePath 维持不校验）；② 与 MKDIR 一致对两个名称段做 `/`、`\` 拒绝。修复后需补一条名称段逃逸测试并重审。

## 残余观察（不阻塞，记录在案）

- O-1 verify-then-act：校验用 canonical、执行用原串，理论 TOCTOU 需沙箱内符号链接置换原语，当前入口（上传不落符号链接）不存在该原语，风险低。
- O-2 默认根 `./filesys` 经 `QDir().absolutePath()` 以进程 CWD 为基准，CWD 漂移则根漂移；f808daf 运行时一致性已由端到端证据坐实。
- O-3 `char[32]` memcpy 满 32 字节时无 NUL 终止（renamefile:530-533 等既有模式），`fromUtf8` 越读——基线既有问题，超本需求范围，建议另开分支。
- O-4 仓库 `pipeline/audit.ps1` 未实现 v1.7 的 docs-only 证据回溯，docs 提交上直接运行会误报缺证据；本审计以 worktree @代码提交哈希规避。

## 处置

F-1 修复会话（owner）→ 重新盖章证据 → 复审通过后才允许合并 master。在此之前"继续/快点"不能放行合并（强制门）。
