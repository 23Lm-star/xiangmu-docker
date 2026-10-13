# REQ-FS-003　上传路径校验 + 文件操作越权

| 项目 | 值 |
|---|---|
| **需求编号** | REQ-FS-003 |
| **分支名** | `fix/path-traversal-and-ownership` |
| **任务等级** | **MEDIUM** |
| **基线 commit** | `b5d7bb3`（编写时 master HEAD） |
| **流程版本** | Development OS v1.7 |
| **状态** | 已实现待审计 —— 分支提交 `f808daf`，pipeline 三绿 + 端到端越权验证 25/25 PASS；**按主人指令暂停合并**，待独立审计会话通过后合并 |

---

## 一、背景（为什么要做）

服务端共 **10 处**接收客户端传来的路径（2026-09-29 修订：原文盘点 6 处，漏了 4 处），其中 **3 处**做了规范路径校验，**7 处缺失**。

### 已核实：10 个路径入口的校验现状（行号按基线 `b5d7bb3` 复核）

| # | 函数 | 位置 | 是否校验 | 校验证明 |
|---|---|---|---|---|
| 1 | `downloadfile()` | `msghandler.cpp:799` | ✅ 有 | `canonicalFilePath` + `startsWith(canonicalRootPrefix)`（803-817 行），**根=整个 filesPath** |
| 2 | `movefile()` | `msghandler.cpp:582` | ✅ 有 | `canonicalSrc` / `canonicalDstDir` 双校验（600-619 行），**根=用户目录 filesPath/loginName** |
| 3 | `listdir()` | `msghandler.cpp:697` | ✅ 有 | `canonicalDir` + `startsWith`（703-716 行），**根=用户目录** |
| 4 | `deldir()` | `msghandler.cpp:389` | ⚠️ **半有** | 只做了**相对路径反推**（410-412 行），用于清理分享链接；**删除动作本身无校验** |
| 5 | `renamefile()` | `msghandler.cpp:481` | ⚠️ **半有** | 同 deldir，靠 `relDir.startsWith("..")` 反推 |
| 6 | **`uploadfileinit()`** | **`msghandler.cpp:733`** | ❌ **无** | 直接 `QString::fromUtf8(pdu->caMsg)` → 传给 `begin()` |
| 7 | **`MKDIR()`** | **`msghandler.cpp:372`** | ❌ **无** | 客户端 path 直接 `dir.mkdir()`（379-380 行），可在任意位置建目录。**原文误记为"已有校验"，2026-09-29 修订纳入本需求** |
| 8 | **`flushfile()`** | `msghandler.cpp:461` | ❌ **无** | 客户端 path 直接 `entryInfoList()`（463-464 行），可**列任意目录**（信息泄露）。修订纳入 |
| 9 | **`changedir()`** | `msghandler.cpp:425` | ❌ **无** | 客户端 path 探测存在性并回显（431-450 行），可**探测任意路径**。修订纳入 |
| 10 | **`sharefilemove()`** | `msghandler.cpp:566` | ❌ **无** | `sourcePath` 全客户端可控（568 行），`QFile::copy` 可把服务器可读的**任意文件**（含 `connect.config`）复制进自己目录再下载 —— **读取逃逸链**。另：目标路径由 `pdu->caData`（接收者名，客户端可控）拼接，`../` 可使目标也逃逸。修订纳入 |

### 缺陷 1（P0-2）：上传路径穿越

`Server/msghandler.cpp:733`：

```cpp
const QString dirPath = QString::fromUtf8(pdu->caMsg);      // ← 客户端完全可控，零校验
const QString fileName = fixedString(strname, 32);
ResumableUploadSession::BeginResult result =
        m_uploadSession.begin(dirPath, fileName, fileSize, resumeEnabled);
```

进入 `Server/resumableuploadsession.cpp:18`：

```cpp
BeginResult ResumableUploadSession::begin(const QString &dirPath, ...)
{
    reset();
    BeginResult result = {false, 0, false};
    if (dirPath.isEmpty() || fileName.isEmpty() || fileName.contains('/')
            || fileName.contains('\\') || totalSize < 0) {
        return result;                                      // ← fileName 校验了，dirPath 没校验
    }
    QDir dir(dirPath);
    if (!dir.exists()) { return result; }                   // ← 只查"目录存在"，不管"在不在沙箱里"
    m_filePath = dir.filePath(fileName);                    // ← 落在任意已存在目录
    ...
}
```

**只有 `fileName` 被校验（不允许含 `/` `\`），`dirPath` 完全放行。** 后果：

- `dirPath = C:/Windows/Temp` → 可在系统目录下建文件
- `dirPath = E:/qt/xiangmu/Server` → 可覆盖服务端源码（下次编译即被执行）
- 配合 `resumeEnabled`（客户端可控 magic）→ 可 **Append** 到已存在文件尾部，篡改已有文件内容

`fileName` 被限制为 32 字节且无路径分隔符，所以不能靠文件名穿越；**但目录穿越这条路完全敞开**。

### 缺陷 2（P1）：删除/重命名的动作本身无归属校验

`deldir()`（389 行）与 `renamefile()`（481 行）**先执行文件操作，再反推相对路径**：

```cpp
// deldir 现状顺序
if(delFileType==1){ QFile file(delFilePath); ans=file.remove(); }       // ← 先删
else if(delFileType==0){ QDir dir(delFilePath); ans=dir.removeRecursively(); }
if(ans){ /* 反推 relPath，清理分享链接 */ }                              // ← 后反推
```

`delFilePath` 来自 `pdu->caMsg`，客户端可控。**没有 `canonicalFilePath` 校验**，因此：

- 可删 `filesPath` 之外的任意文件/目录（`QDir::removeRecursively()` 递归删目录）
- 可重命名 `filesPath` 之外的任意文件
- `loginName` 参数只被用于**事后**清理分享链接，没有参与**授权判定**

对比 `downloadfile` / `movefile` / `listdir` 都做了 `canonicalFilePath` + 前缀校验，**这三个函数的写法就是本需求的正面范例，照抄即可**。

---

## 二、目标（验收标准）

| # | 验收标准 | 怎么验证 |
|---|---|---|
| A1 | 上传目录若不在用户沙箱内，服务端**拒绝**（`ok=false`），不创建任何文件 | 构造 `dirPath=../../` 的上传请求，返回 `ret=false`；确认无文件落地 |
| A2 | 上传目录为合法沙箱内路径时，功能**完全正常** | 正常上传一个文件，成功且内容正确 |
| A3 | 断电续传（resume）功能不受影响 | 中断后重传，从正确 offset 续传 |
| A4 | 删除不在沙箱内的文件/目录**被拒绝**，且**不执行删除动作** | 构造越权删除请求，返回 `false`，目标文件**依然存在** |
| A5 | 删除自己沙箱内的文件/目录**正常** | 正常删除，成功 |
| A6 | 重命名越权路径**被拒绝**，目标文件**依然存在**（F-1 修订：含名称段 `../`/`\` 拼接逃逸） | 构造越权重命名（dirPath 逃逸 + 名称段逃逸两类），返回 `false`，原文件未被改名 |
| A7 | 重命名沙箱内文件**正常** | 正常重命名，成功 |
| A8 | 所有路径校验逻辑**集中在一处**，不复制粘贴 | 代码审查：只有一个 `isPathInSandbox()` 实现 |
| A9 | 三项 pipeline 全绿 | `build.ps1` GREEN、`test.ps1` GREEN、`audit.ps1` PASS |
| A10 | MKDIR 沙箱外建目录**被拒绝**；沙箱内建目录正常 | 构造越权 mkdir，返回 false 且目录未创建（2026-09-29 修订新增） |
| A11 | flushfile/changedir 对沙箱外路径**不回显任何真实信息**（返回空/不存在）；沙箱内正常 | 构造越权请求，确认无目录名/存在性泄露（修订新增） |
| A12 | sharefilemove 源在沙箱外或目标目录经 `caData` 逃逸时**被拒绝**，不产生任何复制文件；沙箱内正常共享不受影响 | 以沙箱外临时文件为源发起复制，返回 false 且目标位置无新文件（修订新增） |

---

## 三、技术方案（已定型，不要自行改动思路）

### 3.1 核心原则：一个校验函数，四处调用

**不要**在 4 个函数里各写一遍 `canonicalFilePath` 逻辑（重复代码 = 未来改一处漏三处）。抽一个**静态辅助函数**：

放到 `Server/msghandler.h`（类内静态成员），实现放 `msghandler.cpp`：

```cpp
// msghandler.h（public 段，2026-09-29 修订：增加可选根参数）
// 判断 path 是否位于指定沙箱根内（含根本身）。
// 规则：根与 path 分别做 canonical 归一（解析 ../ 与符号链接），再判断前缀。
// 返回 true 仅当：根存在 且 path 存在 且 canonical(path) 落在 canonical(root) 之内。
// root 为空 → 默认 Server::getinstance().filesPath。
static bool isPathInSandbox(const QString &path, const QString &root = QString());
```

```cpp
// msghandler.cpp
bool MsgHandler::isPathInSandbox(const QString &path, const QString &root)
{
    if (path.isEmpty()) return false;

    const QString rootRaw = root.isEmpty() ? Server::getinstance().filesPath : root;
    // 实测 connect.config 第 3 行为相对路径 "./filesys" → 必须先绝对化归一（§9 已坐实为必选项）
    QFileInfo rootInfo(QDir(rootRaw).absolutePath());
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    if (canonicalRoot.isEmpty()) return false;      // 沙箱根不存在 → 一律拒绝

    QFileInfo targetInfo(path);
    const QString canonicalTarget = targetInfo.canonicalFilePath();
    if (canonicalTarget.isEmpty()) return false;    // 路径不存在（含非法路径）→ 拒绝

    QString prefix = canonicalRoot;
    if (!prefix.endsWith('/')) prefix.append('/');

    return canonicalTarget == canonicalRoot || canonicalTarget.startsWith(prefix);
}
```

> **根参数用途**：`movefile`/`listdir` 替换现有校验时传入用户目录根（`QDir(filesPath).filePath(loginName)`），**保持现有用户级隔离不放松**；其余入口用默认根（整个 filesPath）。

> **注意 canonical 的一个特性**：`canonicalFilePath()` 对**不存在**的路径返回空字符串。所以：
> - 对**删除/重命名**（目标必须已存在）→ 直接用 `isPathInSandbox(target)` 即可，天然拒绝不存在路径。
> - 对**上传**（目标目录必须已存在，但文件可能尚不存在）→ 应校验 **`dirPath`（目录，必定存在）**，也就是 `isPathInSandbox(dirPath)`。**不要对最终文件路径做 canonical**，因为新文件还不存在会返回空。

### 3.2 改动块（2026-09-29 修订后共 ①–⑤ 五块，覆盖全部 10 个路径入口）

**① `uploadfileinit()`（`msghandler.cpp:733`）—— 在 `begin()` 之前拦一道**

```cpp
const QString dirPath = QString::fromUtf8(pdu->caMsg);
const QString fileName = fixedString(strname, 32);

ResumableUploadSession::BeginResult result = {false, 0, false};
if (isPathInSandbox(dirPath)) {                       // ← 新增校验
    result = m_uploadSession.begin(dirPath, fileName, fileSize, resumeEnabled);
} else {
    qWarning() << "uploadfileinit 拒绝越权目录:" << dirPath;
}
```

> **不要改 `ResumableUploadSession::begin()` 的签名或逻辑。** 校验放在调用方（`MsgHandler`），因为只有 `MsgHandler` 能拿到 `Server::getinstance().filesPath`（沙箱根）。`ResumableUploadSession` 是个纯文件会话类，保持它不依赖全局单例。
> **`begin()` 内部现有的 `fileName` 校验保留**，那是纵深防御的第二层，不要删。

**② `deldir()`（`msghandler.cpp:389`）—— 把校验提到删除动作之前**

现状顺序是「先删后反推」，必须改成「**先校验，后删除**」：

```cpp
PDU *MsgHandler::deldir(const QString& loginName)
{
    QString delFilePath(QString::fromUtf8(pdu->caMsg));
    uint delFileType=0;
    memcpy(&delFileType,pdu->caData,sizeof(uint));

    bool ans = false;
    if (!isPathInSandbox(delFilePath)) {                         // ← 新增：越权直接拒绝
        qWarning() << "[deldir] 拒绝越权删除:" << delFilePath;
    } else if (delFileType==1) {
        QFile file(delFilePath);  ans=file.remove();
    } else if (delFileType==0) {
        QDir dir(delFilePath);    ans=dir.removeRecursively();
    }
    // 下面的分享链接联动清理逻辑保持不变（现在只有 ans==true 才可能走到）
    ...
}
```

> ⚠️ **顺序不能颠倒。** 校验必须在 `file.remove()` / `removeRecursively()` **之前**。`removeRecursively()` 是不可逆删除，删完再校验等于没校验。

**③ `renamefile()`（`msghandler.cpp:481`）—— 前置校验所在目录 + 名称段检查（2026-09-29 二次修订，审计 F-1）**

旧名/新名共用同一目录 `dirPath`（`pdu->caMsg+sizeof(uint)`），校验 `dirPath` 一处即可——**但前提是名称段本身不含路径分隔符**。`oldFileName/newFileName` 来自 `caData`（客户端可控），若含 `../`，拼接串 `dirPath+"/"+name` 在路径归一后越过已校验的 `dirPath`（独立审计 F-1，A6 绕过）。修订为：

> ⚠️ **陷阱**：不要对 `newFilePath` 做 `isPathInSandbox`。目标文件在 rename 前不存在，`canonicalFilePath()` 返回空 → 直接校验它会把**所有正常重命名误拒**（A7 必挂）。正确做法是拒绝名称段分隔符，使拼接结果不可能离开 `dirPath`。

```cpp
const QString dirPath = QString::fromUtf8(pdu->caMsg + sizeof(uint));
const QString oldName = fixedString(oldFileName, 32);   // 同 MKDIR 模式
const QString newName = fixedString(newFileName, 32);
if (!isPathInSandbox(dirPath)) {
    qWarning() << "[renamefile] 拒绝越权重命名目录:" << dirPath;
} else if (oldName.contains('/') || oldName.contains('\\')
        || newName.contains('/') || newName.contains('\\')) {
    qWarning() << "[renamefile] 拒绝越权重命名名称段:" << oldName << newName;
} else {
    // 原来的 rename 逻辑（含事后的分享链接联动，联动复用净化后的 oldName/newName）
}
```

**④ `movefile()` / `listdir()` / `downloadfile()` —— 用新函数替换现有重复逻辑（2026-09-29 修订）**

> 更正原文事实错误：**MKDIR 并非"已有校验"**，它从未被校验过，已移入 ⑤ 做新增拦截。本步只覆盖三处真正有校验的入口。

三处现有校验的**根不同**：`movefile`/`listdir` 的根是**用户目录**（`filesPath/loginName`），`downloadfile` 的根是**整个 filesPath**。替换时必须把原根通过 `isPathInSandbox(path, root)` 的根参数传入，**严格保持现有隔离级别，不收紧也不放松**。**功能上不要改行为**：只替换重复的 canonical 样板代码；**如果替换过程中发现行为有差异（比如原来允许某个边界情况），以原行为为准** —— 那属于另一件事。

> 如果替换风险感觉较大，**可以只做 ①②③⑤，第 ④ 步留给后续需求**。最小必要安全改动是 ①②③⑤。**优先保证不引入回归。**

**⑤ `MKDIR()` / `flushfile()` / `changedir()` / `sharefilemove()` —— 新增拦截（2026-09-29 修订，漏盘入口纳入）**

四处均为原本零校验的入口，统一用 `isPathInSandbox()`（默认根 filesPath）**前置拦截**：

| 函数 | 校验对象 | 拒绝时行为 |
|---|---|---|
| `MKDIR` | `path`（待建目录的所在路径） | 不调用 `mkdir`，`ret1/ret2=false` 返回 |
| `flushfile` | `pdu->caMsg` 目录 | 返回空条目 `respdu`，不列任何目录项 |
| `changedir` | `tarPath` | 按 `isExist=false` 路径返回，不回显任意路径信息 |
| `sharefilemove` | `sourcePath` **与** 目标目录 `filesPath/<caData段>` 都要校验（caData 客户端可控，`../` 可使目标逃逸） | `ret=false`，不调用 `copy` |

> 这四处是对**原本无任何校验**的入口新增拦截：正常客户端流程中路径本就落在沙箱内，行为不变（对应禁止事项 #8"不弱化合法功能"）。

### 3.3 明确不做的事

- **不改协议**。路径仍然通过 `pdu->caMsg` 传递，不加签名字段。
- **不改客户端**。客户端照常发路径。
- **不引入"用户归属目录"新概念**。本需求只保证「不逃出 `filesPath`」，**不**做「A 用户不能删 B 用户文件」的细粒度隔离——那是更大的设计（需要给每个用户建沙箱子目录并在所有入口校验），**另开需求**。
  > 本次的 `loginName` 参数仍然只用于分享链接联动，不参与授权判定。
  > （2026-09-29 修订注：`movefile`/`listdir` **现有**的用户目录根属于维持原状，不是本次新引入的概念；新增拦截一律用默认根 filesPath。）

---

## 四、允许改动的文件（allowed paths）

```
Server/msghandler.h               ← 新增 isPathInSandbox 声明
Server/msghandler.cpp             ← 新增实现 + 调用点（修订后覆盖 deldir/renamefile/uploadfileinit/MKDIR/flushfile/changedir/sharefilemove 新增拦截 + movefile/listdir/downloadfile 替换）
project-docs/tasks/REQ-FS-003/task.md
project-docs/evidence/REQ-FS-003/
```

**`Server/resumableuploadsession.cpp` 与 `.h` 不在允许清单内**（保持原样，除非执行中发现必须改 → 停下报告）。

---

## 五、禁止事项（⚠️ 违反即停止并上报）

| # | 禁止 | 原因 |
|---|---|---|
| 1 | **禁止修改 `Server/protocol.h` / `Client/protocol.h`** | frozen_path 冻结路径，触发 LARGE |
| 2 | **禁止修改 `Server/resumableuploadsession.*` 的接口** | 校验放调用方，会话类保持纯文件逻辑 |
| 3 | **禁止把校验放在删除动作之后** | 顺序颠倒 = 校验无效 |
| 4 | **禁止只校验 `fileName` 不校验 `dirPath`** | 本次缺陷的核心就是 dirPath 漏校验 |
| 5 | **禁止顺手改其他缺陷** | 日志分级、密码加盐各自有独立分支 |
| 6 | **禁止引入"用户级目录隔离"** | 超出范围，见 §3.3 |
| 7 | **禁止大规模格式化** | 不做全文缩进/行尾/include 重排 |
| 8 | **禁止弱化任何合法功能** | 正常上传、下载、删除、重命名、续传必须全部照旧可用 |
| 9 | **禁止自行决定档位升降** | 需升 LARGE → 停下报告 |
| 10 | **禁止 `git push`** | 不可逆操作，等主人批准 |
| 11 | **禁止用 `rm`/`del` 手工删除测试文件** | 测试产生的越权文件走回收站或放在 `build/` 内清理 |

---

## 六、执行流程（Development OS v1.7）

```bash
# ① 会话开场
powershell -File pipeline/state.ps1
#    branch: master   suspended git state: none  ← 否则停下

# ② 切分支
git switch master
git switch -c fix/path-traversal-and-ownership

# ③ 写代码（只改 §4 文件）

# ④ 跑门（顺序：先 test 后 audit）
powershell -File pipeline/build.ps1
powershell -File pipeline/test.ps1
powershell -File pipeline/audit.ps1

# ⑤ 人工验证（§7 的越权场景 pipeline 测不出）

# ⑥ 提交
git add Server/msghandler.h Server/msghandler.cpp
git commit -m "fix(security): enforce sandbox path validation on upload/delete/rename"

# ⑦ 合并回 master
git switch master
git merge --no-ff fix/path-traversal-and-ownership -m "merge: REQ-FS-003 path traversal + ownership (MEDIUM)"
git branch -d fix/path-traversal-and-ownership

# ⑧ 停下等主人批准推送
```

---

## 七、需要的验证证据

pipeline 证明不了「越权被拦住」。必须**人工构造越权请求**并记录结果。

### 必须做的越权测试

| 场景 | 构造方式 | 期望 |
|---|---|---|
| 上传逃逸 | `dirPath` 填 `C:/Windows/Temp`（或 `../../`） | 服务端日志出现「拒绝越权目录」，无文件写入 |
| 上传正常 | `dirPath` 填用户真实沙箱目录 | 上传成功，文件内容一致 |
| 续传 | 上传中断后重传 | offset 正确，续传成功 |
| 删除逃逸 | `delFilePath` 填沙箱外一个**测试用**文件 | 返回 false，**该文件依然存在** |
| 删除正常 | 删自己沙箱内的测试文件 | 成功删除 |
| 重命名逃逸 | 源路径填沙箱外测试文件 | 返回 false，**文件名未变** |
| 重命名名称段逃逸（F-1 修订新增） | `dirPath` 填合法沙箱目录，旧名或新名段填 `../../../build/<临时文件>` 或含 `\` | 返回 false，服务端日志出现「拒绝越权重命名名称段」，**目标/源文件完好** |
| mkdir 逃逸（修订新增） | `MKDIR` 的 path 填沙箱外路径（如 `build/` 下临时目录） | 返回 false，目录未创建 |
| 列目录逃逸（修订新增） | `flushfile` 的 path 填沙箱外目录 | 不回显任何目录项 |
| 共享读取逃逸（修订新增） | `sharefilemove` 源填沙箱外测试文件 | 返回 false，目标目录**无新文件** |

> ⚠️ **越权测试用的"目标文件"必须是自己新建的临时文件**（建议放 `build/` 下），**绝不能拿系统文件或项目源码做实验**。测完记得清理。
> ⚠️ 测试前**先备份**任何会被碰到的真实文件。

### 证据目录

```
build/evidence/fix-path-traversal-and-ownership/<commit-hash>/
    pipeline-run.log
    manual-verify.log     # 上面 10 个场景的实际输出
    manifest.txt
```

```bash
python "C:/Users/王晨扬/.codex/skills/development-os-v1-7/scripts/record-evidence.py" \
  --evidence-dir build/evidence/fix-path-traversal-and-ownership/<hash> \
  --repo E:/qt/xiangmu \
  --label "REQ-FS-003: sandbox path validation enforced on 4 entries" \
  Server/msghandler.cpp Server/msghandler.h
```

---

## 八、任务记录与 handoff（必须产出）

### 8.1 `project-docs/tasks/REQ-FS-003/task.md`

含：档位 + 依据、验收标准、allowed paths、决策日志（append-only，尤其记录「哪些函数用了新函数替换、哪些没动」）。

### 8.2 handoff

路径 `project-docs/handoffs/fix-path-traversal-and-ownership-<日期>.txt`，**严格 10 行**：

```text
# handoff: REQ-FS-003
branch: master
head: <合并后 commit 完整哈希>
tier: MEDIUM
node: merged
audit: none
next: powershell -File pipeline/state.ps1
blockers: none
evidence: project-docs/evidence/REQ-FS-003/
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
| `filesPath` 配的是相对路径（`./filesys`），canonical 后可能对不上 | **2026-09-29 已实测坐实**：`connect.config` 第 3 行就是 `./filesys`。`isPathInSandbox` 内对根做 `QDir(root).absolutePath()` 归一**已是定型方案的必选项**（见 §3.1 实现） |
| 替换已有校验时发现行为变了 | **回退到原逻辑**，只做 ①②③ 三处新增。不要为了"统一"引入回归 |
| 想顺便做用户级隔离 | **不做**。另开需求 |
| `uploadfileinit` 里 `dirPath` 是空串 | `isPathInSandbox("")` 返回 false → 拒绝，符合预期 |
| 客户端上传时带的路径是相对路径 | 需要确认客户端实际发的是绝对路径。**先实测**，如果是相对路径，先弄清基准目录再决定方案，**不要想当然** |
| 测试中误删了真实文件 | 立即停止，报告，从备份恢复。记录到决策日志 |
| pipeline 报 `FROZEN-PATH HIT` | 立即停止报告 |

---

## 十、完成后请回报这些

1. 最终 commit 哈希（分支 + 合并）
2. 三项 pipeline 实际输出（GREEN / GREEN / PASS）
3. §7 十个场景的实际结果（**尤其"越权被拒且目标完好"的各条**）
4. `isPathInSandbox` 最终实现的代码片段
5. 第 ④ 步（替换已有校验，movefile/listdir 传用户根）做了没有；若做了，说明行为是否有差异；第 ⑤ 步四处新增拦截全部落地情况
6. handoff 校验脚本输出
7. 任何偏差或未预期情况

**不要自行推送。**
