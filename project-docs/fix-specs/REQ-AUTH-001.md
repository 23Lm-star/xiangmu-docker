# 需求规格：REQ-AUTH-001 用户安全注销账户功能

## 1. 目标
在 Client-Server 架构下实现“注销账户（删除当前账户）”功能，使得用户能永久删除其账号，以及关联的好友关系和个人文件，解决此前因缺少该功能导致 `friend` 和 `message_info` 表数据在理论上不一致的遗留风险（原 BUG-DB-004 外键接受风险）。

## 2. 功能要求

### 2.1 协议定义
已在 `project-docs/interfaces.md` 和 `protocol.h` 中定义了枚举值：
- `ENUM_MSG_TYPE_LOGOFF_REQUEST = 46`
- `ENUM_MSG_TYPE_LOGOFF_RESPOND = 47`

请求 PDU：
- `uiType`: `ENUM_MSG_TYPE_LOGOFF_REQUEST`
- `caData[0~31]`: 用户名 (UTF-8)
- `caData[32~63]`: 验证的密码 (MD5哈希)

响应 PDU：
- `uiType`: `ENUM_MSG_TYPE_LOGOFF_RESPOND`
- `caData[0~3]`: `int` (表示是否成功)
  - 1: 成功
  - 0: 密码错误或用户不存在

### 2.2 客户端 (Client)
- 在主界面 (Index) 或设置/个人信息中增加【注销账号】入口（UI可按具体位置安排，建议放个人信息内或好友列表边侧）。
- 点击注销需要弹出确认框，并要求用户重新输入密码确认（防误操作）。
- 向服务端发送 `LOGOFF_REQUEST`，发送后等待响应。
- 收到 `LOGOFF_RESPOND` 后：
  - 成功：提示用户注销成功，返回登录界面（或直接退出程序）。
  - 失败：提示密码错误。

### 2.3 服务端 (Server)
`MsgHandler` 增加注销请求处理逻辑：
- 调用 `OperateDB::handleLogoff(const char* name, const char* pwd)`
- 在数据库中需保证事务一致性，以下操作要么全部成功，要么全部失败（回滚）：
  1. 验证用户名和密码是否匹配。
  2. 若匹配，删除 `user_info` 表中该用户记录。
  3. 删除 `friend` 表中 `name` 或 `friendName` 等于该用户名的所有记录。
  4. 删除 `message_info` 中相关聊天记录。
- 除了数据库清理，如果该用户在服务器上有私有文件夹，需要调用递归删除（类似 `MsgHandler::delfile` 或递归版）将用户的根目录及其所有文件永久删除，释放空间。
- 清理所有在线数据结构：若该用户还有其他 Session（断开 TCP 并从在线名单踢出，此步可复用当前 offline 的处理）。

## 3. 验收标准
1. 通过客户端可以正常提交密码和注销请求。
2. 数据库 `user_info`、`friend` 中彻底没有关于此用户名的残余数据。
3. 服务端的对应个人用户文件夹被物理删除。
4. 操作完成后，不能使用原账号密码登录。

## 4. 实施指引
- 数据库操作请放在 `OperateDB` 中以单事务执行。
- 请使用 `QDir::removeRecursively()`（或已有递归删除）处理本地文件夹。
- 本任务不需要建立 MySQL 物理级联外键（因为要同时清理磁盘文件，必须在业务层做综合事务），应用层保证删除干净即可。