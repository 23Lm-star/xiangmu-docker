# REQ-UX-001 需求分析与缺陷定因

> 全部结论基于 master@53b048b 源码事实，非推测。行号即该 commit 的行号。

## 1. 在线用户条目点击无反应（含双击无弹窗）

**结论：交互面缺失 + 成功路径静默。**

- 列表项文本由 `Client/msghandler.cpp:120` 生成为 `"<序号>.<用户名>"`（如 `1.alice`）。
- 唯一的交互槽是 `Client/onlineuser.cpp:24 on_listWidget_itemDoubleClicked`：
  - 发送 `ENUM_MSG_TYPE_ADDFRIEND_REQUEST`（`onlineuser.cpp:34-47`）；
  - 槽在 `onlineuser.h:20-21` 已正确声明为 `private slots`，`setupUi` 的 `connectSlotsByName` 能自动连上，**双击信号本身是通的**。
- 真正的"无反应"来源在客户端响应处理 `Client/msghandler.cpp:125-139 handlAddFriend`：
  - `ret==1`（添加成功）→ 只调用 `flushfriend()` 刷新好友列表，**没有任何弹窗**；
  - `ret==-2` / 其他 → 才弹 `QMessageBox`。
  - 所以成功时用户看不到任何反馈；若用户正处于"在线用户"子窗口之上，好友列表刷新也不可见 → 表现为"点了没反应"。
- 次要脆弱点：用户名解析用"第一个 `.` 之后"（`onlineuser.cpp:27-32`），用户名含 `.` 会解析错；且无任何"单击选中/提示"反馈。

## 2. 用户文件夹空白 + 服务端"拒绝越权列目录"

**结论：服务端只在注册时建用户目录，登录不建；沙箱校验要求路径物理存在 → 存量账号在新部署下无个人目录 → 自身根目录被误判越权。**

- 目录创建只发生在注册：`Server/msghandler.cpp:260-266`（`rigist()` 成功后 `mkdir(filesPath)`、`mkdir(filesPath/<用户名>)`）。**登录路径不创建**（`Server/msghandler.cpp:279 login()`）。
- 列目录门：`Server/msghandler.cpp:649-659 flushfile()` → `pathInActorRoot(path)`；
  - `pathInActorRoot`（`:167-174`）→ `isPathInSandbox(path, actorUserRoot())`；
  - `isPathInSandbox`（`:490-508`）对目标取 `canonicalFilePath()`，**为空即返回 false**（`:501-502`）。即"路径必须物理存在"。
- 实测：`dist/filesys/` 下**没有 `wcy` 目录**（`ls dist/filesys` 无该项；`Server/filesys/` 下的 `wcy` 是 9/13 旧构建残留）。用户账号在共享 DB 中已存在（登录成功、连接已认证 conn=2），但当前部署的 `filesPath` 下没有该账号目录 →
  `canonicalFilePath("filesPath/wcy")` 为空 → `pathInActorRoot=false` → `flushfile` 打 `拒绝越权列目录` 并回空列表 → 文件页空白。
- 放大影响：`MKDIR`（`:521`）、`RENAME`（`:713`）、`MOVEFILE`（`:880/960`）等同走该门，**在缺目录时全部被拒**，用户根本无法自助修复 → 文件功能整体锁死。
- 客户端 `Client/file.cpp:70-76` 在**客户端本地** `mkdir` 用户目录，对服务端沙箱毫无作用（且要求客户端 cwd 与 filesPath 同布局），不能作为修复。

## 3. 群聊展示混乱

**结论：用单行纯文本堆进 QListWidget，无排版语义；状态栏直接吐协议术语。**

- `Client/groupchat.cpp:15-36` 构造：`QListWidget` + 单行 `QLineEdit` + 状态 `QLabel`，无气泡/分栏/换行。
- 渲染：`insertMessage`（`:96-117`）把消息压成一行 `"[HH:mm:ss] 发送者: 内容"`，系统消息压成 `"—— 内容 ——"`；`QListWidget` 默认不换行，长消息横向溢出。
- 状态栏 `updateStatus`（`:168-172`）直接显示 `"群水位 %1 · 服务端已读 %2 · 本地未读 %3"`——普通用户无法理解的协议术语。
- 乐观发送态 `（发送中…/已发送/已送达/发送失败，双击重试）` 混在正文里（`:129/220/278/258`），与真实消息无法区分。
- 叠加第 4 条主题缺陷（浅底浅字），整体不可读。

## 4. 字体看不清（浅底浅字）

**结论：全局 QSS 只给两个窗口设了背景，其余顶层窗口用 Qt 默认浅背景，却被全局规则染成浅色字。**

- `Client/main.cpp:18-25` 把 `:/style.qss` 设为 **application 级**样式表（对全部窗口生效）。
- `Client/style.qss:7-11` 全局 `QWidget { color: #e0e8f0; }`（近白）。
- 但背景只设了两处：`QWidget#Client`（`:14-18`）与 `QWidget#Index`（`:19-23`）。
- 其余顶层窗口——`OnlineUser`、`GroupChat`、`GroupFile`、`Chat`、`ShareFile`、`ShareLinkDialog`、`FolderSelectDialog`、`UserInformation`、`ReconnectDialog`、`TransferTask`——没有背景规则，落到 Qt 默认（浅色）→ **近白字 + 浅底 ≈ 看不见**。

## 5. 界面/字号太小

**结论：各窗口尺寸分散硬编码，QSS 字号偏小。**

- 硬编码尺寸：`Client/client.cpp:59` 600×420、`Client/index.cpp:11` 1000×650、`Client/groupchat.cpp:22` 560×480、`Client/folderselectdialog.cpp:15` 420×360、`Client/reconnectdialog.cpp:19` 320×180、`Client/groupfile.cpp:45` 620×420、`Client/transfertask.cpp:827` 680×360、`Client/simplecaptcha.cpp:20` 120×40。
- QSS 字号 13–24px（`style.qss:10/33/44/79/157/206`）。

---

## 缺陷分组与修复归属

| 缺陷 | 类型 | 主要文件 | 修复方向 |
|---|---|---|---|
| 1 在线用户点击 | 功能交互缺失 | `Client/onlineuser.cpp`(+`Client/msghandler.cpp` 反馈) | 单击反馈 + 双击确认/结果弹窗 |
| 2 用户目录空白 | 服务端逻辑缺陷（阻塞级） | `Server/mytcpsocket.cpp` 登录成功分支 | 登录幂等确保 `filesPath/<actor>` 存在 |
| 3 群聊混乱 | 展示实现缺陷 | `Client/groupchat.cpp` | 可读排版（分栏/换行/去术语） |
| 4 浅底浅字 | 全局样式缺陷 | `Client/style.qss` | 统一背景 + 白字 |
| 5 尺寸太小 | 视觉规格 | 各窗口 `.cpp` + `style.qss` | 长宽各 ×2、字号放大 |
