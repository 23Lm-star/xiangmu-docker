# REQ-CI-009　静态检查：把 Semgrep 接入流水线

| 项目 | 值 |
|---|---|
| **需求编号** | REQ-CI-009 |
| **分支名** | `chore/ci-static-check` |
| **任务等级** | **SMALL** |
| **基线 commit** | `b5d7bb3`（编写时 master HEAD） |
| **流程版本** | Development OS v1.7 |
| **状态** | 待执行 |

---

## 一、背景（为什么要做）

本次审查发现的 17 个缺陷里，**至少 4 个是模式匹配型的**——也就是静态检查能自动抓到的：

| 缺陷 | 类型 | 能否被静态规则抓到 |
|---|---|---|
| P0-3 密码写进日志（7 处 `qDebug`） | 敏感数据外泄模式 | ✅ 可以（"日志语句包含 password/pwd 变量"） |
| P0-1 凭据文件被跟踪 | 密钥入库模式 | ⚠️ 部分（文件名匹配） |
| P2 上传路径未校验 | 输入未验证模式 | ⚠️ 部分（较难，需数据流分析） |
| P3 调试后门（`zombie_sim.txt`） | 硬编码调试开关 | ✅ 可以（字符串字面量匹配） |

**现有流水线（build / test / audit）只检查"能编译、无冻结路径违规、有测试证据"，完全不检查代码模式。** 这类缺陷全靠人工审查——而人工审查会漏。

### 已核实的工具情况（实测）

| 工具 | 状态 |
|---|---|
| **Semgrep** | ✅ **已安装**：`C:/Users/王晨扬/.workbuddy/binaries/python/envs/default/Scripts/semgrep` |
| `cppcheck` | ❌ 未安装 |
| `clang-tidy` | ❌ 未安装 |
| `gitleaks` | ❌ 未安装 |

**Semgrep 已在本机可用**，无需新装工具——这是本需求选择 Semgrep 的首要理由。

---

## 二、目标（验收标准）

| # | 验收标准 | 怎么验证 |
|---|---|---|
| A1 | 存在项目自定义 Semgrep 规则集 | 规则文件存在且语法有效 |
| A2 | 规则能抓到"日志泄露凭据"模式 | 用已知的 7 处历史代码做验证（应报出） |
| A3 | 规则能抓到"硬编码调试后门"模式 | 同上 |
| A4 | 存在 `pipeline/staticcheck.ps1` 可独立运行 | 运行有输出 |
| A5 | 当前代码库在规则下**通过**（因为已知缺陷已在前面需求修掉） | 运行结果 PASS |
| A6 | **静态检查失败时返回非 0（RED）** | 负向验证 |
| A7 | **`audit.ps1` 不因此改变**（本需求不接进 audit） | 见 §3.4 |
| A8 | 三项 pipeline 仍全绿 | GREEN / GREEN / PASS |

> **注意 A7**：本需求**只新增**一个可独立运行的 `staticcheck.ps1`，**不修改 `audit.ps1`** 去调用它。接进 audit 是后续需求（因为改 audit 属改门，需独立批准 + 单独的设计讨论）。

---

## 三、技术方案（已定型）

### 3.1 规则文件位置

```
staticcheck/
    rules/
        xiangmu-security.yaml      ← 自定义规则
```

> 放仓库根的 `staticcheck/`（不是 `tests/`，因为它是检查规则不是测试；也不是 `pipeline/`，因为 `pipeline/` 放脚本）。

### 3.2 规则集内容（按这个写）

```yaml
# staticcheck/rules/xiangmu-security.yaml
rules:
  # ===== 1. 日志泄露凭据 =====
  - id: xiangmu.log-credential-leak
    languages: [cpp]
    severity: ERROR
    message: >
      日志语句输出了密码/凭据变量。凭据不得写入日志——
      日志会被采集、上报、留存，等于凭据二次泄露。
    patterns:
      - pattern-either:
          - pattern: qDebug() << ... << $X << ...
          - pattern: qInfo() << ... << $X << ...
      - metavariable-regex:
          metavariable: $X
          regex: .*(pwd|Pwd|PWD|password|Password|passwd|caData).*
    paths:
      include:
        - Server/
        - Client/
      exclude:
        - build*/
        - "*/release/*"
        - "*/debug/*"

  # ===== 2. 硬编码调试后门 =====
  - id: xiangmu.debug-backdoor-fileread
    languages: [cpp]
    severity: WARNING
    message: >
      疑似调试后门：代码在运行时读取某个固定文件来改变行为。
      调试钩子不应留在生产代码里。
    patterns:
      - pattern-either:
          - pattern: QFile $F("zombie_sim.txt")
          - pattern: QFile $F("..._sim.txt")
          - pattern: QFile $F("debug_...")
    paths:
      include:
        - Server/
        - Client/

  # ===== 3. 硬编码凭据 =====
  - id: xiangmu.hardcoded-db-password
    languages: [cpp]
    severity: ERROR
    message: >
      疑似硬编码数据库凭据。请从配置文件读取，不要写在源码里。
    patterns:
      - pattern-either:
          - pattern: $X = "123456"
          - pattern: $X = "root"
    paths:
      include:
        - Server/
        - Client/

  # ===== 4. 未校验的路径拼接（启发式） =====
  - id: xiangmu.unchecked-path-from-pdu
    languages: [cpp]
    severity: WARNING
    message: >
      直接使用客户端传来的路径（pdu->caMsg）而未做规范化校验。
      使用 MsgHandler::isPathInSandbox() 校验后再使用。
    patterns:
      - pattern: QString $P(QString::fromUtf8(pdu->caMsg))
      - pattern-not-inside: |
          if (...) { ... }
      # 注意：这是启发式规则，允许误报。人工复核。
    paths:
      include:
        - Server/
```

> ⚠️ **规则 4 是启发式的，会有误报。** 因 Semgrep 的 C++ 数据流能力有限，它只能匹配"字面模式"。**规则 4 设为 WARNING 而非 ERROR**，并且**允许它误报**——误报的代价是人工看一眼。
> **如果规则 4 误报太多导致流水线噪音大，可以把它降为 `INFO` 或暂时移除**，在 task.md 记录原因。

### 3.3 `pipeline/staticcheck.ps1`

```powershell
# pipeline/staticcheck.ps1 -- project-specific static analysis (Semgrep).
# Independent gate. NOT wired into audit.ps1 (that requires separate approval).
$ErrorActionPreference = "Continue"
$repo = (git rev-parse --show-toplevel)
$hash = git rev-parse --short HEAD
$evDir = Join-Path $repo "build\evidence\current"
New-Item -ItemType Directory -Force -Path $evDir | Out-Null
$log = Join-Path $evDir "staticcheck-$hash-$(Get-Date -Format yyyyMMdd-HHmmss).log"

$SEMGREP = "C:\Users\王晨扬\.workbuddy\binaries\python\envs\default\Scripts\semgrep.exe"
if (-not (Test-Path $SEMGREP)) {
    Write-Output "staticcheck SKIP: semgrep not found at $SEMGREP"
    "semgrep not found" | Out-File $log -Encoding utf8
    exit 0
}

$rules = Join-Path $repo "staticcheck\rules\xiangmu-security.yaml"
if (-not (Test-Path $rules)) {
    Write-Output "staticcheck RED: rules file missing: $rules"
    exit 1
}

Push-Location $repo
& $SEMGREP scan `
    --config $rules `
    --error `
    --no-git-ignore `
    --exclude build `
    --exclude "*/release" `
    --exclude "*/debug" `
    Server Client *>> $log
$rc = $LASTEXITCODE
Pop-Location

Write-Output "staticcheck log -> $log"
if ($rc -ne 0) { Write-Output "staticcheck RED (rc=$rc)"; exit 1 }
Write-Output "staticcheck PASS ($hash)"
exit 0
```

> `--error` 让 Semgrep 在有 ERROR 级发现时返回非 0。WARNING 级默认不影响退出码——**正好符合"规则 4 允许误报但不阻塞"**的设计。
> `--no-git-ignore` 是必要的，否则 Semgrep 会跳过被 `.gitignore` 的路径（而 `release/` 恰好被忽略）——不过我们本来也要排除它们，所以两者配合。

### 3.4 ⚠️ 为什么不接进 `audit.ps1`

`audit.ps1` 现在是"三件事：冻结路径、仓库卫生、证据对账"的**单一页面判定**。接进静态检查会带来：

1. **噪音风险**：规则 4 是启发式的，一旦误报，`audit.ps1` 会 RED → 阻塞所有正常开发
2. **门槛变化**：`audit.ps1` 的判定标准会变得难以预测
3. **改门需独立批准**：按制度，改 `audit.ps1` 要单独走一次 SMALL 需求

**所以本需求先让它独立存在**。等规则经过一段时间实战（误报率可接受）后，**再开一个独立需求**把它接进 `audit.ps1`。

### 3.5 首次运行的期望结果

跑一遍现有代码。**由于前面需求已经修掉了凭据日志、调试后门**，期望结果：

- 规则 1（日志凭据）：**应无发现**（AUTH-002 已清理）
- 规则 2（调试后门）：**应报出至少 1 处**（`Client/client.cpp:116-140` 的 `zombie_sim.txt`）→ 若报出，说明规则有效；这处是否清理**是另一个需求**
- 规则 3（硬编码凭据）：**可能有发现**（`init_test_data.sql` 的测试数据不在扫描范围；但代码里若残留 `"123456"` 会被报出）
- 规则 4（未校验路径）：**可能有若干误报**

> ⚠️ **A5 说"当前代码库通过"。** 如果规则 2 报出 `zombie_sim.txt`，那 A5 就**不成立**。
> **处理方式**：把 `zombie_sim.txt` 的处理**作为本需求的一部分一并做掉**（它就是个调试后门，删掉即可），或**在规则里 `nosemgrep` 豁免并记录**。
> **推荐：一并删掉**（最小、最安全）。见 §4 允许文件的说明。

---

## 四、允许改动的文件（allowed paths）

```
staticcheck/rules/xiangmu-security.yaml      ← 新建
pipeline/staticcheck.ps1                     ← 新建
Client/client.cpp                            ← 仅删除调试后门（zombie_sim.txt 读取块）
project-docs/tasks/REQ-CI-009/task.md
project-docs/evidence/REQ-CI-009/
```

> ⚠️ `Client/client.cpp` 的改动**仅限**删除心跳调试后门（约 116-140 行）。这是让 A5 成立的必要条件。
> **如果发现还有其他调试后门**，逐个报告，不要批量删。

---

## 五、禁止事项（⚠️ 违反即停止并上报）

| # | 禁止 | 原因 |
|---|---|---|
| 1 | **禁止修改 `Server/protocol.h` / `Client/protocol.h`** | frozen_path |
| 2 | **禁止修改 `pipeline/audit.ps1`** | 接进 audit 是后续需求，需独立批准 |
| 3 | **禁止修改 `pipeline/build.ps1` / `test.ps1` / `state.ps1`** | 超出范围 |
| 4 | **禁止新装工具**（cppcheck/clang-tidy/gitleaks） | 用已装的 Semgrep |
| 5 | **禁止把规则 4 设为 ERROR** | 启发式规则会误报，不得阻塞 |
| 6 | **禁止为了让检查通过而删除整个规则** | 应调整或豁免，并记录理由 |
| 7 | **禁止批量删调试代码** | 只处理规则报出的、逐个确认 |
| 8 | **禁止修改扫描范围以外的源码** | 超范围 |
| 9 | **禁止顺手改其他缺陷** | 一次一件事 |
| 10 | **禁止 `git push`** | 不可逆 |

---

## 六、执行流程（Development OS v1.7）

```bash
# ① 开场
powershell -File pipeline/state.ps1

# ② 切分支
git switch master
git switch -c chore/ci-static-check

# ③ 写规则 + 脚本（§3.1-3.3）
# ④ 先手动跑一次 semgrep，看实际报出什么
"C:/Users/王晨扬/.workbuddy/binaries/python/envs/default/Scripts/semgrep.exe" scan \
   --config staticcheck/rules/xiangmu-security.yaml Server Client

# ⑤ 根据结果决定：
#    - 若报出调试后门 → 删掉
#    - 若规则误报太多 → 调整规则或降级
#    - 目标：最终 staticcheck.ps1 报 PASS

# ⑥ 跑门
powershell -File pipeline/build.ps1
powershell -File pipeline/test.ps1
powershell -File pipeline/audit.ps1
powershell -File pipeline/staticcheck.ps1

# ⑦ 负向验证（§7）

# ⑧ 提交
git add staticcheck/ pipeline/staticcheck.ps1 Client/client.cpp
git commit -m "chore(ci): add project-specific semgrep rules and staticcheck gate"

# ⑨ 合并
git switch master
git merge --no-ff chore/ci-static-check -m "merge: REQ-CI-009 static analysis gate (SMALL)"
git branch -d chore/ci-static-check

# ⑩ 停下等批准
```

> ⚠️ 注意 `git add` 里有 `Client/client.cpp`。**如果第 ⑤ 步没有删调试后门，就不要 add 它。**

---

## 七、需要的验证证据

| 验证 | 怎么验证 | 期望 |
|---|---|---|
| 规则语法有效 | `semgrep scan --config <rules> --dryrun` 或直接跑 | 无语法错误 |
| 规则**真的能抓到**（规则 1） | **临时**在某处加一行 `qDebug() << "pwd_hex=" << pwd;` | 报出 `xiangmu.log-credential-leak` |
| 规则**真的能抓到**（规则 2） | 若已删后门，用临时文件验证 | 报出 |
| 当前代码库 PASS | `staticcheck.ps1` | `staticcheck PASS` |
| **负向：有发现 → RED** | 见上面"临时加一行" | `staticcheck RED` + 退出码非 0 |
| **还原** | 撤销临时行 | 恢复 PASS |
| **`audit.ps1` 未受影响** | 跑 `audit.ps1` | 与改动前行为一致 |
| Semgrep 不存在时优雅跳过 | 临时改 `$SEMGREP` 路径为不存在的 | 输出 `SKIP`，退出码 0 |

> ⚠️ **负向验证是必须的。** 一个从不出错的规则等于没有规则。

### 证据目录

```
build/evidence/chore-ci-static-check/<commit-hash>/
    pipeline-run.log
    semgrep-scan.log          # 首次扫描的完整输出
    negative-verify.log       # 临时注入 + 报出 + 还原
    manifest.txt
```

```bash
python "C:/Users/王晨扬/.codex/skills/development-os-v1-7/scripts/record-evidence.py" \
  --evidence-dir build/evidence/chore-ci-static-check/<hash> \
  --repo E:/qt/xiangmu \
  --label "REQ-CI-009: semgrep rules + standalone staticcheck gate" \
  staticcheck/rules/xiangmu-security.yaml pipeline/staticcheck.ps1
```

---

## 八、任务记录与 handoff

### 8.1 `project-docs/tasks/REQ-CI-009/task.md`

含：档位 + 依据、验收标准、allowed paths、决策日志。
**必须记录**：
- 每条规则的**实测命中情况**（报了哪些、误报哪些）
- 规则 4 的噪音评估（误报率是否可接受）
- 调试后门的处理（删了 / 豁免了）
- **不接进 `audit.ps1` 的理由**（§3.4）
- 未处理的其他后门（若有）

### 8.2 handoff

`project-docs/handoffs/chore-ci-static-check-<日期>.txt`，**严格 10 行**：

```text
# handoff: REQ-CI-009
branch: master
head: <合并后 commit 完整哈希>
tier: SMALL
node: merged
audit: none
next: powershell -File pipeline/staticcheck.ps1
blockers: none
evidence: project-docs/evidence/REQ-CI-009/
owner: claude-code
date: <YYYY-MM-DD>
```

> `next` 一行改成 `staticcheck.ps1`，因为这是本需求新增的门，下个会话应优先知道它存在。

```bash
python "C:/Users/王晨扬/.codex/skills/development-os-v1-7/scripts/validate-handoff.py" <handoff 路径>
```

---

## 九、遇到的边界情况怎么办

| 情况 | 怎么办 |
|---|---|
| Semgrep 报大量误报 | 逐条评估。**优先收窄规则**（加 `pattern-not-inside`），而不是删规则 |
| 规则 4 几乎全是误报 | **把它移到单独文件**或降级为 `INFO`，在 task.md 记录"启发式规则暂不可用于自动判定" |
| 发现除 `zombie_sim.txt` 外的后门 | **逐个报告**，本需求只处理规则实际报出的 |
| Semgrep 版本不兼容规则语法 | 先 `semgrep --version` 确认版本，再调整语法 |
| 想顺便接进 `audit.ps1` | **不做**。另开需求，需独立批准 |
| `--no-git-ignore` 导致扫描到 build/ | 用 `--exclude build`（脚本里已有） |
| C++ 规则不生效（Semgrep 对 C++ 支持有限） | 确认 `languages: [cpp]`；若确实不支持某模式，**记录限制**，不要硬凑 |
| pipeline 报 `FROZEN-PATH HIT` | 立即停止报告 |

---

## 十、完成后请回报这些

1. 最终 commit 哈希（分支 + 合并）
2. 规则文件全文
3. `staticcheck.ps1` 全文
4. **首次扫描的完整输出**（报了什么、误报什么）
5. **负向验证输出**（临时注入 → 报出 → 还原）
6. 调试后门的处理方式
7. `audit.ps1` 行为未变的证据
8. 三项 pipeline + staticcheck 的实际输出
9. handoff 校验脚本输出
10. **规则的已知局限**（诚实列出）

**不要自行推送。**
