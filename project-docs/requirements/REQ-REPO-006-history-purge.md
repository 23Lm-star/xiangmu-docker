# REQ-REPO-006　Git 历史清理（⚠️ 不可逆，需专项批准）

| 项目 | 值 |
|---|---|
| **需求编号** | REQ-REPO-006 |
| **分支名** | `chore/history-purge` |
| **任务等级** | **SMALL（但操作不可逆 → 必须专项批准）** |
| **基线 commit** | `b5d7bb3`（编写时 master HEAD） |
| **流程版本** | Development OS v1.7 |
| **状态** | **⛔ 待主人明确批准后才可执行** |

---

## 一、背景（为什么要做）

REQ-REPO-004 只解决"**以后**不再跟踪"，**改变不了历史**——`Server/connect.config` 的密码、`project-master/vod/` 的测试包，**仍留在每一次 commit 的快照里**。

### 已核实的事实（实测）

| 项 | 实测结果 |
|---|---|
| `.git` 目录体积 | **51 MB** |
| 工作区体积（不含 .git） | **6.2 GB** |
| 历史中 `project-master` 路径出现次数 | **91** 个不同路径 |
| 最大 blob | `project-master/vod/TestFile/视频共享系统自动化测试.zip` = **23.9 MB** |
| 第 2-8 大 blob | `moban2023.zip` 3.3MB、`Client/mag.png` 2.9MB、`vod/test` 1.4MB、`视频共享系统测试报告.zip` 1.1MB、`editors-large.jpg` 549KB、`httplib.h` 355KB、`Server/qtbase/bin/licheck.exe` 350KB |
| `git-filter-repo` 是否可用 | ❌ **未安装** |
| `git filter-branch` | git 自带，但**极慢且官方不推荐** |

### 为什么要清

1. **凭据残留**：`Server/connect.config` 里的 `root/123456` 在所有历史 commit 中可被 `git log -p` 挖出。**推到 public 仓库 = 密码永久公开**（即使当前版本已移除）。
2. **体积**：`.git` 51MB 里绝大部分是 `project-master/vod/` 的测试包与 `qtbase` 二进制。这些从不属于这个项目。
3. **观感**：git 历史里混着 91 个无关路径，污染项也污染历史。

---

## 二、⚠️ 风险声明（执行前必须逐条读）

| # | 风险 | 后果 |
|---|---|---|
| R1 | **重写历史会改变所有 commit 哈希** | 所有已存在的分支、tag 全部失效 |
| R2 | **如果已推送过，重写后必须 force-push** | 会覆盖远端历史，协作者必须重新 clone |
| R3 | **旧 commit 在 `reflog` / `dangling` 中仍可恢复（一段时间内）** | 说明"删干净"需要额外 `gc`，但也会丧失退路 |
| R4 | **操作中途失败可能导致仓库损坏** | 必须用**完整副本**操作，不要在原仓库上试 |
| R5 | **`.git` 里的 reflog 会增长** | `gc --prune=now` 之后才真正释放 |

> **乐观的一点**：本项目**尚未推送到 GitHub**（`origin/master` 落后本地 7 个 commit，且这是首次推送前的准备）。**没有协作者**，所以没有"破坏别人工作"的问题。这是一个**做历史重写的最佳时机**——推上去之后再改就要面对 force-push 的连锁反应。

---

## 三、决策点：先回答三个问题

**执行前必须由主人明确回答：**

| 问题 | 选项 | 我的建议 |
|---|---|---|
| Q1. 要清到什么程度？ | A. 只清凭据（`Server/connect.config`）<br>B. 清凭据 + `project-master/vod/` 测试包<br>C. 清凭据 + 测试包 + `qtbase` 二进制 | **B**（凭据是硬需求，测试包是体积大头；qtbase 已在 e700d22 处理过部分） |
| Q2. 保留旧历史吗？ | A. 完全重写（哈希全变）<br>B. 不重写，接受历史里有密码（**前提：改掉密码本身**） | **A**（首次推送前，成本最低） |
| Q3. 用哪个工具？ | A. `git filter-repo`（需先装）<br>B. `git filter-branch`（自带，慢） | **A**（官方推荐，快且安全） |

> **如果 Q2 选 B**：那前提是**先改掉数据库密码**（此密码在真实环境仍在使用的话），因为历史里的 `root/123456` 一旦公开就是有效凭据。**这种方案下本需求降级为"不改历史，只改密码 + 更新文档"，成本低得多。**
> **但这个密码看起来是本地开发的默认值**（`root/123456` 是 MySQL 最常见的本地默认），风险相对可控——**仍需主人判断它是否在别处复用**。

---

## 四、技术方案（若 Q2 选 A：重写）

### 4.1 前置：安装工具

```bash
# git-filter-repo 是单文件 Python 脚本，推荐 pip 安装到隔离环境
"C:/Users/王晨扬/.workbuddy/binaries/python/envs/default/Scripts/pip.exe" install git-filter-repo

# 验证
"C:/Users/王晨扬/.workbuddy/binaries/python/envs/default/Scripts/git-filter-repo.exe" --version
```

> 若 pip 安装受阻，可直接从官方仓库下载单文件脚本放到 PATH。

### 4.2 前置：完整备份（**不可跳过**）

```bash
# 用 git clone --mirror 做完整镜像备份（比 cp -r 更可靠，会带上所有 ref）
git clone --mirror E:/qt/xiangmu E:/qt/xiangmu-backup-$(date +%Y%m%d-%H%M%S).git

# 验证备份完整性
cd E:/qt/xiangmu-backup-<时间戳>.git && git rev-list --all --count
# 与原始仓库对比：cd /e/qt/xiangmu && git rev-list --all --count
# 两者必须一致
```

> ⚠️ **备份必须放在仓库之外**（如 `E:/qt/` 同级），**不能放在仓库内部**（否则会被一起处理）。

### 4.3 执行清理

```bash
cd /e/qt/xiangmu

# 清凭据文件（历史全路径）
"C:/Users/王晨扬/.workbuddy/binaries/python/envs/default/Scripts/git-filter-repo.exe" \
  --path Server/connect.config --invert-paths --force

# 清测试包目录（若 Q1 选 B/C）
"C:/Users/王晨扬/.workbuddy/binaries/python/envs/default/Scripts/git-filter-repo.exe" \
  --path project-master/ --invert-paths --force

# 清 qtbase（若 Q1 选 C）
"C:/Users/王晨扬/.workbuddy/binaries/python/envs/default/Scripts/git-filter-repo.exe" \
  --path Server/qtbase/ --invert-paths --force
```

> `git-filter-repo` 每次运行会**移除 `origin` remote**（安全设计，防止误推）。清完后需按 §4.5 重新加回。
> 也可以一条命令清多个路径：`--path X --path Y --path Z --invert-paths`。

### 4.4 真正释放空间

```bash
git reflog expire --expire=now --all
git gc --prune=now --aggressive
```

### 4.5 恢复 remote

```bash
git remote add origin <GitHub 仓库 URL>
# 检查
git remote -v
```

### 4.6 验证

```bash
# 1. 凭据已从全部历史消失（应为空输出）
git log --all --oneline -- Server/connect.config
git log --all -p -S"123456" -- Server/ | head

# 2. 体积下降
du -sh .git

# 3. commit 数（会变少）
git rev-list --all --count

# 4. 当前工作区文件仍在
ls Server/connect.config          # 必须存在（本地文件，只是不再被跟踪）
powershell -File pipeline/build.ps1
```

---

## 五、允许改动的文件

```
（无源码文件改动）
.git/ 内部历史（重写）
project-docs/tasks/REQ-REPO-006/task.md
project-docs/evidence/REQ-REPO-006/
```

---

## 六、禁止事项（⚠️ 违反即停止）

| # | 禁止 | 原因 |
|---|---|---|
| 1 | **禁止在无完整备份的情况下执行** | 操作不可逆 |
| 2 | **禁止把备份放在仓库目录内** | 会被一起重写 |
| 3 | **禁止用 `git filter-branch` 代替 filter-repo**（除非主人明确要求） | 慢且易错 |
| 4 | **禁止跳过 §4.2 的备份验证（rev-list 数量必须相等）** | 备份可能是坏的 |
| 5 | **禁止 `git push --force`** | **必须主人单独批准**，本需求不含推送 |
| 6 | **禁止在本需求里改数据库密码** | 那是环境操作，另说 |
| 7 | **禁止清理后立刻删备份** | 至少保留一个版本周期 |
| 8 | **禁止在 `git gc` 前删备份** | gc 之后才真正不可恢复 |
| 9 | **禁止修改任何源码或配置** | 本需求只碰 .git |

---

## 七、执行流程（Development OS v1.7）

> ⚠️ 本需求的"分支"意义与传统不同：历史重写**作用于整个仓库**，不是某个分支的改动。所以流程调整为：

```bash
# ① 开场
powershell -File pipeline/state.ps1
#    ⚠️ 必须确认无未提交改动、无 suspended git state

# ② 【批准门】确认主人已回答 Q1/Q2/Q3
#    未回答 → 停止

# ③ 全量备份 + 验证（§4.2）

# ④ 安装 git-filter-repo（§4.1）

# ⑤ 执行重写（§4.3）
#    重写后注意：当前分支的 HEAD 已变，分支名可能被 filter-repo 保留

# ⑥ gc 释放（§4.4）

# ⑦ 恢复 remote（§4.5）

# ⑧ 验证（§4.6）
powershell -File pipeline/build.ps1
powershell -File pipeline/test.ps1
powershell -File pipeline/audit.ps1

# ⑨ 写 task.md + handoff
# ⑩ 停下。推送（含可能的 force-push）由主人单独批准
```

> **本需求不创建传统分支。** `chore/history-purge` 这个名字只用于 task 记录与 handoff 标识。

---

## 八、任务记录与 handoff

### 8.1 `project-docs/tasks/REQ-REPO-006/task.md`

**必须记录**：
- 主人对 Q1/Q2/Q3 的回答
- 备份路径 + 备份验证结果（rev-list 数）
- 每条 filter-repo 命令的完整输出
- 重写前后的 commit 数、`.git` 体积对比
- 哪些 blob 被清掉了

### 8.2 handoff

`project-docs/handoffs/chore-history-purge-<日期>.txt`，**严格 10 行**：

```text
# handoff: REQ-REPO-006
branch: master
head: <重写后 master 的 commit 完整哈希>
tier: SMALL
node: merged
audit: none
next: powershell -File pipeline/state.ps1
blockers: push requires owner approval (history rewritten)
evidence: project-docs/evidence/REQ-REPO-006/
owner: claude-code
date: <YYYY-MM-DD>
```

> ⚠️ `blockers` 一行必须写明"推送需主人批准，历史已重写"。这是给未来会话的强提醒。

```bash
python "C:/Users/王晨扬/.codex/skills/development-os-v1-7/scripts/validate-handoff.py" <handoff 路径>
```

---

## 九、遇到的边界情况怎么办

| 情况 | 怎么办 |
|---|---|
| 主人没回答 Q1/Q2/Q3 | **停止**，不执行 |
| 备份 rev-list 数量对不上 | **停止**，重做备份。不要将就 |
| filter-repo 报错中断 | **停止**，从备份恢复，报告。不要在损坏的仓库上继续 |
| 重写后发现少了需要的 commit | 从镜像备份恢复：`git fetch <备份路径> --all` |
| 想顺便删废弃分支 | **不做**，另开需求 |
| 重写后 `pipeline/audit.ps1` 报 `NO test evidence` | 正常（哈希变了）→ 先跑 `test.ps1` |
| 想清理完立刻 force-push | **禁止**。主人单独批准 |
| 想清理完删掉备份 | **禁止**，保留 |

---

## 十、完成后请回报这些

1. 主人对 Q1/Q2/Q3 的回答
2. **备份路径 + 备份验证结果**（两个 rev-list 数字）
3. `git-filter-repo` 每条命令的完整输出
4. **重写前 vs 重写后**：commit 数、`.git` 体积、最大的 blob 是否消失
5. §4.6 四项验证的实际输出（尤其"凭据已从历史消失"）
6. 三项 pipeline 实际输出
7. handoff 校验脚本输出
8. **明确说明：尚未推送**

**不要自行推送，尤其不要 force-push。**
