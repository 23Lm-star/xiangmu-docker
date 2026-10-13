# ADR-012 多 Agent 隔离、唯一集成与变更门禁

- 状态：提议（Proposed，待用户确认后生效）
- 日期：2026-07-30
- 提出：项目架构师（协同治理审计补充校正）

## 背景（Context）
经协同治理审计，确认当前系统处于“多个 Agent 在同一目录中依赖自然语言约束的自由编辑”状态：
- 规则设计层：角色/交接/状态流转制度存在，但有模糊授权与边界不清（如 Bug 查找师“原则上”不改代码、工程师自检与正式测试未分层、架构师未禁 git 直接操作）。
- 执行控制层：接近空白——无权限隔离、任务锁、路径白名单、独立 worktree、唯一集成者或自动检查。
- 运行现场层：单一工作区混合 Client/Server/协议/DB/UI/构建/治理文档变更（当前 32 个已修改跟踪文件 + 43 个未跟踪文件），无法可靠绑定到唯一任务、唯一提交、唯一测试版本。

文档告诉 Agent 应协作，但系统没有能力阻止越界。

## 决策（Decision）
引入“执行控制层”，与既有规则设计层、运行现场层构成三层治理：
1. **任务隔离**：每个任务从不可变基线 `base_commit` 切独立分支/worktree，禁止共享工作树直接开发。
2. **唯一集成者**：全仓库同一时刻仅一个集成者负责合并；其余角色禁止直接 commit/push/merge 主线、禁止兼任集成者。
3. **路径白名单 + 受保护区**：任务契约声明 `allowed_paths`/`protected_paths`；越界 diff 由脚本/钩子自动拒绝。
4. **任务锁**：`active-locks.yaml` 防止同模块并发写冲突。
5. **证据等级绑定**：测试报告必须记录被测 commit 与 evidence_level；等级不足时结论降级（见 evidence-levels.md）。
6. **变更门禁**：合并前校验 task_id、基线、diff 范围、提交链、证据等级、审批、锁释放（见 integration-policy.md）。

配套修正规则文本漏洞：Bug 查找师改为绝对禁止改产品代码；明确工程师自检≠正式测试的分层；架构师显式禁止直接 git 操作与兼任集成者。

## 后果（Consequences）
- 正面：越界可被机器拒绝；任务/提交/测试版本可绑定；缺陷关闭结论有据可依；责任可追。
- 负面/成本：需引入分支/worktree 工作流与一次门禁脚本落地；Agent 提示词需增加“先建契约再开工”的步骤；短期有迁移当前混合代码的一次性成本。
- 风险：若门禁脚本未全量部署，需集成者手动兜底，存在人为疏漏窗口（缓解：强制手动运行脚本直至钩子上线）。

## 过渡方案（Transition）
1. **现场保全**（先做，不动代码）：冻结主工作区写入；建保护分支/完整快照；按 R17/R18/REQ-AUTH-001/UI构建/治理文档分组建立变更考古清单。
2. **治理设计**（本 ADR 及五个 governance 文件）：完成并经用户确认。
3. **迁移**：仅将通过重新审计的最小 patch 移入新建隔离 worktree；最后再决定是否清理共享主工作区。
4. **门禁落地**：先手动脚本兜底，再接入 git 钩子/CI。

## 参考
- project-docs/governance/task-template.yaml
- project-docs/governance/path-ownership.yaml
- project-docs/governance/integration-policy.md
- project-docs/governance/evidence-levels.md
- project-docs/governance/active-locks.yaml
- 协同治理审计补充校正报告（2026-07-30）
