# REQ-CFG-010　配置外部化（脱离 Qt 资源）

| 项目 | 值 |
|---|---|
| **需求编号** | REQ-CFG-010 |
| **分支名** | `feat/config-externalization` |
| **任务等级** | **MEDIUM** |
| **基线 commit** | `b5d7bb3`（编写时 master HEAD） |
| **流程版本** | Development OS v1.7 |
| **状态** | 待执行（建议**最后做**——它会碰两个 `connect.config` 的加载方式） |

---

## 一、背景（为什么要做）

**当前配置是"编译进二进制"的。** 实测证据：

### 客户端

`Client/config.qrc`：
```xml
<RCC>
    <qresource prefix="/">
        <file>connect.config</file>     ← 配置被嵌入资源
        ...
    </qresource>
</RCC>
```

`Client/client.cpp:88`：
```cpp
QFile f(":/connect.config");            // ← 从 Qt 资源读，不是从磁盘
```

### 服务端

`Server/config.qrc`：
```xml
<RCC>
    <qresource prefix="/">
        <file>connect.config</file>     ← 同样嵌入
    </qresource>
</RCC>
```

`Server/server.cpp:18`：
```cpp
QFile file(":/connect.config");         // ← 从资源读
```

`Server/Server.pro`：
```pro
RESOURCES += \
    config.qrc
```

### 问题

| # | 问题 | 后果 |
|---|---|---|
| 1 | **改配置必须重新编译** | 改个端口号 → 要 qmake + make 一遍 → 部署时无法"只改配置不重编译" |
| 2 | **配置被编译进 exe** | exe 一旦分发出去，里面的 DB 地址就是**编译时**的值；有敏感信息时会随 exe 泄露 |
| 3 | **无法多环境**（开发/测试/生产） | 一份配置编译一次，多环境要多份 exe |
| 4 | **与 REQ-REPO-004 的处理产生张力** | 既然 `.template` + 忽略规则是"配置不进库"，那配置**本质上就是外部文件**——但代码仍从资源读，说明**它其实还在库里**（`Client/connect.config` 曾长期被跟踪） |
| 5 | 配置读取**没有失败回退** | `Client/client.cpp:88` 若资源缺失 → 弹"打开文件失败"。服务端 `server.cpp:37` 会因行数不足直接返回 false |

### 为什么这是 MEDIUM 不是 SMALL

它**改变运行时行为**（从资源读 → 从磁盘读），影响启动路径，且要处理：
- 找不到配置文件时的行为
- `.template` 与实际配置的关系
- 打包/部署时配置要跟着走
- 两个 `config.qrc` 要处理

**不是改几行就能完成的事。**

---

## 二、目标（验收标准）

| # | 验收标准 | 怎么验证 |
|---|---|---|
| A1 | 服务端从**磁盘**读配置（不再依赖 `:/connect.config`） | 改磁盘配置文件 → **不重编译** → 重启生效 |
| A2 | 客户端从**磁盘**读配置 | 同上 |
| A3 | 配置文件**不存在时**给出**明确错误信息**并退出/提示（不是崩溃、不是静默） | 删掉配置文件 → 启动 → 看到明确错误 |
| A4 | 配置文件**格式错误时**给出明确错误（含"期望 N 行、实际 M 行"） | 造一个 3 行的 server 配置 → 看到明确错误 |
| A5 | 提供 `.template`，README 说明复制步骤 | 按说明能跑起来 |
| A6 | **首次运行时若配置文件缺失，可自动从 `.template` 生成**（可选，见 §3.4） | 删掉配置 → 启动 → 自动生成一份 |
| A7 | 功能行为完全不变（连接、上传、下载全部照旧） | 全链路人工验证 |
| A8 | 不再把配置编译进二进制 | 检查 `.qrc` 已移除 config 条目；检查 exe 内不含配置内容 |
| A9 | 三项 pipeline 全绿 | GREEN / GREEN / PASS |

---

## 三、技术方案（已定型）

### 3.1 配置路径策略（**关键决策**）

从资源改为磁盘后，**从哪里找配置文件**？三个候选：

| 方案 | 路径 | 优点 | 缺点 |
|---|---|---|---|
| **A（推荐）** | exe 同目录下的 `connect.config` | 简单直观，和用户直觉一致 | 需要在构建时把文件复制过去 |
| B | exe 同目录 → 找不到则回退到当前工作目录 | 灵活 | 两个位置容易混淆 |
| C | 系统标准配置目录（`QStandardPaths`） | "规范" | 对学习项目过度设计，用户找不到文件 |

> **推荐 A**：`QCoreApplication::applicationDirPath() + "/connect.config"`。
> 理由：这是个**学习项目**，部署方式就是"把 exe 和配置放一起"。简单即可。
> **实现上建议**：主路径用 A，**若不存在则回退到当前目录**（即 A + B 的组合），并在日志里说明用了哪个路径。这能容忍"从别处启动 exe"的情况。

### 3.2 路径解析辅助函数

新建 `Server/configpath.h` / `.cpp`（客户端也复用同一个实现，或各自一份）：

```cpp
// Server/configpath.h
#ifndef CONFIGPATH_H
#define CONFIGPATH_H

#include <QString>

// 定位配置文件：exe 同目录优先，其次当前工作目录。
// 两者都不存在时返回空字符串。
QString locateConfigFile(const QString &fileName = QStringLiteral("connect.config"));

#endif
```

```cpp
// Server/configpath.cpp
#include "configpath.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

QString locateConfigFile(const QString &fileName)
{
    const QString besideExe = QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(fileName);
    if (QFileInfo::exists(besideExe)) return besideExe;

    const QString inCwd = QDir::current().absoluteFilePath(fileName);
    if (QFileInfo::exists(inCwd)) return inCwd;

    return QString();
}
```

### 3.3 改两个加载点

**服务端 `Server/server.cpp:18`**：

```cpp
bool Server::loadconfig()
{
    const QString path = locateConfigFile();
    if (path.isEmpty()) {
        qCritical() << "connect.config 未找到。请在可执行文件同目录放置配置，"
                       "可复制 connect.config.template 并填入真实值。";
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qCritical() << "connect.config: 打开失败:" << path << file.errorString();
        return false;
    }

    const QString s = QString::fromUtf8(file.readAll());
    // ⚠️ 行尾兼容：原来只 split("\r\n")，现在改为同时支持 \n 与 \r\n
    QStringList lst = s.split(QRegExp("[\r\n]"), QString::SkipEmptyParts);
    ...
    if (lst.size() < 8) {
        qCritical() << "connect.config: 期望至少 8 行，实际" << lst.size()
                    << "行（文件:" << path << "）";
        file.close();
        return false;
    }
    ...
    qDebug() << "loadconfig 使用配置:" << path;
    ...
}
```

> ⚠️ **行尾处理必须改**。原实现 `s.split("\r\n")` 在 Linux/编辑器产出 LF 行尾时会得到**一行**，导致 `lst.size() < 8`。既然要做外部化（文件由人手工创建/复制），必须容忍 `\n`。
> ⚠️ **`Qt::SkipEmptyParts`**：注意 Qt 5.14 用 `QString::SkipEmptyParts`，Qt 5.15+ 才改成 `Qt::SkipEmptyParts`。**本项目是 Qt 5.14.2，用 `QString::SkipEmptyParts`**。若用 `QRegExp` 需要 `#include <QRegExp>`（Qt 5.14 仍可用）。
> **更稳妥的写法**（避免 QRegExp 依赖）：`s.replace("\r\n", "\n"); QStringList lst = s.split('\n', QString::SkipEmptyParts);`

**客户端 `Client/client.cpp:88`**：

```cpp
void Client::loadconfig()
{
    const QString path = locateConfigFile();
    if (path.isEmpty()) {
        QMessageBox::critical(this, "配置缺失",
            "未找到 connect.config。请将其放在程序同目录下，"
            "内容为三行：服务端IP / 端口 / 文件根路径。");
        return;
    }
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        QString s = QString::fromUtf8(f.readAll());
        s.replace("\r\n", "\n");
        QStringList str = s.split('\n', QString::SkipEmptyParts);
        if (str.size() < 3) {
            QMessageBox::critical(this, "配置格式错误",
                QString("connect.config 需要 3 行，实际 %1 行：%2").arg(str.size()).arg(path));
            return;
        }
        m_strlp = str[0];
        m_usPort = str[1].toUShort();
        m_strRootPath = str[2];
        f.close();
    } else {
        QMessageBox::critical(this, "提示", QString("打开配置文件失败: %1").arg(path));
    }
}
```

### 3.4 处理两个 `config.qrc`（**最容易踩的坑**）

`Client/config.qrc` 与 `Server/config.qrc` 都含 `<file>connect.config</file>`。

**改法**：
1. **从 `.qrc` 中移除 `connect.config` 条目**（保留其他资源如图片、qss）
2. 保留 `RESOURCES += config.qrc`（其他资源还要用）

```xml
<!-- Client/config.qrc（移除 connect.config 那一行） -->
<RCC>
    <qresource prefix="/">
        <!-- <file>connect.config</file>  ← 删除 -->
        <file>chat.png</file>
        ...
    </qresource>
</RCC>
```

> ⚠️ **移除 qrc 条目后，配置文件必须存在于磁盘**，否则启动就报错（这正是 A3 要的行为）。
> ⚠️ **不要把 `.qrc` 整个删掉**——图片资源还在里面。

### 3.5 部署时的配置复制（可选但推荐）

在 `.pro` 里加**构建后复制**，让配置文件自动出现在 exe 旁边：

```pro
# Server/Server.pro 追加
CONFIGFILE = $$PWD/connect.config.template
configfilecopy.files = $$CONFIGFILE
configfilecopy.path  = $$OUT_PWD
COPIES += configfilecopy
```

> **⚠️ 慎重**：`COPIES` 是 qmake 的较新特性，**Qt 5.14 支持**，但会在**首次构建时把 `.template` 复制成 `connect.config.template`**（注意是 template，不是真配置）。
> **这解决的是"文件跟着 exe 走"**，不解决"用户要填真实值"。
> **更简单的替代**：不加 `COPIES`，改为在 README 里写清楚"把 `connect.config.template` 复制到 exe 同目录并改名 `connect.config`"。
> **推荐先做简单版（README 指引）**，`COPIES` 作为可选项。若加了 `COPIES` 发现行为不符合预期，**可以去掉**。

### 3.6 A6（自动生成）—— 可选

如果配置文件不存在，**自动从 `.template` 复制一份**，然后提示用户去填：

```cpp
// 若 locateConfigFile() 返回空，可尝试从同目录的 .template 生成
```

> **建议：不做。** 理由：自动生成的配置**内容是错的**（template 里是占位值或 localhost），程序会"看起来启动了但连不上"，比"明确报错说找不到配置"更让人困惑。
> **明确报错（A3）比静默生成更好。** 所以 A6 标记为可选，**默认不做**。

---

## 四、允许改动的文件（allowed paths）

```
Server/configpath.h            ← 新建
Server/configpath.cpp          ← 新建
Server/Server.pro              ← 新增源文件登记（+ 可选 COPIES）
Server/server.cpp              ← 改 loadconfig() 为磁盘读取
Server/config.qrc              ← 移除 connect.config 条目
Client/configpath.h            ← 新建（或复用 Server 的实现）
Client/configpath.cpp          ← 新建
Client/Client.pro              ← 新增源文件登记
Client/client.cpp              ← 改 loadconfig() 为磁盘读取
Client/config.qrc              ← 移除 connect.config 条目
README.md                      ← 更新配置说明（若 REQ-DOC-005 已合并）
project-docs/tasks/REQ-CFG-010/task.md
project-docs/evidence/REQ-CFG-010/
```

> ⚠️ **`Server/connect.config` 与 `Client/connect.config` 本地文件不动**（它们已经不被跟踪，是本地文件）。

---

## 五、禁止事项（⚠️ 违反即停止并上报）

| # | 禁止 | 原因 |
|---|---|---|
| 1 | **禁止修改 `Server/protocol.h` / `Client/protocol.h`** | frozen_path |
| 2 | **禁止删除 `config.qrc`** | 里面还有图片资源 |
| 3 | **禁止删除配置文件格式的字段** | 8 行结构（server）、3 行（client）必须保持 |
| 4 | **禁止改动配置字段的语义或顺序** | 顺序是代码依赖的（`lst[0]`=IP 等） |
| 5 | **禁止去掉"配置缺失时明确报错"的行为** | A3 是核心验收项 |
| 6 | **禁止静默使用默认值** | 会掩盖配置错误 |
| 7 | **禁止自动生成配置为占位值**（除非主人明确要求 A6） | 会制造"看起来正常其实不对"的假象 |
| 8 | **禁止顺手改其他缺陷** | 一次一件事 |
| 9 | **禁止大规模格式化** | 不做全文件缩进/行尾重排 |
| 10 | **禁止 `git push`** | 不可逆 |
| 11 | **禁止在本需求里改数据库密码或连接信息** | 那是环境操作 |

---

## 六、执行流程（Development OS v1.7）

```bash
# ① 开场（建议本需求最后做）
powershell -File pipeline/state.ps1

# ② 切分支
git switch master
git switch -c feat/config-externalization

# ③ 写 configpath 辅助 → 改两个 loadconfig → 改两个 .qrc → 改 .pro

# ④ 跑门
powershell -File pipeline/build.ps1
powershell -File pipeline/test.ps1
powershell -File pipeline/audit.ps1

# ⑤ 人工验证（§7）——本需求的关键验证都要人工做

# ⑥ 提交
git add Server/configpath.h Server/configpath.cpp Server/server.cpp Server/config.qrc Server/Server.pro \
        Client/configpath.h Client/configpath.cpp Client/client.cpp Client/config.qrc Client/Client.pro
git commit -m "feat(config): load connect.config from disk instead of Qt resource"

# ⑦ 合并
git switch master
git merge --no-ff feat/config-externalization -m "merge: REQ-CFG-010 config externalization (MEDIUM)"
git branch -d feat/config-externalization

# ⑧ 停下等批准
```

---

## 七、需要的验证证据（⚠️ 全部要人工做）

| # | 验证 | 怎么做 | 期望 |
|---|---|---|---|
| 1 | **改配置不重编译生效** | 把 server 配置的端口从 5000 改成 5001，**不重新编译**，重启服务端 | 监听 5001 |
| 2 | **配置缺失明确报错** | 临时把 `Server/connect.config` 改名 | 服务端启动打印"未找到 connect.config..."并退出（非崩溃） |
| 3 | **配置格式错误明确报错** | 造一个只有 3 行的 server 配置 | 打印"期望至少 8 行，实际 3 行" |
| 4 | **LF 行尾可读** | 用 LF 行尾（Unix）写配置 | 正常读取（不出现"实际 1 行"） |
| 5 | **CRLF 行尾可读** | 用 CRLF 行尾（Windows）写配置 | 正常读取 |
| 6 | **客户端全功能** | 登录、上传、下载、删除、重命名 | 全部正常 |
| 7 | **exe 内不再含配置** | 在 exe 里搜索配置的独有字符串（如某个端口组合） | 搜不到 |
| 8 | **还原** | 把配置改回原值 | 一切照旧 |

> ⚠️ **第 7 项是本需求的"脱离资源"本质验证。** 用 `strings`（Git Bash 自带）或 `findstr`：
> ```bash
> strings build/build-Server/release/Server.exe | grep -c "5000"
> ```
> 对比改动前后。**注意**：端口号 `5000` 可能作为字符串出现在别处，所以要**用更长的独有串**做判断（比如把配置里 IP 设成一个独特值，再搜它）。

### 证据目录

```
build/evidence/feat-config-externalization/<commit-hash>/
    pipeline-run.log
    manual-verify.log         # 上面 8 项的完整记录
    exe-strings-before.txt
    exe-strings-after.txt
    manifest.txt
```

```bash
python "C:/Users/王晨扬/.codex/skills/development-os-v1-7/scripts/record-evidence.py" \
  --evidence-dir build/evidence/feat-config-externalization/<hash> \
  --repo E:/qt/xiangmu \
  --label "REQ-CFG-010: connect.config loaded from disk, no longer embedded" \
  Server/server.cpp Server/config.qrc Client/client.cpp Client/config.qrc
```

---

## 八、任务记录与 handoff

### 8.1 `project-docs/tasks/REQ-CFG-010/task.md`

含：档位 + 依据、验收标准、allowed paths、决策日志。
**必须记录**：
- 路径策略的选择（§3.1 的 A/B/C）及理由
- 是否加了 `COPIES`，效果如何
- 是否做了 A6（自动生成）及理由
- 行尾兼容的处理
- `QString::SkipEmptyParts` 的 Qt 版本适配（**重要**，见 §9）

### 8.2 handoff

`project-docs/handoffs/feat-config-externalization-<日期>.txt`，**严格 10 行**：

```text
# handoff: REQ-CFG-010
branch: master
head: <合并后 commit 完整哈希>
tier: MEDIUM
node: merged
audit: none
next: powershell -File pipeline/state.ps1
blockers: none
evidence: project-docs/evidence/REQ-CFG-010/
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
| **`QString::SkipEmptyParts` 编译不过** | Qt 5.14 用 `QString::SkipEmptyParts`；Qt 5.15+ 是 `Qt::SkipEmptyParts`。**本项目是 5.14.2 → 用 `QString::`**。若报歧义，改用 `QString::split('\n')` 后手工过滤空串 |
| **`QRegExp` 在 Qt 6 被移除** | 本项目是 Qt 5.14，`QRegExp` 可用。但**推荐用 replace + split 方案**避免依赖 |
| 移除 qrc 条目后编译报找不到 `:/connect.config` | 说明还有代码在引用 `:/` 路径。**全局搜索 `:/connect.config`**，全部改为磁盘路径 |
| 客户端有多个地方读 `:/connect.config` | 同上，全部改 |
| 改了配置但程序仍用旧值 | 检查是不是读到了**另一个位置**的文件（exe 同目录 vs 当前目录）。日志里应打印实际使用的路径 |
| 想让配置支持环境变量覆盖 | **不做**，超出范围，另开需求 |
| `COPIES` 行为不符合预期 | 去掉它，改用 README 指引（§3.5） |
| 本需求与 REQ-REPO-004 冲突（都碰 config 文件） | **先做 004，后做 010**。004 改的是 git 跟踪状态，010 改的是加载方式，不冲突，但顺序上 004 先 |
| pipeline 报 `FROZEN-PATH HIT` | 立即停止报告 |

---

## 十、完成后请回报这些

1. 最终 commit 哈希（分支 + 合并）
2. `locateConfigFile()` 的最终实现
3. 两个 `loadconfig()` 的最终实现
4. 两个 `.qrc` 的 diff
5. **§7 八项人工验证的实际结果**（尤其"改配置不重编译生效"和"配置缺失明确报错"）
6. **exe 字符串对比**（改动前 vs 改动后，证明配置不再嵌入）
7. 是否加了 `COPIES`；若加了，实际效果
8. 三项 pipeline 实际输出
9. handoff 校验脚本输出
10. 遇到的 Qt 版本适配问题

**不要自行推送。**
