# BUG-REV-AUTH-004

- **缺陷编号**: BUG-REV-AUTH-004
- **缺陷标题**: `MsgHandler::logoff()` 中 `char[32]` 无 null 终止符导致密码越界读取
- **严重等级**: 严重
- **涉及模块**: Auth / `Server/msghandler.cpp`
- **触发条件**: 客户端发送 `ENUM_MSG_TYPE_LOGOFF_REQUEST` 请求时
- **问题说明**: 在初始实现的注销功能中，使用 `memcpy` 从 `pdu->caData` 提取 32 字节的用户名和密码到栈数组 `char userName[32]` 和 `char userPwd[32]` 中。由于没有预留空间存放空终止符 (`\0`)，随后调用 `QString::fromUtf8(userName)` 或传递给 `OperateDB::instance().handleLogoff` 时会导致越界读取，将相邻的内存数据（例如密码的部分或脏数据）粘连在字符串末尾。
- **预期行为**: 用户名和密码字符串应被正确提取，无越界数据。
- **实际行为**: 发生越界读取，可能导致数据库校验失败或记录错误日志。
- **判断依据**: C 语言字符串 API 和 Qt 字符串转换 API 均需要空终止符才能正确界定字符串边界。
- **修正建议**: 使用预设的安全字符串提取函数（例如 `fixedString()`）替代 `memcpy()` 进行定长字符串解析。
- **建议验证方式**: 审查 `MsgHandler::logoff()` 代码确认使用了 `fixedString()`；构建 Server 执行注销操作，检查日志输出的用户名和密码是否没有乱码粘连。
- **当前状态**: 已修复