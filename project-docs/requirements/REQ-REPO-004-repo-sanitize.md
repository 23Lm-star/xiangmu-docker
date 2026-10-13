# REQ-REPO-004　仓库卫生：清理误跟踪文件 + 补齐忽略规则

| 项目 | 值 |
|---|---|
| **需求编号** | REQ-REPO-004 |
| **分支名** | `chore/repo-sanitize` |
| **任务等级** | **SMALL** |
| **基线 commit** | `b5d7bb3`（编写时 master HEAD） |
| **流程版本** | Development OS v1.7 |
| **状态** | 待执行 |

---

## 一、背景（为什么要做）

`e700d22` 那次清理已经拿掉了 `Server/qtbase/`、`Server/filesys/` 这类大头，但**漏了客户端侧的连接配置**，且 `.gitignore` 的凭据规则只覆盖了 Server 一半。

### 已核实的事实（实测）

| 项 | 实测结果 |
|---|---|
| `Client/connect.config` 是否被跟踪 | ✅ **仍然被跟踪**（`git ls-files Client/connect.config` 有输出） |
| 内容 | `127.0.0.1` / `5000` / `./filesys` —— **不含密码** |
| `.gitignore` 现有凭据规则 | 只挡了 `/Server/connect.config`、`/Server/connect.config.bak-*`、`/Client/connect.config.local` |
| 被跟踪的文件总数 | 226 |
| `Server/release/`、`Client/release/` 磁盘上存在吗 | 存在，但**已被 `.gitignore` 正确忽略**（`git status --ignored` 显示 `!!`） |
| 被跟踪的 `release/` 产物 | **0 个**（已被忽略，不是缺陷） |

### 缺陷性质：P2（配置/卫生），不是 P0

`Client/connect.config` 里**没有密码**，只有连接地址。但它**属于"每台机器不同"的本地配置**：

- 换机器/换网络就要改，跟踪它会导致每次拉取都有本地改动噪音
- 它被打进 `Client/config.qrc` 作为 Qt 资源（`<file>connect.config</file>`），所以**本地文件必须存在**（删了资源就编译不过）
- 一旦有人往里加了端口/路径以外的信息（比如以后加 token），就会变成真凭据泄露

**结论**：**取消跟踪，保留本地文件，补一个 `.template`**。与 `Server/connect.config` 的处理方式完全一致（那次是 REQ 分支 1 做的，照同一套路）。

---

## 二、目标（验收标准）

| # | 验收标准 | 怎么验证 |
|---|---|---|
| A1 | `Client/connect.config` **不再被 git 跟踪** | `git ls-files Client/connect.config` 输出为空 |
| A2 | `Client/connect.config` **本地文件依然存在**（qrc 要它） | `ls Client/connect.config` 有输出 |
| A3 | 存在 `Client/connect.config.template` 作为参考模板 | 文件存在且内容结构一致（3 行） |
| A4 | `.gitignore` 规则覆盖 Client 侧 | 有 `/Client/connect.config` 规则 |
| A5 | 编译照常通过（qrc 仍能嵌入 connect.config） | `build.ps1` GREEN |
| A6 | 工作区 `git status` 干净 | 无未提交改动 |
| A7 | 三项 pipeline 全绿 | GREEN / GREEN / PASS |

---

## 三、技术方案（已定型）

### 3.1 步骤（严格按顺序，**不要合并命令**）

> ⚠️ **血的教训**：分支 1 执行时把 `git add <template>` 和 `git rm --cached <config>` 放在**同一条命令**里，git 的重命名检测把两件事当成一次 rename，**把工作区文件也删了**。恢复花了额外一轮。
> **本需求必须分步执行，每步之后检查。**

```bash
# 步骤 1：先备份（保命）
cp Client/connect.config Client/connect.config.bak-$(date +%Y%m%d)

# 步骤 2：确认文件当前状态
git ls-files Client/connect.config        # 应有输出（说明被跟踪）
ls Client/connect.config                  # 应有输出（说明磁盘上有）

# 步骤 3：创建模板（内容同源，仅作参考）
cp Client/connect.config Client/connect.config.template

# 步骤 4：单独执行 —— 取消跟踪（--cached 不删工作区文件）
git rm --cached Client/connect.config

# 步骤 5：检查 —— 这一步不能省
ls Client/connect.config                  # 【必须仍有输出】若没有 → 立即从步骤1的备份恢复
git ls-files Client/connect.config        # 【必须为空】

# 步骤 6：单独执行 —— 添加模板
git add Client/connect.config.template

# 步骤 7：编辑 .gitignore，在凭据段落追加
```

### 3.2 `.gitignore` 追加内容

在现有凭据段落（约 73-77 行）合并成：

```gitignore
# Local credentials -- never track. Use connect.config.template as the reference.
/Server/connect.config
/Server/connect.config.bak-*
/Client/connect.config
/Client/connect.config.bak-*
/Client/connect.config.local
```

> `Client/connect.config.template` **必须被跟踪**（它是参考模板），**不要**把它也忽略掉。
> `Client/connect.config.bak-*` 是备份文件，不该进库。

### 3.3 内容形态

`Client/connect.config.template` 内容保持与真实文件同结构（3 行，**不含任何敏感值**）：

```
127.0.0.1
5000
./filesys
```

> 因为它本来就没有密码，模板与真实文件**内容相同**是可接受的。**不要**为了"看起来像模板"而加 `<YOUR_XXX>` 占位符——那会让 qrc 嵌入的模板失效、误导读者。它的作用是"告诉别人这个文件长什么样、该放哪"。

---

## 四、允许改动的文件（allowed paths）

```
.gitignore                          ← 追加 Client 侧规则
Client/connect.config.template      ← 新建
Client/connect.config               ← 仅取消跟踪（不从磁盘删除）
project-docs/tasks/REQ-REPO-004/task.md
project-docs/evidence/REQ-REPO-004/
```

---

## 五、禁止事项（⚠️ 违反即停止并上报）

| # | 禁止 | 原因 |
|---|---|---|
| 1 | **禁止 `git rm`（不带 `--cached`）** | 会删掉工作区文件 → qrc 编译失败 |
| 2 | **禁止把 `add` 和 `rm --cached` 放同一条命令** | 触发 rename 检测 → 误删工作区文件（分支 1 实际踩过） |
| 3 | **禁止删除 `Client/connect.config` 本地文件** | 它被 `config.qrc` 引用，删了编译不过 |
| 4 | **禁止删除或修改 `Client/config.qrc`** | 超出范围 |
| 5 | **禁止在 `.template` 里写真实凭据** | 模板要能公开 |
| 6 | **禁止顺手改其他缺陷** | 一次一件事 |
| 7 | **禁止 `git push`** | 不可逆操作 |
| 8 | **禁止用 `rm -f` 清理备份文件**（如需清理） | 走回收站，或保留在 `build/` 内 |

---

## 六、执行流程（Development OS v1.7）

```bash
# ① 开场
powershell -File pipeline/state.ps1

# ② 切分支
git switch master
git switch -c chore/repo-sanitize

# ③ 按 §3.1 分步执行（每步后检查！）

# ④ 跑门
powershell -File pipeline/build.ps1
powershell -File pipeline/test.ps1
powershell -File pipeline/audit.ps1

# ⑤ 收尾检查（§7 五项全过）

# ⑥ 提交
git add .gitignore Client/connect.config.template
git commit -m "chore(repo): untrack Client/connect.config, add template and ignore rules"

# ⑦ 合并
git switch master
git merge --no-ff chore/repo-sanitize -m "merge: REQ-REPO-004 repo sanitize (SMALL)"
git branch -d chore/repo-sanitize

# ⑧ 停下等批准
```

---

## 七、需要的验证证据（⚠️ 五项缺一不可）

```bash
# 1. 本地文件仍在
ls -l Client/connect.config                 # 必须存在

# 2. 不再被跟踪
git ls-files --error-unmatch Client/connect.config   # 应报错 "did not match any file"（这就是成功）

# 3. 忽略规则生效
git check-ignore -v Client/connect.config   # 应输出命中的 .gitignore 规则行

# 4. 工作区干净
git status --short                          # 应无输出（或只有无关的新文档）

# 5. 编译通过
powershell -File pipeline/build.ps1         # GREEN
```

> ⚠️ **不要用 `git ls-files | grep "connect.config$"` 判断。** 实测中 `$` 锚点匹配行为不可靠，会把 `connect.config.template` 也算进去造成误判。**用 `git ls-files --error-unmatch` 判断**（不匹配时返回非 0，这才是明确的判据）。

### 证据目录

```
build/evidence/chore-repo-sanitize/<commit-hash>/
    pipeline-run.log
    manual-verify.log     # 上面 5 条的实际输出
    manifest.txt
```

```bash
python "C:/Users/王晨扬/.codex/skills/development-os-v1-7/scripts/record-evidence.py" \
  --evidence-dir build/evidence/chore-repo-sanitize/<hash> \
  --repo E:/qt/xiangmu \
  --label "REQ-REPO-004: Client/connect.config untracked, template + ignore rules added" \
  .gitignore Client/connect.config.template
```

---

## 八、任务记录与 handoff

### 8.1 `project-docs/tasks/REQ-REPO-004/task.md`

含：档位 + 依据、验收标准、allowed paths、决策日志。**必须记录「分步执行」的每一步实际结果**（尤其是步骤 5 的检查结果）。

### 8.2 handoff

`project-docs/handoffs/chore-repo-sanitize-<日期>.txt`，**严格 10 行**：

```text
# handoff: REQ-REPO-004
branch: master
head: <合并后 commit 完整哈希>
tier: SMALL
node: merged
audit: none
next: powershell -File pipeline/state.ps1
blockers: none
evidence: project-docs/evidence/REQ-REPO-004/
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
| **步骤 5 检查发现 `Client/connect.config` 不见了** | **立即停止**，用 `cp Client/connect.config.bak-<日期> Client/connect.config` 恢复，然后**报告**。这重现了分支 1 的坑，说明命令还是被合并执行了 |
| 编译报找不到 `connect.config` | 同上，先恢复本地文件 |
| 发现 `Client/` 下还有其他不该跟踪的文件 | **先报告**，列出清单。可能另开需求，不要自行清理 |
| 想一并清理 `release/` 目录 | **不做**。它已被正确忽略（实测 `git status --ignored` 显示 `!!`），不是缺陷 |
| 想清理 `.git` 51MB 的历史体积 | **不做**。那是 REQ-REPO-006（历史清理）的事，且不可逆 |

---

## 十、完成后请回报这些

1. 最终 commit 哈希（分支 + 合并）
2. **§7 五项验证的实际输出**（逐条贴）
3. `.gitignore` 改动前后的 diff
4. 三项 pipeline 实际输出
5. handoff 校验脚本输出
6. 是否出现任何文件丢失（应为"无"）

**不要自行推送。**
