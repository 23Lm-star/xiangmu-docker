# REQ-TEST-008B　把测试登记进流水线

| 项目 | 值 |
|---|---|
| **需求编号** | REQ-TEST-008B |
| **分支名** | `chore/register-tests-in-pipeline` |
| **任务等级** | **SMALL** |
| **基线 commit** | `b5d7bb3`（编写时 master HEAD） |
| **流程版本** | Development OS v1.7 |
| **状态** | 待执行（**依赖 REQ-TEST-008A 先完成**） |

---

## 一、背景（为什么要做）

`pipeline/test.ps1` 目前的 `$TESTS = @()` 是空的，脚本第 14 行老实报告"0 automated tests registered"。

REQ-TEST-008A 产出了 `tests/uploadsession/` 测试套件，但**它还没接进流水线**——`test.ps1` 不会自动构建和运行它。不接进去，测试就是"只有作者会跑"的一次性脚本。

### 为什么拆成独立需求（8B）

`pipeline.md` 规则 7 明确要求：**对流水线脚本（`pipeline/*.ps1`）的改动必须作为独立的 SMALL 任务**。

理由（这是制度设计，不是官僚）：

1. **流水线是"门"本身**。改门的逻辑必须能被单独审查。混在功能改动里改门 = 门失去了把关能力。
2. **`test.ps1` 是 `audit.ps1` 证据链的一环**。它决定"是否存在测试证据"。改它影响整条信任链。
3. **可回滚性**。门如果有问题，要能单独 revert，不牵连业务代码。

### 已核实的 `test.ps1` 现状（实测）

```powershell
$TESTS = @()                                        # ← 空数组，第 11 行

if ($TESTS.Count -eq 0) {
    Write-Output "test: 0 automated tests registered (fill TESTS in pipeline/test.ps1)"
    "no automated tests registered (hash $hash)" | Out-File $log -Encoding utf8
    exit 0                                          # ← 0 个测试也算通过
}

$fail = 0
foreach ($t in $TESTS) {
    if (-not (Test-Path $t)) { Write-Output "MISSING test exe: $t"; $fail++; continue }
    & $t *>> $log
    if ($LASTEXITCODE -ne 0) { Write-Output "FAIL: $t"; $fail++ } else { Write-Output "PASS: $t" }
}
Write-Output "test log -> $log"
if ($fail -gt 0) { Write-Output "test RED ($fail failed)"; exit 1 }
Write-Output "test GREEN"
exit 0
```

**关键观察**：`test.ps1` 已经支持 `$TESTS` 数组，**逻辑是现成的**。本需求要做的其实是两件事：

1. **把 exe 路径填进 `$TESTS`**
2. **确保 exe 存在**——`test.ps1` 只检查 `Test-Path`，**不负责构建**。所以要么先构建好，要么在 `test.ps1` 里补构建步骤

> **这就是本需求的真实难点**：`test.ps1` 的设计假设是"exe 已被构建"。而 `build.ps1` 目前不构建测试。所以需要决定：**谁负责构建测试 exe？**

---

## 二、目标（验收标准）

| # | 验收标准 | 怎么验证 |
|---|---|---|
| A1 | `pipeline/test.ps1` 的 `$TESTS` 指向真实的测试 exe | 文件内容检查 |
| A2 | 运行 `test.ps1` 时，测试**真的被执行**（不再报 0 tests） | 输出应有 `PASS: ...` |
| A3 | 测试失败时 `test.ps1` 返回非 0（RED） | 故意破坏一处 → 确认 RED |
| A4 | 测试通过时 `test.ps1` 返回 0（GREEN） | 正常运行 |
| A5 | **exe 不存在时有明确处理**（不能让流水线假装通过） | 见 §3.3 |
| A6 | `test.ps1` 会生成 `build/evidence/current/test-<hash>-*.log`（`audit.ps1` 依赖它） | 文件存在 |
| A7 | `audit.ps1` 报 PASS | 运行 |
| A8 | 三项 pipeline 全绿 | GREEN / GREEN / PASS |

---

## 三、技术方案（已定型）

### 3.1 决策点：谁构建测试 exe？

**三个选项，必须选一个并在 task.md 写明理由：**

| 选项 | 做法 | 优点 | 缺点 |
|---|---|---|---|
| **A（推荐）** | 在 `test.ps1` 里**先构建测试再运行** | 一步到位，不会忘记构建 | `test.ps1` 变重（承担构建职责） |
| B | 在 `build.ps1` 里构建测试 | 职责清晰（build 负责建） | `build.ps1` 变慢，且改 `build.ps1` 也属"改门" |
| C | 保持 `test.ps1` 只运行，要求先手动构建 | 改动最小 | 容易忘记构建 → `Test-Path` 失败 |

> **推荐 A。** 理由：`test.ps1` 的语义是"跑测试"，跑测试隐含"得有测试可跑"。让 `test.ps1` 自己保证 exe 存在，**消除了"忘记构建"这个失败模式**。
> 如果选 A，`test.ps1` 需要先调用 qmake + make（复制 `build.ps1` 的工具链路径设置）。

### 3.2 `$TESTS` 的填法

**不要硬编码绝对路径**。用相对仓库根：

```powershell
# 测试可执行文件（相对仓库根）
$TESTS = @(
    "build\build-tst-uploadsession\release\tst_uploadsession.exe"
)
```

> 注意 Qt 的 `release/` 子目录（`CONFIG += console` 下，mingw32-make 默认 `release` 目标）。
> ⚠️ **先实测确认 exe 的真实落点**。可能是 `release\`、`debug\`、或直接 `build\build-tst-uploadsession\`（取决于 `CONFIG += debug_and_release`）。
> **做法**：先按 §3.5 手动构建一次，`find build/build-tst-uploadsession -name "*.exe"` 看到实际路径，再填进 `$TESTS`。

### 3.3 若选 A：`test.ps1` 的改动

在 `test.ps1` 的 `$TESTS = @(...)` 之前插入构建段：

```powershell
# --- build test targets before running (they are not covered by build.ps1) ---
$QTBIN  = "C:\Qt\5.14.2\mingw73_64\bin"
$MINGWBIN = "C:\Qt\Tools\mingw730_64\bin"
$env:Path = "$QTBIN;$MINGWBIN;$env:Path"

$TEST_PROJECTS = @("tests\uploadsession\tst_uploadsession.pro")
foreach ($pro in $TEST_PROJECTS) {
    $name = [IO.Path]::GetFileNameWithoutExtension($pro)
    $src  = Join-Path $repo $pro
    $bld  = Join-Path $repo "build\build-$name"
    New-Item -ItemType Directory -Force -Path $bld | Out-Null
    Push-Location $bld
    cmd /c "`"$QTBIN\qmake.exe`" `"$src`" 2>&1" | Out-Null
    if ($LASTEXITCODE -ne 0) { Pop-Location; Write-Output "test RED (qmake $name)"; exit 1 }
    cmd /c "`"$MINGWBIN\mingw32-make.exe`" -j4 2>&1" | Out-Null
    $rc = $LASTEXITCODE
    Pop-Location
    if ($rc -ne 0) { Write-Output "test RED (make $name, rc=$rc)"; exit 1 }
    Write-Output "test build OK: $name"
}
```

> **`$env:Path` 设置与 `build.ps1` 保持一致**（`build.ps1` 第 9-11 行）。**不要用 Anaconda 的 qmake**。
> 上面 `| Out-Null` 是为了不让构建输出污染测试日志；若要保留，改成 `*>> $log`。

### 3.4 ⚠️ 不要让流水线"假装通过"（A5）

现状有个**隐患**：`$TESTS.Count -eq 0` 时 `exit 0`（算通过）。一旦 `$TESTS` 填了，这个分支不再触发，但**如果 exe 路径写错了**，会走 `MISSING test exe` → `$fail++` → RED。这是对的。

**但要额外注意**：**不要**为了让流水线变绿而：
- 把 `$TESTS` 留空（又变回"0 tests 通过"）
- 用 `if (-not (Test-Path $t)) { continue }` 静默跳过

**exe 缺失必须是 RED。** 现有的 `MISSING test exe ... $fail++` 逻辑**保留，不要弱化**。

> **同时建议**（可选）：把 `$TESTS.Count -eq 0` 的 `exit 0` 改成**输出醒目提示**，避免未来有人误以为有测试覆盖。
> **⚠️ 但如果改这个，会影响 `audit.ps1` 的证据判定**（它只看"有没有 test 日志"，不看内容）。**本需求不动这部分逻辑**——保持 `exit 0`，只在 `task.md` 里记一笔"0 测试时流水线仍算通过，这是 bootstrap 遗留，待有测试后自然失效"。

### 3.5 构建与验证

```bash
# 1. 手动构建一次，确认 exe 实际落点
export PATH="/c/Qt/5.14.2/mingw73_64/bin:/c/Qt/Tools/mingw730_64/bin:$PATH"
mkdir -p build/build-tst-uploadsession && cd build/build-tst-uploadsession
qmake "E:/qt/xiangmu/tests/uploadsession/tst_uploadsession.pro"
mingw32-make -j4
find . -name "tst_uploadsession.exe"        # ← 用这个路径填 $TESTS
cd /e/qt/xiangmu

# 2. 编辑 pipeline/test.ps1

# 3. 跑
powershell -File pipeline/test.ps1
powershell -File pipeline/audit.ps1
```

---

## 四、允许改动的文件（allowed paths）

```
pipeline/test.ps1                            ← 主要改动
project-docs/tasks/REQ-TEST-008B/task.md
project-docs/evidence/REQ-TEST-008B/
```

> ⚠️ **`pipeline/build.ps1`、`audit.ps1`、`state.ps1` 不在允许清单内**（除非选选项 B，那需要先报告并获批准）。

---

## 五、禁止事项（⚠️ 违反即停止并上报）

| # | 禁止 | 原因 |
|---|---|---|
| 1 | **禁止修改 `Server/protocol.h` / `Client/protocol.h`** | frozen_path |
| 2 | **禁止修改 `pipeline/audit.ps1` / `state.ps1`** | 超出范围，改门需独立需求 |
| 3 | **禁止弱化 `MISSING test exe` 的失败逻辑** | A5：缺失必须 RED |
| 4 | **禁止把 `$TESTS` 留空来"通过"** | 等于自欺 |
| 5 | **禁止静默跳过缺失的 exe**（如 `continue`） | 同上 |
| 6 | **禁止用 Anaconda 的 qmake** | 必须用 `C:\Qt\5.14.2\mingw73_64\bin\qmake.exe` |
| 7 | **禁止修改 `tests/` 下的测试代码** | 那是 8A 的产物 |
| 8 | **禁止修改任何源码** | 本需求只碰流水线脚本 |
| 9 | **禁止顺手改其他缺陷** | 一次一件事 |
| 10 | **禁止 `git push`** | 不可逆 |

---

## 六、执行流程（Development OS v1.7）

```bash
# ① 开场（确认 REQ-TEST-008A 已合并）
powershell -File pipeline/state.ps1

# ② 切分支
git switch master
git switch -c chore/register-tests-in-pipeline

# ③ 手动构建一次确认 exe 路径（§3.5 第 1 步）

# ④ 改 pipeline/test.ps1

# ⑤ 跑门
powershell -File pipeline/build.ps1
powershell -File pipeline/test.ps1      # ← 应出现 "test build OK" + "PASS: ..." + "test GREEN"
powershell -File pipeline/audit.ps1     # ← 应 PASS

# ⑥ 负向验证（§7）

# ⑦ 提交
git add pipeline/test.ps1
git commit -m "chore(pipeline): register upload-session test in pipeline/test.ps1"

# ⑧ 合并
git switch master
git merge --no-ff chore/register-tests-in-pipeline -m "merge: REQ-TEST-008B register tests in pipeline (SMALL)"
git branch -d chore/register-tests-in-pipeline

# ⑨ 停下等批准
```

---

## 七、需要的验证证据

| 验证 | 怎么验证 | 期望 |
|---|---|---|
| 测试真被跑 | `test.ps1` 输出 | 有 `PASS: build\build-tst-uploadsession\release\tst_uploadsession.exe` |
| 不再报 0 tests | `test.ps1` 输出 | **无** "0 automated tests registered" |
| 日志生成 | `ls build/evidence/current/test-*.log` | 有新日志 |
| audit 依赖满足 | `audit.ps1` | PASS（不是 "NO test evidence"） |
| **负向：测试失败 → RED** | 临时改测试断言为必假（如 `QVERIFY(false)`），重跑 `test.ps1` | 输出 `FAIL: ...` + `test RED` + 退出码非 0 |
| **负向：构建失败 → RED** | 临时在测试 .cpp 里写语法错误 | 输出 `test RED (make ...)` |
| **还原** | 撤销临时改动 | `test.ps1` 恢复 GREEN |

> ⚠️ 负向验证的临时改动**必须完全还原**，用 `git status` 确认 `tests/` 无改动。

### 证据目录

```
build/evidence/chore-register-tests-in-pipeline/<commit-hash>/
    pipeline-run.log
    negative-verify.log
    manifest.txt
```

```bash
python "C:/Users/王晨扬/.codex/skills/development-os-v1-7/scripts/record-evidence.py" \
  --evidence-dir build/evidence/chore-register-tests-in-pipeline/<hash> \
  --repo E:/qt/xiangmu \
  --label "REQ-TEST-008B: upload-session test registered in pipeline" \
  pipeline/test.ps1
```

---

## 八、任务记录与 handoff

### 8.1 `project-docs/tasks/REQ-TEST-008B/task.md`

含：档位 + 依据、验收标准、allowed paths、决策日志。
**必须记录**：
- **构建职责的选择**（§3.1 的 A/B/C 选了哪个及理由）
- exe 的**实际落点路径**（实测得出）
- 负向验证过程
- "0 测试时仍 exit 0"这个 bootstrap 遗留的处理决定

### 8.2 handoff

`project-docs/handoffs/chore-register-tests-in-pipeline-<日期>.txt`，**严格 10 行**：

```text
# handoff: REQ-TEST-008B
branch: master
head: <合并后 commit 完整哈希>
tier: SMALL
node: merged
audit: pass
next: powershell -File pipeline/state.ps1
blockers: none
evidence: project-docs/evidence/REQ-TEST-008B/
owner: claude-code
date: <YYYY-MM-DD>
```

> 本需求 `audit: pass`（因为流水线现已真实跑测试）。

```bash
python "C:/Users/王晨扬/.codex/skills/development-os-v1-7/scripts/validate-handoff.py" <handoff 路径>
```

---

## 九、遇到的边界情况怎么办

| 情况 | 怎么办 |
|---|---|
| **REQ-TEST-008A 尚未合并** | **停止**。没有测试可登记 |
| 找不到 exe（`build/` 下结构不同） | `find build/build-tst-uploadsession -name "*.exe"` 实测确认，再填 `$TESTS` |
| exe 落在 `debug\` 而非 `release\` | 按实际路径填。或改测试 .pro 强制 `CONFIG += release`（那要回 8A，**不要在这里改**） |
| 选选项 B（在 `build.ps1` 构建测试） | **停下报告**，因为那要改 `build.ps1`（改门需独立批准） |
| `test.ps1` 变慢明显 | 可接受。测试只有 1 个，增量构建很快 |
| 想让 `audit.ps1` 校验"测试数 > 0" | **不做**。那是改 `audit.ps1`，另开需求 |
| 负向验证后忘了还原 | **立即** `git checkout -- tests/` + `git status` 确认 |
| pipeline 报 `FROZEN-PATH HIT` | 立即停止报告 |

---

## 十、完成后请回报这些

1. 最终 commit 哈希（分支 + 合并）
2. **`pipeline/test.ps1` 的完整 diff**
3. 构建职责的选择（A/B/C）及理由
4. exe 的实测落点路径
5. `test.ps1` 正常运行的完整输出（应含 `test build OK` + `PASS:` + `test GREEN`）
6. **负向验证两份输出**（测试失败 RED / 构建失败 RED）+ 还原确认
7. `audit.ps1` 输出
8. `build.ps1` 输出
9. handoff 校验脚本输出

**不要自行推送。**
