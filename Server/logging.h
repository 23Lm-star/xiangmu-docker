#ifndef LOGGING_H
#define LOGGING_H

#include <QLoggingCategory>

// 决策 D1：C++ 标识符用 lcXxx，避免与 operatedb/databaseconnectionpool 局部变量 db 冲突；
// 显示名（规则串里用的）保持 xmu.*，与需求文档一致。
Q_DECLARE_LOGGING_CATEGORY(lcNet)   // 网络层：连接、收发、协议、好友/聊天转发
Q_DECLARE_LOGGING_CATEGORY(lcDb)    // 数据库层：SQL、连接池
Q_DECLARE_LOGGING_CATEGORY(lcFs)    // 文件系统：上传/下载/删除/重命名/分享链接
Q_DECLARE_LOGGING_CATEGORY(lcAuth)  // 认证：登录/注册/注销/验证码
Q_DECLARE_LOGGING_CATEGORY(lcApp)   // 应用生命周期：配置、启动

#endif // LOGGING_H
