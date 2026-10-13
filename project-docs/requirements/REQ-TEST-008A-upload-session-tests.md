# REQ-TEST-008A　上传会话回归测试

| 项目 | 值 |
|---|---|
| **需求编号** | REQ-TEST-008A |
| **分支名** | `test/upload-session` |
| **任务等级** | **MEDIUM** |
| **基线 commit** | `b5d7bb3`（编写时 master HEAD） |
| **流程版本** | Development OS v1.7 |
| **状态** | 待执行（**依赖 REQ-FS-003 先完成**） |

---

## 一、背景（为什么要做）

实测：`pipeline/test.ps1` 的 `$TESTS = @()` **是空的**，脚本第 14 行老实报告：

```
test: 0 automated tests registered (fill TESTS in pipeline/test.ps1)
```

**这个项目目前没有任何自动化测试。** 每次改动都靠人工点界面验证——这正是 REQ-FS-003（路径校验）、REQ-AUTH-002（加盐）这些安全改动难以建立长期信心的原因。

### 为什么第一个测试选「上传会话」

| 理由 | 说明 |
|---|---|
| **纯逻辑、无 UI** | `ResumableUploadSession` 不依赖 `QWidget`，可以独立编译测试 |
| **纯逻辑、无数据库** | 不依赖 MySQL，测试环境零准备 |
| **有明确的不变量** | "offset 必须连续"、"len 不能越界"是硬规则，可断言 |
| **是安全隐患所在** | REQ-FS-003 改的就是它的调用方，有测试才能防止回归 |
| **已被 AUTH/FS 改动波及** | 上传是三个安全需求都碰到的路径 |

**这是投入产出比最高的第一个测试。**

### 被测类的真实接口（实测读取 `Server/resumableuploadsession.h`）

```cpp
class ResumableUploadSession
{
public:
    struct BeginResult { bool ok; qint64 offset; bool complete; };

    ResumableUploadSession();

    BeginResult begin(const QString &dirPath,
                      const QString &fileName,
                      qint64 totalSize,
                      bool resumeEnabled);

    bool   acceptWrittenChunk(qint64 offset, qint64 len);
    bool   isComplete() const;
    qint64 receivedSize() const;
    qint64 totalSize() const;
    QString stagingFilePath() const;
    void   reset();
};
```

### 已核实的实现约束（`resumableuploadsession.cpp`）

| 行为 | 实现事实（行号） |
|---|---|
| `dirPath` 或 `fileName` 空 → 失败 | 26 行 |
| `fileName` 含 `/` 或 `\` → 失败 | 26-27 行 |
| `totalSize < 0` → 失败 | 27 行 |
| `dirPath` 不存在的目录 → 失败 | 32-34 行 |
| `resumeEnabled && 已存在文件 && 0 < 大小 < totalSize` → `offset = 大小` | 39-42 行 |
| `resumeEnabled && 已存在文件 && 大小 == totalSize` → **`complete=true`** | 43-47 行 |
| `resumeEnabled && offset>0` → 以 **Append** 打开 | 52 行 |
| 否则 → **Truncate** | 52 行 |
| `acceptWrittenChunk`：`offset != m_receivedSize` 或 `len<=0` 或 `len > 剩余` → false | 69 行 |
| `isComplete()`：`m_totalSize>=0 && m_receivedSize>=m_totalSize` | 78 行 |

---

## 二、目标（验收标准）

| # | 验收标准 | 怎么验证 |
|---|---|---|
| A1 | 存在独立可执行的测试程序 | 编译出 `.exe` 并运行 |
| A2 | 测试**不依赖** UI、数据库、网络 | 测试进程独立跑通 |
| A3 | 覆盖 `begin()` 的全部拒绝分支（空路径、含分隔符文件名、负大小、目录不存在） | 至少 4 个断言 |
| A4 | 覆盖 resume 的三种情况（续传 / 已完成 / 不启用） | 至少 3 个断言 |
| A5 | 覆盖 `acceptWrittenChunk()` 的边界（正确 offset / 错误 offset / 负 len / 超长 len） | 至少 4 个断言 |
| A6 | 覆盖 `isComplete()` / `receivedSize()` / `totalSize()` | 至少 3 个断言 |
| A7 | 测试**真的会失败**（负向验证：故意破坏一处实现，测试必须红） | 见 §7 关键步骤 |
| A8 | 测试**不写真实沙箱目录**，用临时目录 | 测试产物在 `build/` 或系统临时目录 |
| A9 | 测试结果可被 `pipeline/test.ps1` 调用（本需求只产出 exe，不登记；登记是 8b） | exe 存在且返回码正确 |
| A10 | 三项 pipeline 全绿 | GREEN / GREEN / PASS |

> **A7 是本需求最重要的验收项。** 一个"从不失败"的测试等于没有测试。必须证明它抓得住回归。

---

## 三、技术方案（已定型）

### 3.1 用 Qt Test（`QTest`），不引入第三方框架

Qt 自带 `QtTest` 模块，**已在项目技术栈内**：

```
QT += testlib
```

`Server/Server.pro` 已 `QT += core gui network sql`。**测试程序用独立的 `.pro`**，不要混进主工程。

### 3.2 目录结构

```
tests/
    uploadsession/
        tst_uploadsession.pro      ← 独立 qmake 工程
        tst_uploadsession.cpp      ← 测试代码
```

> **不要让测试 .pro 被主工程引用。** 它是独立构建目标。
> ⚠️ `tests/` 是**源码**目录，**不是** build 产物目录。构建产物仍要落到 `build/` 下。需要在 `build.ps1` 之外单独构建（见 §3.5）。

### 3.3 测试骨架（按这个写）

```cpp
// tests/uploadsession/tst_uploadsession.cpp
#include <QtTest>
#include <QTemporaryDir>
#include "resumableuploadsession.h"

class TestUploadSession : public QObject
{
    Q_OBJECT

private slots:
    void init();          // 每个用例前准备临时目录
    void cleanup();       // 每个用例后清理

    // begin() 拒绝分支
    void beginRejectsEmptyDir();
    void beginRejectsEmptyFileName();
    void beginRejectsSlashInFileName();
    void beginRejectsBackslashInFileName();
    void beginRejectsNegativeSize();
    void beginRejectsMissingDir();

    // begin() 正常 + resume
    void beginFreshTruncates();
    void beginResumeUsesExistingSize();
    void beginResumeCompleteWhenSameSize();
    void beginWithoutResumeIgnoresExisting();

    // acceptWrittenChunk 边界
    void chunkAcceptsSequential();
    void chunkRejectsWrongOffset();
    void chunkRejectsZeroLen();
    void chunkRejectsNegativeLen();
    void chunkRejectsOverrun();

    // 状态查询
    void isCompleteAfterFullReceipt();
    void receivedSizeTracksChunks();
    void resetClearsState();

private:
    QTemporaryDir *m_tmp = nullptr;
    QString m_dir;
};

void TestUploadSession::init()
{
    m_tmp = new QTemporaryDir();
    QVERIFY(m_tmp->isValid());
    m_dir = m_tmp->path();          // 临时目录，测完自动删
}

void TestUploadSession::cleanup()
{
    delete m_tmp;
    m_tmp = nullptr;
}

// ---------- begin() 拒绝分支 ----------

void TestUploadSession::beginRejectsEmptyDir()
{
    ResumableUploadSession s;
    auto r = s.begin(QString(), "a.bin", 100, false);
    QVERIFY(!r.ok);
}

void TestUploadSession::beginRejectsEmptyFileName()
{
    ResumableUploadSession s;
    auto r = s.begin(m_dir, QString(), 100, false);
    QVERIFY(!r.ok);
}

void TestUploadSession::beginRejectsSlashInFileName()
{
    ResumableUploadSession s;
    auto r = s.begin(m_dir, "sub/a.bin", 100, false);
    QVERIFY(!r.ok);
}

void TestUploadSession::beginRejectsBackslashInFileName()
{
    ResumableUploadSession s;
    auto r = s.begin(m_dir, "sub\\a.bin", 100, false);
    QVERIFY(!r.ok);
}

void TestUploadSession::beginRejectsNegativeSize()
{
    ResumableUploadSession s;
    auto r = s.begin(m_dir, "a.bin", -1, false);
    QVERIFY(!r.ok);
}

void TestUploadSession::beginRejectsMissingDir()
{
    ResumableUploadSession s;
    auto r = s.begin(m_dir + "/nope/nope", "a.bin", 100, false);
    QVERIFY(!r.ok);
}

// ---------- begin() 正常 + resume ----------

void TestUploadSession::beginFreshTruncates()
{
    ResumableUploadSession s;
    auto r = s.begin(m_dir, "a.bin", 100, false);
    QVERIFY(r.ok);
    QCOMPARE(r.offset, qint64(0));
    QVERIFY(!r.complete);
    QCOMPARE(s.totalSize(), qint64(100));
    QCOMPARE(s.receivedSize(), qint64(0));
    QVERIFY(!s.stagingFilePath().isEmpty());
}

void TestUploadSession::beginResumeUsesExistingSize()
{
    ResumableUploadSession s;
    // 先造一个 40 字节的已有文件
    {
        auto r0 = s.begin(m_dir, "b.bin", 100, false);
        QVERIFY(r0.ok);
        QFile f(s.stagingFilePath());
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(40, 'x'));
        f.close();
    }
    ResumableUploadSession s2;
    auto r = s2.begin(m_dir, "b.bin", 100, true);
    QVERIFY(r.ok);
    QCOMPARE(r.offset, qint64(40));       // 续传起点
    QVERIFY(!r.complete);
    QCOMPARE(s2.receivedSize(), qint64(40));
}

void TestUploadSession::beginResumeCompleteWhenSameSize()
{
    ResumableUploadSession s;
    {
        auto r0 = s.begin(m_dir, "c.bin", 100, false);
        QVERIFY(r0.ok);
        QFile f(s.stagingFilePath());
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(100, 'y'));
        f.close();
    }
    ResumableUploadSession s2;
    auto r = s2.begin(m_dir, "c.bin", 100, true);
    QVERIFY(r.ok);
    QVERIFY(r.complete);                  // 已完整
    QCOMPARE(r.offset, qint64(100));
}

void TestUploadSession::beginWithoutResumeIgnoresExisting()
{
    ResumableUploadSession s;
    {
        auto r0 = s.begin(m_dir, "d.bin", 100, false);
        QVERIFY(r0.ok);
        QFile f(s.stagingFilePath());
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(40, 'z'));
        f.close();
    }
    ResumableUploadSession s2;
    auto r = s2.begin(m_dir, "d.bin", 100, false);   // 不启用续传
    QVERIFY(r.ok);
    QCOMPARE(r.offset, qint64(0));        // 从 0 开始（Truncate）
    QCOMPARE(s2.receivedSize(), qint64(0));
}

// ---------- acceptWrittenChunk 边界 ----------

void TestUploadSession::chunkAcceptsSequential()
{
    ResumableUploadSession s;
    QVERIFY(s.begin(m_dir, "e.bin", 100, false).ok);
    QVERIFY(s.acceptWrittenChunk(0, 30));
    QVERIFY(s.acceptWrittenChunk(30, 30));
    QCOMPARE(s.receivedSize(), qint64(60));
}

void TestUploadSession::chunkRejectsWrongOffset()
{
    ResumableUploadSession s;
    QVERIFY(s.begin(m_dir, "f.bin", 100, false).ok);
    QVERIFY(!s.acceptWrittenChunk(10, 10));    // offset 不是 0
    QCOMPARE(s.receivedSize(), qint64(0));     // 状态未变
}

void TestUploadSession::chunkRejectsZeroLen()
{
    ResumableUploadSession s;
    QVERIFY(s.begin(m_dir, "g.bin", 100, false).ok);
    QVERIFY(!s.acceptWrittenChunk(0, 0));
}

void TestUploadSession::chunkRejectsNegativeLen()
{
    ResumableUploadSession s;
    QVERIFY(s.begin(m_dir, "h.bin", 100, false).ok);
    QVERIFY(!s.acceptWrittenChunk(0, -5));
}

void TestUploadSession::chunkRejectsOverrun()
{
    ResumableUploadSession s;
    QVERIFY(s.begin(m_dir, "i.bin", 100, false).ok);
    QVERIFY(s.acceptWrittenChunk(0, 80));
    QVERIFY(!s.acceptWrittenChunk(80, 30));    // 80+30 > 100
    QCOMPARE(s.receivedSize(), qint64(80));
}

// ---------- 状态查询 ----------

void TestUploadSession::isCompleteAfterFullReceipt()
{
    ResumableUploadSession s;
    QVERIFY(s.begin(m_dir, "j.bin", 100, false).ok);
    QVERIFY(!s.isComplete());
    QVERIFY(s.acceptWrittenChunk(0, 100));
    QVERIFY(s.isComplete());
}

void TestUploadSession::receivedSizeTracksChunks()
{
    ResumableUploadSession s;
    QVERIFY(s.begin(m_dir, "k.bin", 1000, false).ok);
    QCOMPARE(s.receivedSize(), qint64(0));
    QVERIFY(s.acceptWrittenChunk(0, 100));
    QCOMPARE(s.receivedSize(), qint64(100));
    QVERIFY(s.acceptWrittenChunk(100, 400));
    QCOMPARE(s.receivedSize(), qint64(500));
}

void TestUploadSession::resetClearsState()
{
    ResumableUploadSession s;
    QVERIFY(s.begin(m_dir, "l.bin", 100, false).ok);
    QVERIFY(s.acceptWrittenChunk(0, 50));
    s.reset();
    QCOMPARE(s.receivedSize(), qint64(0));
    QCOMPARE(s.totalSize(), qint64(0));
    QVERIFY(s.stagingFilePath().isEmpty());
}

QTEST_APPLESS_MAIN(TestUploadSession)
#include "tst_uploadsession.moc"
```

> 用 `QTEST_APPLESS_MAIN`（**不是** `QTEST_MAIN`），因为它不需要 `QApplication`，只需要 `QCoreApplication`。被测类是纯逻辑类。

### 3.4 `tests/uploadsession/tst_uploadsession.pro`

```qmake
QT       += testlib
QT       -= gui

CONFIG   += console c++11
CONFIG   -= app_bundle

TARGET   = tst_uploadsession
TEMPLATE = app

INCLUDEPATH += $$PWD/../../Server

SOURCES += \
    tst_uploadsession.cpp \
    $$PWD/../../Server/resumableuploadsession.cpp

HEADERS += \
    $$PWD/../../Server/resumableuploadsession.h
```

> ⚠️ **直接引用 `Server/resumableuploadsession.cpp` 而不改它**。不要为了测试去改被测源码（那是越界）。
> ⚠️ `INCLUDEPATH` 指向 `Server/`，因为被测 .h 在那儿。

### 3.5 构建测试（影子构建）

**不要**把构建产物放进 `tests/`。手动构建：

```bash
export PATH="/c/Qt/5.14.2/mingw73_64/bin:/c/Qt/Tools/mingw730_64/bin:$PATH"
mkdir -p build/build-tst-uploadsession
cd build/build-tst-uploadsession
qmake "E:/qt/xiangmu/tests/uploadsession/tst_uploadsession.pro"
mingw32-make -j4
./release/tst_uploadsession.exe
echo "exit=$?"
```

> **本需求不修改 `pipeline/build.ps1` 去构建测试。** 那属于 REQ-TEST-008B（登记进流水线）。
> 本需求只要求"测试能构建、能跑通"，产物在 `build/` 下。

---

## 四、允许改动的文件（allowed paths）

```
tests/uploadsession/tst_uploadsession.pro     ← 新建
tests/uploadsession/tst_uploadsession.cpp     ← 新建
project-docs/tasks/REQ-TEST-008A/task.md
project-docs/evidence/REQ-TEST-008A/
```

> ⚠️ **不修改 `Server/` 下任何文件**（包括被测的 `resumableuploadsession.cpp`）。
> ⚠️ **不修改 `pipeline/test.ps1`**（那是 8b 的活）。

---

## 五、禁止事项（⚠️ 违反即停止并上报）

| # | 禁止 | 原因 |
|---|---|---|
| 1 | **禁止修改 `Server/protocol.h` / `Client/protocol.h`** | frozen_path |
| 2 | **禁止修改被测源码**（`resumableuploadsession.cpp/.h`） | 测试要测**现状**。发现源码有 bug → 报告，不自行改 |
| 3 | **禁止修改 `pipeline/*.ps1`** | 那是 8b 的范围 |
| 4 | **禁止引入第三方测试框架**（gtest/catch2） | 用 Qt Test |
| 5 | **禁止把测试产物写进 `tests/` 或 `Server/`** | 违反 directory_contract；产物在 `build/` |
| 6 | **禁止测试写真实沙箱目录**（`Server/filesys/`） | 用 `QTemporaryDir` |
| 7 | **禁止在测试里删/改真实文件** | 同上 |
| 8 | **禁止写"永真"测试**（如 `QVERIFY(true)`） | A7 要求测试真会失败 |
| 9 | **禁止跳过 A7 的负向验证** | 那是唯一能证明测试有效的步骤 |
| 10 | **禁止顺手改其他缺陷** | 一次一件事 |
| 11 | **禁止 `git push`** | 不可逆 |

> ⚠️ **第 2 条的例外**：如果测试暴露出被测源码的**真 bug**，**不要修**。记录在 task.md，报告主人，作为后续需求。这是 DOS v1.7 的核心原则：**发现偏离 → 记录，不吸收**。

---

## 六、执行流程（Development OS v1.7）

```bash
# ① 开场（确认 REQ-FS-003 已合并）
powershell -File pipeline/state.ps1
git log --oneline -5

# ② 切分支
git switch master
git switch -c test/upload-session

# ③ 写测试（§3.3-3.5）

# ④ 构建 + 运行测试
#    （见 §3.5 命令；全部用例应 PASS）

# ⑤ 跑门
powershell -File pipeline/build.ps1
powershell -File pipeline/test.ps1
powershell -File pipeline/audit.ps1

# ⑥ 【关键】A7 负向验证
#    临时的、不提交的破坏实验，见 §7

# ⑦ 提交
git add tests/uploadsession/
git commit -m "test(upload): add Qt Test suite for ResumableUploadSession"

# ⑧ 合并
git switch master
git merge --no-ff test/upload-session -m "merge: REQ-TEST-008A upload session tests (MEDIUM)"
git branch -d test/upload-session

# ⑨ 停下等批准
```

---

## 七、需要的验证证据

### 7.1 测试运行（正常）

```bash
./build/build-tst-uploadsession/release/tst_uploadsession.exe
# 期望：Totals: N passed, 0 failed, 0 skipped
```

### 7.2 ⚠️ 关键：负向验证（证明测试真的能失败）

**这是 A7，不能省。** 步骤：

```bash
# 1. 记录测试文件的校验和（用于恢复确认）
# 2. 临时改一处被测实现，制造一个真实缺陷，例如：
#    把 resumableuploadsession.cpp:69 的
#      if (offset != m_receivedSize || len <= 0 || len > m_totalSize - m_receivedSize)
#    改成
#      if (len <= 0)
#    （即去掉 offset 连续性校验）
# 3. 重新构建测试并运行
# 4. 【期望】chunkRejectsWrongOffset / chunkRejectsOverrun 变红
# 5. 【必须】用 git checkout -- Server/resumableuploadsession.cpp 完全还原
# 6. 重新构建运行，确认全部恢复 PASS
```

**把第 4 步的红色输出保存为证据。** 同时把第 6 步的绿色输出也保存。

> ⚠️ **第 5 步的还原不可跳过。** 还原后用 `git status` 确认 `Server/` 无改动。
> ⚠️ **绝不能把破坏实验提交上去。**

### 7.3 证据目录

```
build/evidence/test-upload-session/<commit-hash>/
    pipeline-run.log
    test-run.log            # 正常运行的完整输出
    negative-verify.log     # 负向验证：破坏后变红 + 还原后变绿
    manifest.txt
```

```bash
python "C:/Users/王晨扬/.codex/skills/development-os-v1-7/scripts/record-evidence.py" \
  --evidence-dir build/evidence/test-upload-session/<hash> \
  --repo E:/qt/xiangmu \
  --label "REQ-TEST-008A: ResumableUploadSession test suite, negative-verified" \
  tests/uploadsession/tst_uploadsession.cpp tests/uploadsession/tst_uploadsession.pro
```

---

## 八、任务记录与 handoff

### 8.1 `project-docs/tasks/REQ-TEST-008A/task.md`

含：档位 + 依据、验收标准、allowed paths、决策日志。
**必须记录**：
- 用例清单与数量
- **负向验证的完整过程与结果**（改了什么、红了哪些、如何还原）
- 测试**未能**覆盖的部分（诚实列出）
- 若测试暴露了被测源码的 bug → 记录但不修

### 8.2 handoff

`project-docs/handoffs/test-upload-session-<日期>.txt`，**严格 10 行**：

```text
# handoff: REQ-TEST-008A
branch: master
head: <合并后 commit 完整哈希>
tier: MEDIUM
node: merged
audit: none
next: powershell -File pipeline/state.ps1
blockers: test exe not yet registered in pipeline (REQ-TEST-008B)
evidence: project-docs/evidence/REQ-TEST-008A/
owner: claude-code
date: <YYYY-MM-DD>
```

> `blockers` 一行写明"尚未登记进流水线，由 8B 负责"。

```bash
python "C:/Users/王晨扬/.codex/skills/development-os-v1-7/scripts/validate-handoff.py" <handoff 路径>
```

---

## 九、遇到的边界情况怎么办

| 情况 | 怎么办 |
|---|---|
| **REQ-FS-003 尚未合并** | 可先做（本测试不依赖它）。但**若 8B 要登记进流水线，顺序是先 8A 后 8B** |
| 测试跑到一半发现 `QTemporaryDir` 在 Windows 上有路径分隔符问题 | 用 `QDir::toNativeSeparators` 或直接用 `/`（Qt 在 Windows 上接受 `/`） |
| 测试暴露出源码 bug | **记录，不修**。另开需求 |
| 某个断言写不出来（因为实现没暴露必要信息） | **不要改源码来暴露信息**。改为测试可观察的行为，或在 task.md 记录"无法覆盖" |
| 用例数很多，想拆成多个文件 | 允许多个 `tst_*.cpp`，但**本需求先做 uploadsession 一个**，不要提前扩大 |
| 负向验证后忘了还原 | **立即** `git checkout -- Server/resumableuploadsession.cpp`，然后 `git status` 确认 |
| `qmake` 用了 Anaconda 的 | 必须显式用 `C:\Qt\5.14.2\mingw73_64\bin\qmake.exe`（`build.ps1` 就是这么做的） |
| pipeline 报 `FROZEN-PATH HIT` | 立即停止报告 |

---

## 十、完成后请回报这些

1. 最终 commit 哈希（分支 + 合并）
2. **测试用例清单**（函数名 + 断言要点）
3. 正常运行的完整输出（`Totals: N passed`）
4. **负向验证证据**：改了什么、哪些用例变红、如何还原、还原后输出
5. 构建命令与产物路径
6. 三项 pipeline 实际输出
7. **测试未覆盖的部分**（诚实列出）
8. handoff 校验脚本输出

**不要自行推送。**
