#ifndef CLI_LOGGING_H
#define CLI_LOGGING_H

#include <QLoggingCategory>

// 决策 D3：客户端单分类，与 Server 对称建文件并登记 Client.pro
Q_DECLARE_LOGGING_CATEGORY(lcCli)   // 客户端全部日志

#endif // CLI_LOGGING_H
