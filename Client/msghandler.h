#ifndef MSGHANDLER_H
#define MSGHANDLER_H

#include "protocol.h"
#include <QString>
#include <QFile>

class MsgHandler
{
public:
    PDU* pdu;
    MsgHandler();
    void handlrigist();
    // REQ-EXT-TRANSFER-003 R-1：登录成功时**同步**返回登录名（失败/异常返回空串）。
    // 调用方必须把它写进 Client::m_LoginName 之后再做任何依赖用户名的动作——
    // 1.5 s 后的 UI 定时器不再承担名字写入。
    QString handllogin();
    void handlfinduser();
    void handlonlineuser();
    void handlAddFriend();
    void handlAddFriendAgree(const QString& myName);
    void handlFlushFriend();
    void handlDeleteFriend();
    void chat();
    void getcharmsg();
    void mkdir();
    void deldir();
    void changedir();
    void renamedir();
    void sharefileagree();
    void sharefilefinsh();
    void flushfile();
    void handleCaptcha();
    void handlMoveFile();
    void handlUserInformation();
    void handlLogoff();
    void handleQueryFilePublishLink(PDU* pdu);
    void handleCreatePublishLink(PDU* pdu);
    void handleResolvePublishLink(PDU* pdu);
    // REQ-EXT-TRANSFER-001：30/32/41/42/43/102/106/108 已改由 TransferManager 按 transfer_id 路由，
    // 本类原有的单槽上传/下载接收状态已删除。
};

#endif // MSGHANDLER_H
