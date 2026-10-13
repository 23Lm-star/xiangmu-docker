# REQ-AUTH-002　密码加盐 + 清除认证日志泄露

| 项目 | 值 |
|---|---|
| **需求编号** | REQ-AUTH-002 |
| **分支名** | `fix/auth-security-baseline` |
| **任务等级** | **MEDIUM** |
| **基线 commit** | `b5d7bb3`（本文档编写时的 master HEAD） |
| **流程版本** | Development OS v1.7 |
| **状态** | 待执行 |

---

## 一、背景（为什么要做）

两个独立的安全缺陷，因为**改同一段代码**（`handleLogin`）所以合并为一个需求做。拆开做必然产生合并冲突。

### 缺陷 1：密码哈希无盐（P1-6）

`Server/operatedb.cpp` 直接把客户端传来的哈希存进库。客户端 `Client/client.cpp:23` 算的是**裸 MD5**：

```cpp
QByteArray passwordHashForProtocol(const QString &password)
{
    return QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Md5).toHex();
}
```

MD5 是**无盐**且**计算极快**的，等于把密码明文等价物交给攻击者：`123456` 的 MD5 是 `e10adc3949ba59abbe56e057f20f883e`，这是彩虹表第一条命中项。项目自带 `project-docs/init_test_data.sql` 就用了这个值当测试数据，实际证明了这一点。

### 缺陷 2：密码写进日志（P0-3）

7 处排查期遗留的 DIAG 日志把凭据打进日志文件：

| 文件 | 行 | 泄露内容 |
|---|---|---|
| `Server/msghandler.cpp` | 222 | 用户名 + `name_hex` |
| `Server/msghandler.cpp` | 223 | **密码哈希**（`pwd_hex`） |
| `Server/msghandler.cpp` | 224 | **整个 64 字节凭据区**（`caData raw hex`，含用户名+密码） |
| `Server/operatedb.cpp` | 229 | 用户名 + `name_hex` |
| `Server/operatedb.cpp` | 230 | **密码哈希**（`pwd_hex`） |
| `Server/operatedb.cpp` | 237 | **数据库里存的密码哈希**（`pwd` + `HEX(pwd)`） |
| `Server/operatedb.cpp` | 239 | 用户名（辅助诊断行） |

日志常被采集、上报、留存，等于凭据二次泄露。

---

## 二、目标（验收标准）

用业务语言描述，完成后逐条核对：

| # | 验收标准 | 怎么验证 |
|---|---|---|
| A1 | 新注册的用户，数据库里存的**不是**裸 MD5，而是加盐后的值 | 注册 `alice/123456`，查库确认 `pwd != e10adc3949ba59abbe56e057f20f883e` |
| A2 | 加盐对用户完全无感：注册后能正常登录 | 注册 → 登录，成功 |
| A3 | **老用户仍能登录**（向后兼容，不能把现有账号锁死） | 用 `init_test_data.sql` 里的 `tester/123456` 登录，成功 |
| A4 | 老用户登录后，其密码哈希**自动升级**为加盐版本 | 老用户登录一次后查库，`salt` 字段非空 |
| A5 | 注销账户功能不受影响（它也校验密码） | 登录新账号 → 注销 → 成功 |
| A6 | 服务端日志中**不再出现任何密码相关的值** | 全局搜索 `pwd_hex`、`caData raw hex` 无残留；启动服务端登录一次，看日志无密码 |
| A7 | 三项 pipeline 全绿 | `build.ps1` GREEN、`test.ps1` GREEN、`audit.ps1` PASS |

---

## 三、技术方案（已定型，不要自行改动思路）

### 3.1 加盐方案 —— 为什么是这个方案

**核心约束：不能改协议。** `Server/protocol.h` 是 frozen_path（冻结路径），改它会触发 LARGE 流程 + 需要人工批准。而协议里密码字段是固定的 32 字节（`caData[64]` = 用户名 32 + 密码哈希 32）。

所以：

```
现状：pwd = MD5(明文)                    → 32 位 hex
改为：pwd = MD5( MD5(明文) + salt )      → 仍是 32 位 hex  ✅ 长度不变
```

**服务端加盐，客户端完全不动。** 客户端继续传裸 MD5，服务端拿到后再叠一层盐。

> ⚠️ **明确禁止改用 SHA256。** SHA256 hex 是 64 字符，会超出 VARCHAR(32) 和协议 32 字节字段，**必须改 `protocol.h`**，那是冻结路径。
> 如果执行中发现必须改协议，**立即停止，不要自行修改**，把问题报给主人升级为 LARGE。

### 3.2 数据库改动

```sql
ALTER TABLE user_info ADD COLUMN salt VARCHAR(32) NOT NULL DEFAULT '';
```

- `salt = ''`（空字符串）表示**这是老数据，按旧逻辑比对**
- 迁移文件放在 `Server/migrate_add_password_salt.sql`（参照已有的 `migrate_friend_to_json.sql` 命名风格）

### 3.3 三个入口都要改（⚠️ 最容易漏的地方）

密码哈希在本项目里被用于**三个**功能，全部要处理：

| # | 功能 | 服务端函数 | 客户端位置 |
|---|---|---|---|
| 1 | 注册 | `OperateDB::handleRegit`（`operatedb.cpp:42`） | `client.cpp:351` |
| 2 | 登录 | `OperateDB::handleLogin`（`operatedb.cpp:214`） | `client.cpp:377` |
| 3 | **注销账户** | `OperateDB::handleLogoff`（`operatedb.cpp`） | `client.cpp:458` |

**注销这条最容易漏**，它对密码做了**两处**校验（同一个函数里）：
- 第 1 步：`SELECT * FROM user_info WHERE name = ? AND pwd = ?`
- 第 4 步：`DELETE FROM user_info WHERE name = ? AND pwd = ?`

**这两处都必须改成"先取 salt 再算加盐哈希比对"**，否则注销功能会直接坏掉（用户无法注销，或更糟——删错记录）。

### 3.4 具体改法

**新增盐生成函数**（建议放在 `OperateDB` 内，或 `operatedb.cpp` 的匿名 namespace）：

```cpp
// 生成 32 位 hex 随机盐
static QString generateSalt()
{
    return QString::fromLatin1(
        QCryptographicHash::hash(
            QByteArray::number(QDateTime::currentMSecsSinceEpoch())
                + QByteArray::number(QRandomGenerator::global()->generate64()),
            QCryptographicHash::Sha256).toHex().left(32));
}
```

> 用时间戳 + 随机数做盐的熵源。注意 `operatedb.cpp` 需要 `#include <QRandomGenerator>` 和 `#include <QDateTime>`。

**新增加盐哈希函数**：

```cpp
static QString applySalt(const QString &clientHash, const QString &salt)
{
    if (salt.isEmpty()) return clientHash;   // 老数据：不加盐，保持原值比对
    return QString::fromLatin1(
        QCryptographicHash::hash((clientHash + salt).toUtf8(),
                                 QCryptographicHash::Md5).toHex());
}
```

**注册（`handleRegit`）**：
1. 生成 `salt`
2. 存 `applySalt(客户端传来的哈希, salt)` 到 `pwd`
3. 存 `salt` 到新字段

**登录（`handleLogin`）**：
1. 先 `SELECT pwd, salt FROM user_info WHERE name = ?`（按用户名取）
2. 用取到的 `salt` 算 `applySalt(客户端哈希, salt)`
3. 用算出的值做比对
4. **若 `salt` 为空**（老用户）→ 按旧逻辑直接用客户端哈希比对；**比对成功后立刻补写 salt**（这就是验收 A4 的自动升级）

> ⚠️ 登录函数里现有的 DIAG 诊断块（第 229-239 行那个"先查用户是否存在"的 `QSqlQuery check`）**整个删掉**，它正是泄露来源。

**注销（`handleLogoff`）**：两处密码比对同样改为"取 salt → 算加盐哈希 → 比对"。

**已核实：本项目没有改密功能。** 已全局搜索 `CHANGEPWD` / `change_pwd` / `modifypwd` / `resetpwd` / `修改密码`、以及 `protocol.h` 的消息类型枚举，均无结果。**不要新建改密功能**——那是扩大范围。

---

## 四、允许改动的文件（allowed paths）

只许改这些，**多改一个文件都算越界**：

```
Server/operatedb.cpp              ← 主要改动（register/login/logoff + 加盐函数）
Server/operatedb.h                ← 仅当需要新增函数声明
Server/msghandler.cpp             ← 仅删除第 222-224 行 DIAG 日志
Server/migrate_add_password_salt.sql   ← 新建（迁移文件）
project-docs/tasks/REQ-AUTH-002/task.md  ← 任务记录
project-docs/evidence/REQ-AUTH-002/      ← 证据目录
```

---

## 五、禁止事项（⚠️ 违反即停止并上报）

| # | 禁止 | 原因 |
|---|---|---|
| 1 | **禁止修改 `Server/protocol.h` 或 `Client/protocol.h`** | frozen_path 冻结路径。改了会触发 LARGE 流程。发现必须改 → **停下上报** |
| 2 | **禁止把客户端改成传 SHA256 或明文** | 客户端必须继续传裸 MD5，加盐只发生在服务端。见 §3.1 |
| 3 | **禁止修改 `Client/client.cpp` 的哈希逻辑** | 本需求不动客户端代码。客户端 `passwordHashForProtocol` 保持原样 |
| 4 | **禁止顺手改其他缺陷** | 本需求只做加盐 + 清日志。路径校验、日志分级等**各自有独立分支**，不在本分支做 |
| 5 | **禁止大规模格式化代码** | 不做全文缩进调整、不做行尾转换、不重排 include。改动要能一眼看清 |
| 6 | **禁止删除或弱化任何功能** | 尤其：注销、登录、注册三个功能必须都还能用 |
| 7 | **禁止把老用户锁死** | `salt=''` 的老数据必须仍能登录（验收 A3）。若实现后发现老用户登录失败，是 bug 不是"预期行为" |
| 8 | **禁止自行决定档位升降** | 发现需要升 LARGE → 停下报告，由主人决定 |
| 9 | **禁止在 `git push` 前合并** | 本地完成、pipeline 全绿后交给主人，**推送由主人批准**（不可逆操作） |

---

## 六、执行流程（Development OS v1.7）

严格按这个顺序，不要跳步：

```bash
# ① 会话开场（唯一允许的开场动作）
powershell -File pipeline/state.ps1
#    应显示：branch: master   suspended git state: none
#    如果显示有未提交的改动 → 先停下报告

# ② 从 master 切分支
git switch master
git switch -c fix/auth-security-baseline

# ③ 写代码（只改 §4 列出的文件）

# ④ 跑门 —— 注意顺序！先 test 后 audit
powershell -File pipeline/build.ps1     # 必须 GREEN
powershell -File pipeline/test.ps1      # 必须 GREEN
powershell -File pipeline/audit.ps1     # 必须 PASS
#    ↑ audit 首次可能报 RED "NO test evidence" —— 正常，先跑 test 就有了

# ⑤ 人工验证（pipeline 测不到的部分，见 §7）
#    用 MySQL 客户端实际操作数据库，逐条核对验收 A1-A5

# ⑥ 提交
git add Server/operatedb.cpp Server/operatedb.h Server/msghandler.cpp Server/migrate_add_password_salt.sql
git commit -m "fix(security): salt password hashes and remove credential logging"

# ⑦ 合并回 master（no-ff，保留分支痕迹）
git switch master
git merge --no-ff fix/auth-security-baseline -m "merge: REQ-AUTH-002 password salt (MEDIUM)"
git branch -d fix/auth-security-baseline

# ⑧ 停下，交给主人决定是否推送。不要自行 push。
```

---

## 七、需要的验证证据

pipeline 只能证明"能编译、没破坏冻结路径"，**证明不了加盐真的生效**。所以要人工验证并把结果写进证据目录。

### 必须跑的人工验证

```sql
-- 验证 A1：新注册用户存的是加盐值
SELECT name, pwd, salt FROM user_info WHERE name = 'alice';
-- 预期：salt 非空（32 位 hex）；pwd != 'e10adc3949ba59abbe56e057f20f883e'

-- 验证 A4：老用户登录后自动补盐
SELECT name, pwd, salt FROM user_info WHERE name = 'tester';
-- 首次应为 salt='' ；登录一次后再查，salt 应非空
```

### 证据目录（按 v1.7 布局）

```
build/evidence/fix-auth-security-baseline/<commit-hash>/
    pipeline-run.log     # test/audit 输出
    manual-verify.log    # 上面 SQL 的实际输出（脱敏：不要贴真实密码）
    manifest.txt         # 由 record-evidence.py 生成
```

生成证据清单：
```bash
python "C:/Users/王晨扬/.codex/skills/development-os-v1-7/scripts/record-evidence.py" \
  --evidence-dir build/evidence/fix-auth-security-baseline/<hash> \
  --repo E:/qt/xiangmu \
  --label "REQ-AUTH-002: salt applied, credentials no longer logged" \
  Server/operatedb.cpp Server/msghandler.cpp
```

---

## 八、任务记录与 handoff（必须产出）

### 8.1 任务记录 `project-docs/tasks/REQ-AUTH-002/task.md`

必须含：档位 + 依据、验收标准、allowed paths、决策日志（append-only）。

### 8.2 会话收尾 handoff

路径 `project-docs/handoffs/fix-auth-security-baseline-<日期>.txt`，**严格 10 行**：

```text
# handoff: REQ-AUTH-002
branch: master
head: <合并后的 commit 完整哈希>
tier: MEDIUM
node: merged
audit: none
next: powershell -File pipeline/state.ps1
blockers: none
evidence: project-docs/evidence/REQ-AUTH-002/
owner: claude-code
date: <YYYY-MM-DD>
```

写完**必须**用脚本校验格式：
```bash
python "C:/Users/王晨扬/.codex/skills/development-os-v1-7/scripts/validate-handoff.py" <handoff 文件路径>
# 期望输出：OK: handoff valid (10 lines, tier=MEDIUM, node=...)
```

---

## 九、遇到的边界情况怎么办

| 情况 | 怎么办 |
|---|---|
| 发现 `pwd` 字段在其他地方也被读写（比如某个 SQL 或脚本） | **先报告**，不要自行扩大改动范围。可能需要另开需求 |
| 想找改密功能 | **已核实不存在**，不要找、不要建 |
| 老用户登录失败 | 是 bug。检查 `salt=''` 的分支是否走了旧逻辑 |
| 需要改 `protocol.h` | **立即停止**，报告主人。这是冻结路径，需升级 LARGE |
| pipeline 报 `FROZEN-PATH HIT` | 同上，立即停止报告 |
| 改完发现影响面比预想大（比如冒出一堆相关文件） | **停止**，报告"这个需求该拆"，由主人决定 |

---

## 十、完成后请回报这些

1. 最终 commit 哈希（分支提交 + 合并提交）
2. 三项 pipeline 的实际输出（GREEN / GREEN / PASS）
3. 人工验证 A1–A5 的实际结果
4. handoff 校验脚本的输出
5. 遇到的任何偏差或未预期情况

**不要自行推送。** 推送属不可逆操作，等主人批准。
