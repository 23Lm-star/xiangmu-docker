# 工程实施规格：分享链接安全加固 + 客户端 UI

> 批次：R18-20260729-ARC
> 架构决策：`decisions/ADR-011-20260729-低耦合心跳与应用内分享链接.md`
> 关联缺陷：`BUG-ARC-PUBLINK-001`
> 责任 Agent：项目工程师
> 范围：`BUG-ARC-PUBLINK-001` 全部修复项（安全加固 6 项 + 客户端 UI 4 项）
> 本文不是测试通过结论；完成后必须移交 Bug 查找师静态复查。

## 0. 总则（最高优先级）

- **追加优先**：新功能优先新增独立文件；既有文件只允许在下方列出的接入点做最小追加。
- **当前工作区已脏**：存在用户已有的 `.pro.user`、QSS、图片和治理文档修改。不得执行 `git reset`、`git reset --hard`、`git checkout .`、`git restore .`、整体 `cherry-pick`、整文件覆盖。
- 禁止修改或注释任何既有业务路径，特别是认证、好友分享 `SHAREFILE_*`（33~37）、`MyTcpSocket::seedMsg()`、`sendAndRelease()`、下载流、好友转发、`clientOffLine()`、PDU 的 `memfree()` 规则。
- 不新增外部依赖；Qt 5.14.2 / C++11 / qmake 是固定环境。
- 本期链接是认证后的 `cloud-disk://share/<opaque-token>` **应用内链接**，不是浏览器匿名公网 URL。

---

## 1. 执行顺序

```text
[0. 基线核验：确认当前 publish_links 表及 msghandler/operatedb 现状]
       ↓
[1. 协议扩展：两端 protocol.h 追加 54~57]
       ↓
[2. 服务端安全加固：表迁移 + 随机源 + hash + 路径校验 + 解析逻辑]
       ↓  构建 Server、自检
[3. 客户端 UI：File 右键菜单 + ShareLinkDialog + 链接查询]
       ↓  构建 Client、自检
[4. 统一文档同步]
       ↓
[移交 Bug 查找师静态审查]
```

---

## 2. 任务 A：协议扩展（两端同步）

### 2.1 追加枚举值

两端 `protocol.h` 当前最大值为 53，在末尾追加以下 4 个枚举值：

```cpp
// ===== 分享链接扩展（BUG-ARC-PUBLINK-001） =====
ENUM_MSG_TYPE_RESOLVE_PUBLINK_REQUEST = 54,      // 解析分享链接（通过 token）
ENUM_MSG_TYPE_RESOLVE_PUBLINK_RESPOND = 55,      // 解析结果
ENUM_MSG_TYPE_QUERY_FILE_PUBLINK_REQUEST = 56,   // 按文件路径查询已有链接
ENUM_MSG_TYPE_QUERY_FILE_PUBLINK_RESPOND = 57,   // 返回已有链接（或空）
```

**禁止**：不得改动 1~53 任何已有枚举值。

### 2.2 PDU 契约

| 类型 | 方向 | caData[64] | caMsg |
|------|------|-----------|-------|
| 54 RESOLVE_PUBLINK_REQUEST | C→S | 空 | token 原文（URL-safe 编码字符串） |
| 55 RESOLVE_PUBLINK_RESPOND | S→C | `bool ok`(byte 0) | 成功时：JSON `{"file_path":"...","file_size":123}` |
| 56 QUERY_FILE_PUBLINK_REQUEST | C→S | 空 | 文件相对路径（相对于当前用户根目录） |
| 57 QUERY_FILE_PUBLINK_RESPOND | S→C | `bool exists`(byte 0) | 成功时：`cloud-disk://share/<token>`；否则为空 |

接口登记到 `project-docs/interfaces.md §1.3/§1.4`。

---

## 3. 任务 B：服务端安全加固

### 3.1 数据库迁移

#### 3.1.1 新表结构

```sql
CREATE TABLE IF NOT EXISTS share_link (
  id BIGINT PRIMARY KEY AUTO_INCREMENT,
  token_hash CHAR(64) NOT NULL UNIQUE,
  owner_name VARCHAR(255) NOT NULL,
  relative_path VARCHAR(1024) NOT NULL,
  expire_time DATETIME NOT NULL,
  revoked TINYINT NOT NULL DEFAULT 0,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
);
```

#### 3.1.2 OperateDB 新增窄接口

| 方法 | 职责 | SQL 要求 |
|------|------|---------|
| `createShareLink(hash, owner, path, expire)` → bool | 插入新链接记录 | `prepare()`+`bindValue()` |
| `getShareLinkByHash(hash)` → struct | 按 hash 查链接元数据 | `prepare()`+`bindValue()` |
| `getShareLinkByPath(owner, path)` → QString | 按 owner+path 查已有 token（最新一条） | `prepare()`+`bindValue()` |

禁止修改或删除现有 `publish_links` 相关代码（`createPublishLink()` 等）— 旧实现保留但不再调用；新方法独立追加到 `operatedb.h/.cpp`。

#### 3.1.3 数据库外联规则与注销清理（⚠️ 重要）

**项目数据库不使用 FOREIGN KEY 约束**（与既有 `user_info`/`friend`/`message_info` 一致），采用事务内手动逐表清理模式。`share_link` 表与 `user_info` 的概念外联关系如下：

| 子表 | 外联列 | 父表.列 | 清理时机 |
|------|--------|---------|---------|
| `share_link` | `owner_name` | `user_info.name` | 用户注销时级联删除 |
| `publish_links`（既有） | `creator_name` | `user_info.name` | 用户注销时级联删除 ⚠️ **当前缺失** |
| `message_info`（既有） | `seed_name`/`rev_name` | `user_info.name` | ✅ 已实现（logoff 步骤 2） |
| `friend`（既有） | `name`/`friendName` | `user_info.name` | ✅ 已实现（logoff 步骤 3） |

**修复要求**：在 `operatedb.cpp::handleLogoff()` 的事务中，步骤 3（删除 friend）和步骤 4（删除 user_info）之间，插入两个新步骤：

```cpp
// 步骤 3.5 — 删除 share_link 表（新增表）
q.prepare("DELETE FROM share_link WHERE owner_name = ?");
q.addBindValue(QString::fromUtf8(name));
if(!q.exec()){
    qDebug()<<"logoff: failed to delete share_links for:"<<QString::fromUtf8(name);
    db.rollback();
    return false;
}

// 步骤 3.6 — 删除 publish_links 表（既有表，补充清理）
q.prepare("DELETE FROM publish_links WHERE creator_name = ?");
q.addBindValue(QString::fromUtf8(name));
if(!q.exec()){
    qDebug()<<"logoff: failed to delete publish_links for:"<<QString::fromUtf8(name);
    db.rollback();
    return false;
}
```

原步骤编号顺延。

**约束**：
- 禁止给任何表添加 `FOREIGN KEY` 约束（保持与现有表规则一致）
- 禁止改变 logoff 事务中既有步骤的顺序（message_info → friend → 新增 → user_info）
- publish_links 的清理是本修复中附带的既有缺陷修复（防止注销后遗留孤儿记录）

### 3.2 随机 Token 生成与哈希

```cpp
// 新增 Server/sharelinkservice.h/.cpp
class ShareLinkService {
public:
    static QString generateToken();        // 16 随机字节 → URL-safe Base64
    static QString hashToken(const QString &token); // SHA-256 → hex
    static bool validatePath(const QString &ownerRoot, const QString &relativePath); // canonical 校验
    static QDateTime defaultExpireTime();  // 当前时间 + 24h
};
```

约束：
- `QRandomGenerator::system()` 生成 16 个随机字节（128 bit）
- Base64 URL-safe 编码（`+`→`-`，`/`→`_`，去尾部 `=`）
- `QCryptographicHash::Sha256` 计算 hex digest
- `validatePath()` 使用 `QDir(ownerRoot).absoluteFilePath(relativePath)` → `QFileInfo::canonicalFilePath()`，确认在 owner root 前缀内且目标文件存在
- 原始 token 绝不写入 `qDebug()`、日志或数据库

### 3.3 消息处理

#### 3.3.1 替换创建链接逻辑

修改 `Server/msghandler.cpp` 中 `createPublishLink()`：
- 使用 `ShareLinkService::generateToken()` 替代 `qrand() % ...`
- 使用 `ShareLinkService::hashToken()` 替代明文存储
- 使用 `ShareLinkService::validatePath()` 校验路径
- 调用 `OperateDB::createShareLink()` 替代 `createPublishLink()`

#### 3.3.2 新增解析链接处理

```cpp
// Server/msghandler.cpp 新增
PDU* MsgHandler::resolvePublishLink();  // 处理 type=54
```

- 从 caMsg 读取 token
- hash 后查询 `OperateDB::getShareLinkByHash()`
- 校验未过期、未撤销
- 通过 `ShareLinkService::validatePath()` 校验文件仍存在
- 返回 JSON 格式的文件元数据

#### 3.3.3 新增链接查询处理

```cpp
// Server/msghandler.cpp 新增
PDU* MsgHandler::queryFilePublishLink();  // 处理 type=56
```

- 从 caMsg 读取 relativePath
- 调用 `OperateDB::getShareLinkByPath(owner, path)`
- 返回 `cloud-disk://share/<token>` 或空

#### 3.3.4 分发接入

`Server/mytcpsocket.cpp::handlMsg()` 追加 3 个 case：
```cpp
case ENUM_MSG_TYPE_RESOLVE_PUBLINK_REQUEST:   // 54
case ENUM_MSG_TYPE_QUERY_FILE_PUBLINK_REQUEST: // 56
case ENUM_MSG_TYPE_CREATE_PUBLINK_REQUEST:    // 52（已存在，改为调用新逻辑）
```

### 3.4 文件级修改清单（服务端）

| 文件 | 操作 | 说明 |
|------|------|------|
| `Server/protocol.h` | 修改 | 追加 54~57 |
| `Server/sharelinkservice.h/.cpp` | **新增** | token 生成/哈希/路径校验 |
| `Server/Server.pro` | 修改 | 注册 sharelinkservice |
| `Server/share_link.sql` | **新增** | 建表 SQL |
| `Server/operatedb.h/.cpp` | 修改 | 追加 3 个 share_link 窄接口 + **logoff() 补充 publish_links/share_link 清理步骤（3.5/3.6）** |
| `Server/msghandler.h/.cpp` | 修改 | 重写 createPublishLink + 新增 resolvePublishLink + queryFilePublishLink |
| `Server/mytcpsocket.cpp` | 修改 | 追加 case 54/56 |
| `Server/msghandler.h` | 修改 | 追加函数声明 |

**禁止修改**：`mytcpserver.*`、既有 `publish_links` 代码路径、下载流、好友分享。

---

## 4. 任务 C：客户端 UI

### 4.1 File 界面右键菜单

修改 `Client/file.h/.cpp`：

```cpp
// file.h 追加
private slots:
    void onFileListContextMenu(const QPoint &pos);
    void onGetShareLink();

// file.cpp 构造函数中
m_fileListWidget->setContextMenuPolicy(Qt::CustomContextMenu);
connect(m_fileListWidget, &QListWidget::customContextMenuRequested,
        this, &File::onFileListContextMenu);

void File::onFileListContextMenu(const QPoint &pos) {
    QListWidgetItem *item = m_fileListWidget->itemAt(pos);
    if (!item) return;
    // 只对普通文件（非目录）显示菜单
    if (item->data(Qt::UserRole).toString() == "dir") return;
    
    QMenu menu;
    menu.addAction("获取分享链接", this, &File::onGetShareLink);
    menu.exec(m_fileListWidget->viewport()->mapToGlobal(pos));
}
```

约束：
- 右键菜单仅对普通文件（非目录）生效
- 菜单只追加，不删除或改变现有 File 界面的任何按钮/操作/布局
- 使用 `m_fileListWidget` 的现有变量名

### 4.2 ShareLinkDialog 弹窗

新增 `Client/sharelinkdialog.h/.cpp/.ui`：

```cpp
class ShareLinkDialog : public QDialog {
    Q_OBJECT
public:
    explicit ShareLinkDialog(const QString &filePath, QWidget *parent = nullptr);
    void setLink(const QString &link);  // 设置链接文本

private:
    QLineEdit *m_linkEdit;  // 只读输入框
    QPushButton *m_copyBtn; // 复制按钮
};
```

UI 布局（`.ui`）：
- `QVBoxLayout`
  - `QLabel`：`"文件：<文件名>"`
  - `QLineEdit`：只读，placeholderText = "暂无分享链接，请先创建"
  - `QPushButton`：`"复制链接"`，点击时 `QApplication::clipboard()->setText(m_linkEdit->text())`

行为：
- 链接为空时，复制按钮 disabled
- 链接非空时，复制按钮 enabled，点击复制到剪贴板并弹出 `QToolTip` 提示"已复制"

### 4.3 链接查询流程

```text
File::onGetShareLink()
    → 获取当前选中文件的 relativePath
    → 构造 PDU(type=56)，caMsg = relativePath
    → Client::seedMsg(pdu)
    → 服务端返回 type=57
    → Client::handlMsg() case 57 → MsgHandler::handleQueryFilePublishLink()
    → 解析 caData[0] (bool exists) + caMsg (token)
    → 构造 ShareLinkDialog，setLink(link)
    → dialog.exec()
```

### 4.4 消息分发接入

`Client/client.cpp::handlMsg()` 追加 case：
```cpp
case ENUM_MSG_TYPE_QUERY_FILE_PUBLINK_RESPOND:  // 57
```

`Client/msghandler.h/.cpp` 追加处理函数：
```cpp
void MsgHandler::handleQueryFilePublishLink(PDU *&pdu);
```

### 4.5 文件级修改清单（客户端）

| 文件 | 操作 | 说明 |
|------|------|------|
| `Client/protocol.h` | 修改 | 追加 54~57 |
| `Client/file.h/.cpp` | 修改 | 右键菜单 + onGetShareLink() |
| `Client/sharelinkdialog.h/.cpp/.ui` | **新增** | 链接展示弹窗 |
| `Client/Client.pro` | 修改 | 注册 sharelinkdialog |
| `Client/msghandler.h/.cpp` | 修改 | 追加 handleQueryFilePublishLink() |
| `Client/client.cpp` | 修改 | handlMsg() 追加 case 57 |

**禁止修改**：`sharefile.*`、`file.ui` 布局、既有下载/上传/删除/重命名逻辑。

---

## 5. 文件级禁止清单

- 禁止编辑：`Client/sharefile.*`、`Server/mytcpserver.*`、`Client/uploader.*`
- 禁止改动：`MyTcpSocket::seedMsg()`、`sendAndRelease()`、`startDownloadStream()`、`sendNextChunk()`、`clientOffLine()` 的既有内容
- 禁止修改任何现有 PDU 值（1~53）、`PDU` 结构、内存池接口、`friend`/`user_info`/`message_info` 表
- 禁止直接删除 `publish_links` 相关代码（旧实现保留不调用即可）
- 禁止在对话框或日志中打印完整 token
- 禁止新增 HTTP 服务、浏览器路由、外部库

---

## 6. 工程师交付内容

移交 Bug 查找师时，必须附：

1. 协作总则要求的结构化"变更摘要"
2. 每个新增/修改文件清单和"未修改但受影响文件"
3. 两端 qmake 构建输出（真实命令和结果）
4. 服务端自检证据：
   - 创建链接：返回 `cloud-disk://share/<token>` 格式
   - 解析链接：token 正确时返回文件元数据 JSON
   - 查询链接：按文件路径返回已有 token
   - 路径校验：越权路径被拒绝
   - 过期链接：过期后解析返回失败
5. 客户端自检证据：
   - File 界面右键菜单对普通文件出现，对目录不出现
   - 点击"获取分享链接"弹窗正确展示
   - 无链接时显示空白 + 复制按钮 disabled
   - 有链接时可复制，剪贴板内容正确
6. `BUG-ARC-PUBLINK-001` 状态由"已确认，待修复"更新为"修复中"
7. 不得宣称测试通过或关闭任何缺陷
