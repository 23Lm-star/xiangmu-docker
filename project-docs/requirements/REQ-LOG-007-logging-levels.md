# REQ-LOG-007　日志分级治理

| 项目 | 值 |
|---|---|
| **需求编号** | REQ-LOG-007 |
| **分支名** | `refactor/logging-levels` |
| **任务等级** | **MEDIUM** |
| **基线 commit** | `b5d7bb3`（编写时 master HEAD） |
| **流程版本** | Development OS v1.7 |
| **状态** | 待执行（**依赖 REQ-AUTH-002 先完成**） |

---

## 一、背景（为什么要做）

实测统计：**服务端 + 客户端共 13 个文件、213 处日志调用**，其中 **211 处是 `qDebug()`**、`qWarning` 仅 1 处、`qCritical` 11 处。

| 文件 | 日志调用数 |
|---|---|
| `Server/msghandler.cpp` | 77 |
| `Server/operatedb.cpp` | 38 |
| `Client/msghandler.cpp` | 22 |
| `Client/file.cpp` | 17 |
| `Server/mytcpsocket.cpp` | 13 |
| `Client/client.cpp` | 9 |
| `Server/mytcpserver.cpp` | 8 |
| `Server/server.cpp` | 7 |
| `Client/friend.cpp` | 7 |
| `Server/databaseconnectionpool.cpp` | 4 |
| `Client/chat.cpp` | 3 |
| `Server/main.cpp` / `Client/main.cpp` / `Client/onlineuser.cpp` | 各 1 |

**问题**：

1. **全部走 `qDebug()`**，语义完全丢失。真正该报的错误（`Server/server.cpp:20` 配置打开失败）和执行细节（`handleLogin: pwd_hex=`）同一个级别。
2. **`qDebug()` 默认输出到 stderr，且无法在运行时关掉**。生产环境这些输出会持续刷屏、拖慢 IO、且可能被日志系统收走。
3. **未配置任何 Qt 日志规则**（无 `qtlogging.ini`、无 `QLoggingCategory`），意味着无法做到"默认安静、需要时打开"。
4. 客户端也有敏感输出：`Client/file.cpp:411` 把**上传路径**打出来。

> **本需求与 REQ-AUTH-002 的关系**：AUTH-002 已经删掉 7 处**凭据**日志（`operatedb.cpp:229/230/237/239`、`msghandler.cpp:222/223/224`）。本需求负责**剩下的全部**——把日志按语义分级，并建立**可持续**的规则（不是删掉就完事）。
> **所以本需求必须在 AUTH-002 合并之后再开始**，否则两者会改同一批行、必然冲突。

---

## 二、目标（验收标准）

| # | 验收标准 | 怎么验证 |
|---|---|---|
| A1 | 建立日志分类：至少区分「安全/错误」「警告」「调试详情」三级 | 代码审查 + 有分类声明 |
| A2 | **默认运行时，详细日志不输出**（安静启动） | 启动服务端，控制台只有必要的启动信息 |
| A3 | **需要排查时，可用环境变量打开详细日志** | 设置变量后重启，详细日志出现 |
| A4 | 真正的错误（配置失败、连接失败、协议错误）用 `qCritical`/`qWarning` | 代码审查 |
| A5 | 高频调试输出（每次请求都打的）降级到"详细"类 | 代码审查 + 运行观察 |
| A6 | 客户端不再打印上传/下载的完整路径 | 搜索确认 |
| A7 | **不删任何一条日志**（除 AUTH-002 已处理的凭据日志），只改级别 | 对比改动前后日志语句数量 |
| A8 | 功能行为完全不变 | 登录/上传/下载/删除全部照旧 |
| A9 | 三项 pipeline 全绿 | GREEN / GREEN / PASS |

> **重点：A7 是本需求的核心约束。** 不是"把 qDebug 都删了"，而是"分级 + 可开关"。删日志会让人在出问题时无从下手。

---

## 三、技术方案（已定型）

### 3.1 引入 `QLoggingCategory`（Qt 官方机制）

Qt 提供 `QLoggingCategory` 做运行时可控分级，**不需要引入任何第三方库**。

新建 `Server/logging.h`：

```cpp
#ifndef LOGGING_H
#define LOGGING_H

#include <QLoggingCategory>

// 分类声明（定义放 logging.cpp）
Q_DECLARE_LOGGING_CATEGORY(net)      // 网络层：连接、收发、协议
Q_DECLARE_LOGGING_CATEGORY(db)       // 数据库层：SQL、连接池
Q_DECLARE_LOGGING_CATEGORY(fs)       // 文件系统：上传/下载/删除/重命名
Q_DECLARE_LOGGING_CATEGORY(auth)     // 认证：登录/注册/注销/验证码
Q_DECLARE_LOGGING_CATEGORY(app)      // 应用生命周期：配置、启动

#endif // LOGGING_H
```

新建 `Server/logging.cpp`：

```cpp
#include "logging.h"
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(net,  "xmu.net")
Q_LOGGING_CATEGORY(db,   "xmu.db")
Q_LOGGING_CATEGORY(fs,   "xmu.fs")
Q_LOGGING_CATEGORY(auth, "xmu.auth")
Q_LOGGING_CATEGORY(app,  "xmu.app")
```

### 3.2 默认关闭详细日志

在 `Server/main.cpp` 的 `main()` **最开头**（`QApplication` 之后即可）加：

```cpp
#include "logging.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // 默认：只显示 warning 及以上；debug 默认关闭。
    // 排查时设环境变量 QT_LOGGING_RULES="xmu.*.debug=true" 即可打开全部详细日志。
    QLoggingCategory::setFilterRules(QStringLiteral(
        "xmu.*.debug=false\n"
        "xmu.*.info=true"));

    ...
}
```

> **Qt 本身的规则字符串优先级高于 `setFilterRules`**：如果用户设了 `QT_LOGGING_RULES` 环境变量，它会**覆盖**代码里的设置。这正是我们要的——**默认安静，需要时用环境变量打开**。
> 客户端 `Client/main.cpp` 做同样处理（分类前缀用 `xmu.cli.*`，或复用同一套）。

### 3.3 分级映射规则（照这个表改，逐条对照）

| 原级别 | 场景 | 改成 | 例子 |
|---|---|---|---|
| `qDebug` | 每次请求都打、纯执行细节 | **`qCDebug(<cat>)`** | `msghandler.cpp` 里 upload/download/deldir 的 `qDebug()<<"..."` |
| `qDebug` | 错误情况（但当前错用了 debug） | **`qCWarning(<cat>)`** | `operatedb.cpp:251` "user name or password not found" |
| `qDebug` | 用户/文件标识（非凭据） | `qCDebug(<cat>)`，**若含路径则考虑移除路径** | `deldir` 的 "删除文件: <path>" |
| 已是 `qWarning` | 保持不变 | `qCWarning(<cat>)` | `mytcpsocket.cpp:345` 缓冲区超限 |
| 已是 `qCritical` | 保持不变 | `qCCritical(<cat>)` | `server.cpp` 配置/监听失败 |

### 3.4 分类归属速查

| 文件 | 归属分类 |
|---|---|
| `Server/mytcpserver.cpp`、`Server/mytcpsocket.cpp` | `net` |
| `Server/operatedb.cpp`、`Server/databaseconnectionpool.cpp` | `db` |
| `Server/msghandler.cpp` 的文件相关（upload/download/deldir/renamefile/movefile/listdir/MKDIR） | `fs` |
| `Server/msghandler.cpp` 的认证相关（login/rigist/logoff/captcha） | `auth` |
| `Server/msghandler.cpp` 的其余（好友、聊天） | `net` |
| `Server/server.cpp`、`Server/main.cpp` | `app` |
| `Client/*` | 对应分类（客户端可另建 `xmu.cli.*`，或复用同一套 + 文件级判断） |

> **`Server/msghandler.cpp` 有 77 处，归三类**。按**所在函数**判断，不要一刀切。

### 3.5 客户端处理（可简化）

客户端不需要 5 个分类那么细。**建议**：客户端只保留一个分类 `xmu.cli`，把 22+17+9+7+3 处 `qDebug` 全部映射到 `qCDebug(xmu.cli)`，并在 `Client/main.cpp` 里默认关闭。

> **客户端也有敏感输出要处理**：
> - `Client/file.cpp:411`：`qDebug()<<"文件名"<<pdu->caData<<"文件大小"<<fileSize<<" 上传路径"<<pdu->caMsg` → 路径降级或去掉路径
> - `Client/file.cpp:205`、`:320`：打印 `pdu->caData`（含文件名/路径） → 降级
> 处理原则：**文件名可以留（debug 级），完整绝对路径不要打。**

---

## 四、允许改动的文件（allowed paths）

```
Server/logging.h                  ← 新建
Server/logging.cpp                ← 新建
Server/Server.pro                 ← 新增 SOURCES/HEADERS 条目
Server/main.cpp                   ← 设置默认过滤规则
Server/msghandler.cpp             ← 日志分级（77 处）
Server/operatedb.cpp              ← 日志分级（38 处）
Server/mytcpserver.cpp            ← 日志分级（8 处）
Server/mytcpsocket.cpp            ← 日志分级（13 处）
Server/server.cpp                 ← 日志分级（7 处）
Server/databaseconnectionpool.cpp ← 日志分级（4 处）
Client/file.cpp                   ← 日志分级 + 去路径（17 处）
Client/msghandler.cpp             ← 日志分级（22 处）
Client/client.cpp                 ← 日志分级（9 处）
Client/friend.cpp                 ← 日志分级（7 处）
Client/chat.cpp                   ← 日志分级（3 处）
Client/onlineuser.cpp             ← 日志分级（1 处）
Client/main.cpp                   ← 设置默认过滤规则 + 1 处日志
Client/Client.pro                 ← 若客户端也加分类文件
project-docs/tasks/REQ-LOG-007/task.md
project-docs/evidence/REQ-LOG-007/
```

> 注意 `.pro` 改动会带来一个联带问题：**新增 `logging.cpp` 后，`SOURCES`/`HEADERS` 要登记**，否则链接不过。这是本需求最容易踩的坑。

---

## 五、禁止事项（⚠️ 违反即停止并上报）

| # | 禁止 | 原因 |
|---|---|---|
| 1 | **禁止修改 `Server/protocol.h` / `Client/protocol.h`** | frozen_path，触发 LARGE |
| 2 | **禁止删除任何日志语句**（除 AUTH-002 已删的凭据日志） | A7：只改级别不删 |
| 3 | **禁止引入第三方日志库**（spdlog/log4cxx 等） | 用 Qt 自带的 `QLoggingCategory` 就够 |
| 4 | **禁止改变日志的文本内容**（除去掉路径） | 只改级别与分类，方便 diff 审查 |
| 5 | **禁止顺手改其他缺陷** | 一次一件事 |
| 6 | **禁止大规模格式化** | 不做缩进/行尾/include 重排 |
| 7 | **禁止在 AUTH-002 未合并前开始** | 会改同一批行必然冲突 |
| 8 | **禁止把默认级别设成"什么都打"** | A2 要求默认安静 |
| 9 | **禁止弱化错误日志**（把 `qCritical` 降级） | 错误必须留在错误级别 |
| 10 | **禁止 `git push`** | 不可逆 |

---

## 六、执行流程（Development OS v1.7）

```bash
# ① 开场（并确认 AUTH-002 已合并进 master）
powershell -File pipeline/state.ps1
git log --oneline -5          # 应能看到 AUTH-002 的 merge commit

# ② 切分支
git switch master
git switch -c refactor/logging-levels

# ③ 建分类文件 → 改 .pro → 逐个文件改级别（建议先服务端后客户端）

# ④ 跑门
powershell -File pipeline/build.ps1
powershell -File pipeline/test.ps1
powershell -File pipeline/audit.ps1

# ⑤ 人工验证默认安静 + 环境变量可打开（§7）

# ⑥ 提交
git add Server/logging.h Server/logging.cpp Server/Server.pro \
        Server/*.cpp Client/*.cpp Client/Client.pro
git commit -m "refactor(logging): introduce QLoggingCategory levels, quiet by default"

# ⑦ 合并
git switch master
git merge --no-ff refactor/logging-levels -m "merge: REQ-LOG-007 logging levels (MEDIUM)"
git branch -d refactor/logging-levels

# ⑧ 停下等批准
```

---

## 七、需要的验证证据

| 验证 | 怎么验 |
|---|---|
| **默认安静** | 启动服务端，记录控制台输出（应只有启动必需信息，无每次请求的细节） |
| **可打开** | 设 `QT_LOGGING_RULES="xmu.*.debug=true"`，重启，记录输出（细节应出现） |
| **可单类打开** | 设 `QT_LOGGING_RULES="xmu.db.debug=true"`，只应看到数据库相关细节 |
| **错误仍可见** | 故意制造配置错误（临时改名 `connect.config`），应看到 `qCritical` 输出 |
| **日志语句数未减少** | `grep -c "qCDebug\|qCWarning\|qCCritical\|qDebug\|qWarning\|qCritical"`，与改动前总量对比（应≈持平，仅减少 AUTH-002 已删的 7 条） |
| **无路径打印** | 搜索客户端上传/下载日志，确认不打完整路径 |

### 证据目录

```
build/evidence/refactor-logging-levels/<commit-hash>/
    pipeline-run.log
    quiet-run.log         # 默认启动的输出
    verbose-run.log       # QT_LOGGING_RULES 打开后的输出
    manual-verify.log     # 其余验证
    manifest.txt
```

```bash
python "C:/Users/王晨扬/.codex/skills/development-os-v1-7/scripts/record-evidence.py" \
  --evidence-dir build/evidence/refactor-logging-levels/<hash> \
  --repo E:/qt/xiangmu \
  --label "REQ-LOG-007: QLoggingCategory levels, quiet by default, opt-in verbose" \
  Server/logging.h Server/logging.cpp Server/main.cpp Client/main.cpp
```

---

## 八、任务记录与 handoff

### 8.1 `project-docs/tasks/REQ-LOG-007/task.md`

含：档位 + 依据、验收标准、allowed paths、决策日志。
**必须记录**：分类归属的判定依据（哪些函数归 `fs`、哪些归 `auth`）、日志总数改动前后对比、客户端分类的简化决策。

### 8.2 handoff

`project-docs/handoffs/refactor-logging-levels-<日期>.txt`，**严格 10 行**：

```text
# handoff: REQ-LOG-007
branch: master
head: <合并后 commit 完整哈希>
tier: MEDIUM
node: merged
audit: none
next: powershell -File pipeline/state.ps1
blockers: none
evidence: project-docs/evidence/REQ-LOG-007/
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
| **AUTH-002 尚未合并** | **停止**，先做 AUTH-002。两者改同一批行 |
| `.pro` 加了 `logging.cpp` 但忘记加 `HEADERS` | 编译报错，补上即可（正常） |
| `Client/Client.pro` 是否也要加 logging 文件 | 若客户端也用分类 → 加；若客户端只用单一分类 → **可以直接在 `Client/main.cpp` 里用 `qSetMessagePattern` + 一个匿名 `QLoggingCategory`**，不新建文件 |
| 某个 `qDebug` 不确定该归哪一级 | **倾向保守**：保持 `qCDebug`，不要升级成 warning（避免刷屏） |
| 改完发现日志数量明显变少 | **停下检查**：说明有日志被误删，违反 A7 |
| 某处 `qDebug` 其实在报错 | 升为 `qCWarning`，并在 task.md 记录 |
| pipeline 报 `FROZEN-PATH HIT` | 立即停止报告 |

---

## 十、完成后请回报这些

1. 最终 commit 哈希（分支 + 合并）
2. 新建的 `logging.h` / `logging.cpp` 内容
3. **日志数量改动前后对比表**（按文件）
4. §7 六项验证的实际输出（尤其 quiet vs verbose 两份日志）
5. 三项 pipeline 实际输出
6. handoff 校验脚本输出
7. 承认的不确定处（哪些日志级别判定是推测的）

**不要自行推送。**
