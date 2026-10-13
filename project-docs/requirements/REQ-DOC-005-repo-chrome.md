# REQ-DOC-005　仓库门面：README + LICENSE + 分支整理

| 项目 | 值 |
|---|---|
| **需求编号** | REQ-DOC-005 |
| **分支名** | `docs/repo-chrome` |
| **任务等级** | **SMALL** |
| **基线 commit** | `b5d7bb3`（编写时 master HEAD） |
| **流程版本** | Development OS v1.7 |
| **状态** | 待执行 |

---

## 一、背景（为什么要做）

项目要推 GitHub，但仓库**缺少所有对外门面文件**。实测确认：

| 项 | 实测结果 |
|---|---|
| `README.md` | ❌ 不存在 |
| `LICENSE` | ❌ 不存在 |
| `.github/` | ❌ 不存在 |
| 现存分支 | `master` + `backup-runnable-2026-07-30` + 4 个远端分支 |
| manifest 记录的分支情况 | `stale branches pending user decision: origin/main (dead), backup-runnable-2026-07-30 (local+remote)` |
| `origin/main` | 已废弃（manifest: "origin/main is dead, do not use"），远端 HEAD 已 set-head 到 master |

别人点进仓库看到的是"一堆代码 + 一堆不知名分支"，没有任何说明。**这是"能不能让别人看懂"的问题，不是代码缺陷**，所以是 SMALL。

---

## 二、目标（验收标准）

| # | 验收标准 | 怎么验证 |
|---|---|---|
| A1 | 仓库根有 `README.md`，能说明项目是什么、怎么跑起来 | 按 README 步骤能构建成功 |
| A2 | README 含依赖、构建（qmake/MinGW）、运行三步 | 人工按步走一遍 |
| A3 | 仓库根有 `LICENSE` | 文件存在，是标准许可证全文 |
| A4 | README 明确标注**这是学习/课程项目**，避免被误认为生产代码 | 文本审查 |
| A5 | README 说明 `connect.config` 需从 `.template` 复制 | 与 REQ-REPO-004 呼应 |
| A6 | 废弃分支的处理**有明确结论**（保留或删除，并说明理由） | 见 §3.3 |
| A7 | 三项 pipeline 全绿 | GREEN / GREEN / PASS |

---

## 三、技术方案（已定型）

### 3.1 README.md 结构（照这个写，不要自由发挥）

```markdown
# 项目名（一句话定位）

> 学习项目说明：本项目为个人学习/课程实践项目，非生产环境代码，
> 未做完整的安全加固与性能压测。请勿直接用于生产。

## 功能概览
- 用户注册 / 登录（含验证码冷却与锁定）
- 好友管理（添加、删除、刷新）
- 私聊消息
- 文件共享（上传/下载/断点续传/重命名/移动/删除）
- 分享链接（外链分享、发布链接）

## 技术栈
- C++11 / Qt 5.14.2（MinGW 7.3.0 64bit）
- 自研 PDU 二进制协议（TCP）
- MySQL（服务端持久化）
- 自研内存池组件

## 目录结构
（Client/ 客户端，Server/ 服务端，project-docs/ 治理与文档，pipeline/ 构建测试流水线）

## 构建
### 依赖
- Qt 5.14.2 mingw73_64（C:\Qt\5.14.2\mingw73_64）
- MinGW 7.3.0（C:\Qt\Tools\mingw730_64）
- MySQL 服务端

### 配置
1. 复制 `Server/connect.config.template` → `Server/connect.config`，填入真实数据库信息
2. 复制 `Client/connect.config.template` → `Client/connect.config`，填入服务端地址

### 构建
    powershell -File pipeline/build.ps1

## 运行
1. 初始化数据库：执行 `project-docs/init_test_data.sql`
2. 启动服务端
3. 启动客户端

## 文档
- 架构：`project-docs/architecture.md`
- 模块：`project-docs/modules.md`
- 业务功能：`project-docs/业务功能文档.md`
- 缺陷与修复记录：`project-docs/defects/`、`project-docs/fix-specs/`

## 许可证
见 LICENSE
```

> ⚠️ **README 里不要出现任何真实密码、IP、密钥。** 只能出现 `127.0.0.1` 这类示例值。

### 3.2 LICENSE

**让主人决定许可证类型**（AI 不应擅自选择）。默认建议：

- 如果是**课程作业 / 怕被抄** → 用 **MIT**（最简单，别人能看懂）或**不加许可证**（默认保留所有权利）
- 如果想明确"仅供学习" → 用 **MIT + README 免责声明**
- 如果要用 **GPL-3.0** → 需要主人确认

> **执行前先问主人选哪个**。**不要**自行填一个。若因故无法询问，**默认 MIT** 并在 task.md 决策日志里标注"待主人确认"。

### 3.3 废弃分支的处理

manifest 明确记录了两个待决分支：

| 分支 | 现状 | 建议 | 理由 |
|---|---|---|---|
| `origin/main` | 已废弃，远端 HEAD 指向 master | **保留不动**（不删远端） | 删远端分支是**不可逆**操作，且 set-head 已让默认分支是 master，不影响观感 |
| `backup-runnable-2026-07-30` | 本地 + 远端都有 | **保留本地，远端留给主人决定** | 它是"可运行备份"，删了就没退路 |
| `origin/add_数据库连接池功能` | 远端存在 | **保留** | 有功能价值，可能是未合并的功能分支 |

> ⚠️ **本需求不删除任何分支。** 分支删除属不可逆操作，**必须主人逐个批准**。本需求只做：**在 README 或 task.md 里把这些分支的处置建议写清楚**，交给主人拍板。
> 如果主人明确指示删某个分支 → 那属于**另一个需求**（且需要独立批准流程），**不在本分支做**。

### 3.4 可选：`.gitattributes`

建议加一个最小 `.gitattributes`，统一行尾（项目里有 `.ps1`、`.cpp`、`.md`）：

```gitattributes
* text=auto
*.ps1 text eol=crlf
*.sh  text eol=lf
*.cpp text eol=lf
*.h   text eol=lf
*.md  text eol=lf
*.sql text eol=lf
```

> 这是**可选项**。若加，注意它会改变 git 对行尾的处理，可能导致大量文件"看起来被改"（实际只是行尾归一）。**若加，必须先跑 `git status` 确认没有大规模误报**，一旦出现大面积改动**立即回退**。
> **首次推送前加 `.gitattributes` 是较优时机**（还没推上去，不会有历史包袱）。

---

## 四、允许改动的文件（allowed paths）

```
README.md                    ← 新建
LICENSE                      ← 新建（类型待主人确认）
.gitattributes               ← 新建（可选，见 §3.4）
project-docs/tasks/REQ-DOC-005/task.md
project-docs/evidence/REQ-DOC-005/
```

---

## 五、禁止事项（⚠️ 违反即停止并上报）

| # | 禁止 | 原因 |
|---|---|---|
| 1 | **禁止删除或重命名任何分支** | 不可逆，需主人逐个批准 |
| 2 | **禁止 `git push`（含推分支）** | 不可逆 |
| 3 | **禁止在 README/LICENSE 里放真实凭据/IP/密钥** | 门面文件要能公开 |
| 4 | **禁止自行选定 LICENSE 类型**（除 §3.2 的默认兜底） | 法律选择归主人 |
| 5 | **禁止修改任何源码** | 本需求纯文档 |
| 6 | **禁止改 `project-docs/` 下已有的架构文档** | 超出范围 |
| 7 | **禁止在 README 里承诺不存在的功能** | 只写实测存在的功能 |
| 8 | **若加 `.gitattributes` 导致大面积改动 → 立即回退** | 见 §3.4 |

---

## 六、执行流程（Development OS v1.7）

```bash
# ① 开场
powershell -File pipeline/state.ps1

# ② 切分支
git switch master
git switch -c docs/repo-chrome

# ③ 写 README.md / LICENSE /（可选）.gitattributes

# ④ 跑门
powershell -File pipeline/build.ps1
powershell -File pipeline/test.ps1
powershell -File pipeline/audit.ps1

# ⑤ 验证 README 步骤可执行（见 §7）

# ⑥ 提交
git add README.md LICENSE .gitattributes    # .gitattributes 若未加则去掉
git commit -m "docs(repo): add README, LICENSE and line-ending policy"

# ⑦ 合并
git switch master
git merge --no-ff docs/repo-chrome -m "merge: REQ-DOC-005 repo chrome (SMALL)"
git branch -d docs/repo-chrome

# ⑧ 停下等批准
```

---

## 七、需要的验证证据

| 验证 | 怎么验 |
|---|---|
| README 步骤可执行 | **照 README 走一遍**：复制 template → 构建 → 记录是否成功 |
| README 无敏感信息 | 全文搜索密码/IP/密钥关键词，确认只有 `127.0.0.1` 这类示例 |
| README 功能与实际一致 | 逐条对照 `project-docs/业务功能文档.md` |
| LICENSE 完整 | 是标准许可证全文，没有被截断 |
| 分支未被动过 | `git branch -a` 与执行前一致 |

### 证据目录

```
build/evidence/docs-repo-chrome/<commit-hash>/
    pipeline-run.log
    manual-verify.log
    manifest.txt
```

```bash
python "C:/Users/王晨扬/.codex/skills/development-os-v1-7/scripts/record-evidence.py" \
  --evidence-dir build/evidence/docs-repo-chrome/<hash> \
  --repo E:/qt/xiangmu \
  --label "REQ-DOC-005: README + LICENSE + line-ending policy" \
  README.md LICENSE
```

---

## 八、任务记录与 handoff

### 8.1 `project-docs/tasks/REQ-DOC-005/task.md`

含：档位 + 依据、验收标准、allowed paths、决策日志。
**必须记录**：LICENSE 类型的选择（或"待主人确认"）、对 4 个遗留分支给出的处置建议。

### 8.2 handoff

`project-docs/handoffs/docs-repo-chrome-<日期>.txt`，**严格 10 行**：

```text
# handoff: REQ-DOC-005
branch: master
head: <合并后 commit 完整哈希>
tier: SMALL
node: merged
audit: none
next: powershell -File pipeline/state.ps1
blockers: none
evidence: project-docs/evidence/REQ-DOC-005/
owner: claude-code
date: <YYYY-MM-DD>
```

```bash
python "C:/Users/王晨扬/.codex/skills/development-os-v1-7/scripts/validate-handoff.py" <handoff 路径>
```

---

## 九、遇到的边界情况怎么办

| 情况 | 怎么办 |
|---|---|
| 不确定 LICENSE 选哪个 | **问主人**。不确认就用 MIT 并在日志标注 |
| 主人要求删分支 | **不在本分支做**。另开需求，需要独立批准 |
| `.gitattributes` 引发大面积 status 变化 | **立即回退**（`git checkout -- .gitattributes`），报告 |
| README 里想放截图 | 可放，但图片要放仓库内（如 `docs/images/`）；**不要**引用外链 |
| 发现 README 里想写的功能和实际不符 | 以**实际代码**为准，README 改口，或先报告 |
| `origin/main` 需要删 | 同"主人要求删分支"，另开需求 |

---

## 十、完成后请回报这些

1. 最终 commit 哈希（分支 + 合并）
2. README 全文（或摘要）
3. LICENSE 类型（及是否已获主人确认）
4. 是否加了 `.gitattributes`；若加了，`git status` 有无异常
5. 4 个遗留分支的处置建议
6. 三项 pipeline 实际输出
7. handoff 校验脚本输出

**不要自行推送，不要自行删分支。**
