#include "logging.h"
#include "client.h"
#include "msghandler.h"
#include <QString>
#include <QMessageBox>
#include <qmessagebox.h>
#include <qdir.h>
#include <QDate>
#include <QFile>
#include <QFileInfo>
#include <QTimer>
#include "index.h"
#include "userinformation.h"
MsgHandler::MsgHandler()
{
}

void MsgHandler::handlrigist(){
    int ret;
    memcpy(&ret,this->pdu->caData,sizeof(ret));
    qCDebug(lcCli)<<"rigist ret"<<ret<<endl;
    char rigistName[32]={'\0'};
    memcpy(rigistName,this->pdu->caData+32,32);
    qCDebug(lcCli)<<QString(rigistName)<<endl;
    if(ret==-2){
        QMessageBox::information(&Client::getinstance(),"提示","验证码错误，请重新输入");
        return;
    }
    if(ret==-3){
        QMessageBox::information(&Client::getinstance(),"提示","验证码已过期，请重新获取");
        return;
    }
    if(ret==-4){
        int rem=0;
        memcpy(&rem,this->pdu->caData+4,sizeof(rem));
        // 触发客户端 10 分钟锁定：获取验证码按钮进入倒计时，注册/登录被客户端拦截
        Client::getinstance().startCaptchaCooldown(rem > 0 ? rem : 600, true);
        QMessageBox::information(&Client::getinstance(),"提示",
            QString("验证码错误次数过多，账号已锁定，请 %1 分钟后再试").arg(qMax(1,rem/60)));
        return;
    }
    if(ret){
        Client::getinstance().showLoadingAnimation(true);
        QString name=QString(rigistName);
        QTimer::singleShot(1500,[name](){
            QMessageBox::information(&Client::getinstance(),"提示","注册成功");
            Client::getinstance().flus();
            QDir dir;
            dir.mkdir(QString("%1").arg(Client::getinstance().m_strRootPath));
            dir.mkdir(QString("%1/%2").arg(Client::getinstance().m_strRootPath).arg(name));
        });
    }
    else {
        Client::getinstance().showLoadingAnimation(false);
    }
}

QString MsgHandler::handllogin()
{
    int ret;
    memcpy(&ret,pdu->caData,sizeof(ret));
    if(ret==-2){
        QMessageBox::information(&Client::getinstance(),"提示","验证码错误，请重新输入");
        return QString();
    }
    if(ret==-3){
        QMessageBox::information(&Client::getinstance(),"提示","验证码已过期，请重新获取");
        return QString();
    }
    if(ret==-4){
        int rem=0;
        memcpy(&rem,pdu->caData+4,sizeof(rem));
        // 触发客户端 10 分钟锁定：获取验证码按钮进入倒计时，注册/登录被客户端拦截
        Client::getinstance().startCaptchaCooldown(rem > 0 ? rem : 600, true);
        QMessageBox::information(&Client::getinstance(),"提示",
            QString("验证码错误次数过多，账号已锁定，请 %1 分钟后再试").arg(qMax(1,rem/60)));
        return QString();
    }
    if(ret==1){
        Client::getinstance().showLoadingAnimation(true);
        char lName[32]={'\0'};
        memcpy(lName,pdu->caData+32,sizeof(lName));
        QString loginName=QString(lName);
        // R-1：名字同步返回给调用方（Client::handlMsg 的 LOGIN_RESPOND 分支先落 m_LoginName
        // 再 setCurrentUser）；本定时器此后只负责 UI 切换与既有延迟刷新。
        QTimer::singleShot(1500,[loginName](){
            qCDebug(lcCli)<<"登录账号成功，用户名为:"<<loginName;
            // REQ-EXT-GROUP-004：新会话丢弃上一会话的群读状态（换账号登录不得继承旧游标）
            Client::getinstance().resetGroupStates();
            // 文件页的用户根按本次登录身份重算——重连路径下 File 可能先于登录被构造，
            // 那次构造拿到的空登录名会让文件页停在存储根目录
            Index::getinstance().getFile()->resetUserPath();
            Index::getinstance().show();
            Client::getinstance().hide();
            // 主界面完全构造并显示后，在事件循环下一轮触发初始刷新（仅好友列表）
            QTimer::singleShot(0, [](){
                Index::getinstance().getFriend()->flushfriend();
                // REQ-EXT-GROUP-004：这里是全客户端唯一的"会话已建立"时点，一次取回全部群的
                // 未读锚点（群水位 + 我的已读游标）。不能再早——actor 未建立，摘要必被拒；
                // 不能再晚——等到第一次打开群聊窗口才取，会让没打开过的群一直空着。
                Index::getinstance().getGroup()->initializeSession();
                // 文件列表刷新延迟到用户首次点击"文件"页时
            });
        });
        return loginName;
    }
    else if(ret==-1){
        Client::getinstance().showLoadingAnimation(false);
        QTimer::singleShot(1500,[](){
            QMessageBox::information(&Client::getinstance(),"提示","用户或密码其中一个错误");
        });
    }
    else{
        Client::getinstance().showLoadingAnimation(false);
        QTimer::singleShot(1500,[](){
            QMessageBox::information(&Client::getinstance(),"提示","状态异常");
        });
    }
    return QString();
}

void MsgHandler::handlonlineuser(){
      //取出所有在线用户的用户名
      int leng=pdu->uiMsgLen/32;//柔性数组长度
      QStringList userNamelist;
      for(int i=0;i<leng;i++){
          char userName[33]={'\0'};
          memcpy(userName,pdu->caMsg+i*32,32);//取出每一个用户的用户名
          if(strcmp(userName,Client::getinstance().m_LoginName.toStdString().c_str())==0){
              continue;
          }
          // REQ-UX-001：列表项直接展示用户名（旧实现加 "<序号>." 前缀，消费端按首个 '.' 解析，用户名含 '.' 会解析错）
          userNamelist.append(QString::fromUtf8(userName));
      }
      Index::getinstance().getFriend()->m_onlineUser->updateOnlineUser(userNamelist);
}

void MsgHandler::handlAddFriend()
{
    int ret=0;
    memcpy(&ret,pdu->caData,sizeof (ret));
    qCDebug(lcCli)<<"AddFriend ans ret"<<ret<<endl;
    // REQ-UX-001 A1：任何结果都必须有可见反馈并更新在线用户窗口提示条
    OnlineUser *ou=Index::getinstance().getFriend()->m_onlineUser;
    if(ret==1){
        // ret=1 来自对方的"同意"回执：双向好友关系已建立
        Index::getinstance().getFriend()->flushfriend();
        if(ou){ ou->setHint(QStringLiteral("好友添加成功")); }
        QMessageBox::information(ou,"提示","好友添加成功");
    }
    else if(ret==-2){
        if(ou){ ou->setHint(QStringLiteral("该用户不在线")); }
        QMessageBox::information(ou,"提示","该用户不在线");
    }
    else {
        if(ou){ ou->setHint(QStringLiteral("添加失败：对方不在线或已是好友")); }
        QMessageBox::information(ou,"提示","添加失败：对方不在线或已是好友");
    }
}
//同意添加好友
void MsgHandler::handlAddFriendAgree(const QString &myName)
{
    char seedName[32]={'\0'};
    memcpy(seedName,pdu->caData,32);
    int ret=QMessageBox::question(Index::getinstance().getFriend(),"提示",QString("你同意%1的好友申请吗?").arg(seedName));
    if(ret!=QMessageBox::Yes){
        return;
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_ADDFRIEND_RESPOND;
    memcpy(pdu->caData,myName.toStdString().c_str(),sizeof(myName.toStdString()));
    memcpy(pdu->caData+32,&seedName,sizeof (seedName));
    Client::getinstance().sorket.write((char*)pdu,pdu->uiTotalLen);
}

void MsgHandler::handlFlushFriend()
{
    int leng=pdu->uiMsgLen/32;//柔性数组长度
    QStringList userNamelist;
    for(int i=0;i<leng;i++){
        char userName[32]={'\0'};
        memcpy(userName,pdu->caMsg+i*32,32);//取出每一个用户的用户名
        userNamelist.append(QString("%1.%2").arg(i+1).arg(userName));//将编号还有用户名都统一加进去
    }
    Index::getinstance().getFriend()->flushFriend_LW(userNamelist);
}
//删除好友
void MsgHandler::handlDeleteFriend()
{
    bool ret=0;
    memcpy(&ret,pdu->caData,sizeof(ret));
    if(ret){
        Index::getinstance().getFriend()->flushfriend();
    }
    else {
        QMessageBox::information(Index::getinstance().getFriend(),"提示","删除好友失败");
    }
}


//聊天
void MsgHandler::chat()
{
    char seedName[32]={'\0'};
    memcpy(seedName,pdu->caData,32);
    Msg* pdu_Msg=(Msg*)pdu->caMsg;
    char msg[110]={'\0'};
    memcpy(msg,pdu_Msg->msg,pdu_Msg->msgLen);
    char currtime[30]={'\0'};
    memcpy(currtime,pdu_Msg->Time,30);
    Index::getinstance().getFriend()->m_chat->updateShow_TE(QString("%1:%2 发送时间:%3").arg(seedName).arg(msg).arg(currtime));
    Index::getinstance().getFriend()->m_chat->m_strChatName=seedName;
    if(Index::getinstance().getFriend()->m_chat->isHidden()){
        Index::getinstance().getFriend()->m_chat->show();
        Index::getinstance().getFriend()->m_chat->setWindowTitle(seedName);
    }
}
//获取所有未读消息
void MsgHandler::getcharmsg()
{
        QStringList Msglist;
        //记录偏移还有是否还有位置没有读取
        for(int dis=0;dis<pdu->uiMsgLen;){
            Msg* respdu_Msg=(Msg*)(pdu->caMsg+dis);
            char msg[110]={'\0'};
            char currTime[30]={'\0'};
            memcpy(msg,respdu_Msg->msg,respdu_Msg->msgLen);
            memcpy(currTime,respdu_Msg->Time,30);
            Msglist.append(QString(msg));
            Msglist.append(QString(currTime));
            qCDebug(lcCli)<<"getmsg msg:"<<QString(msg)<<" currTime:"<<QString(currTime)<<"  ";
            dis+=respdu_Msg->totalLen;
        }
        // 更新界面
        Index::getinstance().getFriend()->m_chat->updateShow_TEs(Msglist);
}
//创建目录
void MsgHandler::mkdir()
{
    bool ret=1;
    memcpy(&ret,pdu->caData,sizeof (ret));
    qCDebug(lcCli)<<"mkdir ret"<<ret<<endl;
    if(ret){
        QMessageBox::information(&Index::getinstance(),"提示","创建文件夹成功");
        Index::getinstance().getFile()->flushfile();
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示","创建文件夹失败");
    }
}

void MsgHandler::deldir()
{
    bool ret=0;
    memcpy(&ret,pdu->caData,sizeof(ret));
    qCDebug(lcCli)<<"删除文件结果"<<ret<<endl;
    if(ret){
        QMessageBox::information(&Index::getinstance(),"提示","删除文件夹成功");
        PDU* pdu=mkPDU(Index::getinstance().getFile()->m_strCurPath.size()+1);
        pdu->uiType=ENUM_MSG_TYPE_FLUSHFILE_REQUEST;
        memcpy(pdu->caMsg,Index::getinstance().getFile()->m_strCurPath.toStdString().c_str(),Index::getinstance().getFile()->m_strCurPath.size());
        Client::getinstance().seedMsg(pdu);
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示","删除文件夹失败");
    }
}

void MsgHandler::changedir()
{
    bool ret=0;
    memcpy(&ret,pdu->caData,sizeof(ret));
    if(ret){//如果路径存在
        Index::getinstance().getFile()->m_strCurPath=pdu->caMsg;
        Index::getinstance().getFile()->flushfile();
    }
    else {//如果路径不存在
        QMessageBox::information(&Index::getinstance(),"提示","路径不存在");
        Index::getinstance().getFile()->flushCurrLE();
    }
}
//重命名文件
void MsgHandler::renamedir()
{
    bool ret=0;
    memcpy(&ret,pdu->caData,sizeof (ret));
    qCDebug(lcCli)<<"rename ret"<<ret<<endl;
    if(ret){
        QMessageBox::information(&Index::getinstance(),"提示","文件重命名成功");
        PDU* pdu=mkPDU(Index::getinstance().getFile()->m_strCurPath.toUtf8().toStdString().size()+1);
        pdu->uiType=ENUM_MSG_TYPE_FLUSHFILE_REQUEST;
        memcpy(pdu->caMsg,Index::getinstance().getFile()->m_strCurPath.toStdString().c_str(),Index::getinstance().getFile()->m_strCurPath.toUtf8().toStdString().size());
        Client::getinstance().seedMsg(pdu);
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示","文件重命名失败");
    }
}

void MsgHandler::sharefileagree()
{
    char seedName[32]={'\0'};
    memcpy(seedName,pdu->caData,32);
    QString filePath=pdu->caMsg;
    int index=filePath.lastIndexOf('/');
    QString fileName=filePath.right(filePath.size()-index-1);
    qCDebug(lcCli)<<"接收到seedName:"<<seedName<<"的文件fileName "<<fileName<<" ";
    int ret=QMessageBox::question(&Index::getinstance(),"分享文件请求",QString("你是否同意接受%1的文件%2").arg(seedName).arg(fileName));
    qCDebug(lcCli)<<" ret="<<ret<<endl;
    if(ret!=QMessageBox::Yes){
        return ;
    }
    PDU* respdu=mkPDU(pdu->uiMsgLen);
    respdu->uiType=ENUM_MSG_TYPE_SHAREFILE_AGREE_RESPOND;
    memcpy(respdu->caData,Client::getinstance().m_LoginName.toStdString().c_str(),32);
    memcpy(respdu->caMsg,pdu->caMsg,respdu->uiMsgLen);
    Client::getinstance().seedMsg(respdu);
}

void MsgHandler::sharefilefinsh()
{
    bool ret=0;
    memcpy(&ret,pdu->caData,sizeof (ret));
    qCDebug(lcCli)<<"sharefilefinsh ret"<<ret<<endl;
    if(ret){
        QMessageBox::information(&Index::getinstance(),"提示","接收分享文件成功");
        Index::getinstance().getFile()->flushfile();//刷新一下文件
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示","接收分享文件失败");
    }
}

void MsgHandler::flushfile()
{
    QList<FileInfo*>qFileinfolist;
    int len=pdu->uiMsgLen/sizeof(FileInfo);
    for(int i=0;i<len;i++){
        FileInfo* newFileInfo=new FileInfo;
        memcpy(newFileInfo,pdu->caMsg+i*sizeof (FileInfo),sizeof(FileInfo));
        qFileinfolist.append(newFileInfo);
        qCDebug(lcCli)<<"newFileInfo->caName"<<newFileInfo->caName<<" newFileInfo->uiType"<<newFileInfo->uiType<<" ";
    }
    Index::getinstance().getFile()->updateCurrfile(qFileinfolist);
}

// 解析分享链接响应：服务端已把文件复制到其【用户目录/分享链接/】，这里提示并刷新列表
void MsgHandler::handleResolvePublishLink(PDU* pdu)
{
    bool ok = false;
    memcpy(&ok, pdu->caData, sizeof(ok));
    qCDebug(lcCli) << "[handleResolvePublishLink] 服务端返回 ok=" << ok;
    if(!ok){
        QMessageBox::information(&Index::getinstance(), "提示", "分享链接无效或已失效（链接不存在/已撤销/已过期）");
        return;
    }
    // 服务端已把文件复制到其【用户目录/分享链接/】下，这里解析返回的相对路径并提示
    QString json = QString::fromUtf8(pdu->caMsg);
    QString savedPath;
    const QString key = "\"saved_path\":\"";
    int idx = json.indexOf(key);
    if(idx >= 0){
        int start = idx + key.length();
        int end = json.indexOf('"', start);
        if(end > start){
            savedPath = json.mid(start, end - start);
        }
    }
    if(savedPath.isEmpty()){
        QMessageBox::information(&Index::getinstance(), "提示", "获取分享文件失败：未返回保存路径");
        return;
    }
    QMessageBox::information(&Index::getinstance(), "获取成功",
                             QString("文件已保存到：%1").arg(savedPath));
    // 刷新文件列表，使“分享链接”文件夹出现在用户目录中
    Index::getinstance().getFile()->flushfile();
}

void MsgHandler::handlfinduser(){
    int ret;
    memcpy(&ret,pdu->caData,sizeof(ret));
    qCDebug(lcCli)<<"client finduser ret"<<ret<<endl;
    if(ret==2){
        QMessageBox::information(&Client::getinstance(),"提示","该用户不存在");
    }
    else if(ret==1){
        QMessageBox::information(&Client::getinstance(),"提示","该用户在线");
    }
    else if(ret==0){
       QMessageBox::information(&Client::getinstance(),"提示","该用户不在线");
    }
}

void MsgHandler::handleCaptcha(){
    int status;
    memcpy(&status, pdu->caData, sizeof(status));
    int remaining;
    memcpy(&remaining, pdu->caData + 4, sizeof(remaining));

    if (status == 0) {
        // 成功：更新显示并启动正常冷却（倒计时由服务端下发秒数驱动）
        QByteArray codeBytes(pdu->caMsg, pdu->uiMsgLen);
        QString code = QString::fromUtf8(codeBytes).trimmed();
        qCDebug(lcCli) << "收到服务端验证码:" << code;
        Client::getinstance().updateCaptchaDisplay(code);
        Client::getinstance().startCaptchaCooldown(remaining > 0 ? remaining : 60);
    } else if (status == -5) {
        // 冷却中：禁止获取，按服务端剩余秒数倒计时
        Client::getinstance().startCaptchaCooldown(remaining > 0 ? remaining : 60);
        QMessageBox::information(&Client::getinstance(), "提示",
            QString("获取验证码过于频繁，请 %1 秒后再试").arg(remaining));
    } else if (status == -6) {
        // 锁定中：获取验证码按钮进入 10 分钟倒计时，并禁用注册/登录（客户端拦截）
        Client::getinstance().startCaptchaCooldown(remaining > 0 ? remaining : 600, true);
        QMessageBox::information(&Client::getinstance(), "提示",
            QString("账号已锁定，请 %1 分钟后再试").arg(qMax(1, remaining / 60)));
    }
}

void MsgHandler::handlMoveFile()
{
    bool ret=false;
    memcpy(&ret,pdu->caData,sizeof(ret));
    char fileName[32]={'\0'};
    memcpy(fileName,pdu->caData+32,32);
    qCDebug(lcCli)<<"movefile ret:"<<ret<<" fileName:"<<QString::fromUtf8(fileName);
    if(ret){
        QMessageBox::information(&Index::getinstance(),"提示",QString("文件%1移动成功").arg(QString::fromUtf8(fileName)));
        Index::getinstance().getFile()->flushfile();
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示","文件移动失败");
    }
}

void MsgHandler::handlUserInformation()
{
    int resultCode=0;
    int online=0;
    memcpy(&resultCode,pdu->caData,sizeof(resultCode));
    memcpy(&online,pdu->caData+sizeof(int),sizeof(online));

    char userName[32]={'\0'};
    memcpy(userName,pdu->caData+32,32);

    qCDebug(lcCli)<<"handlUserInformation result:"<<resultCode<<"online:"<<online<<"name:"<<QString::fromUtf8(userName);

    if(resultCode!=0){
        QMessageBox::information(&Index::getinstance(),"提示","用户信息获取失败");
        return;
    }

    //构建最小信息列表，仅填充用户名和在线状态，其余显示"未设置/暂无资料"
    QStringList msg;
    msg.append(QString::fromUtf8(userName));       //0: 用户名
    msg.append("未设置");                            //1: 昵称
    msg.append("0");                                //2: 年龄
    msg.append("0");                                //3: 性别
    msg.append("未设置");                            //4: 省
    msg.append("未设置");                            //5: 市
    msg.append("未设置");                            //6: 县
    msg.append("未设置");                            //7: 职业
    msg.append("未设置");                            //8-11: 学校
    msg.append("未设置");
    msg.append("未设置");
    msg.append("未设置");
    msg.append(online ? "在线" : "离线");            //12: 使用签名字段显示在线状态
    UserInformation::getinstance().fluMsg(msg);
}

void MsgHandler::handlLogoff()
{
    bool ret=false;
    memcpy(&ret,pdu->caData,sizeof(ret));
    qCDebug(lcCli)<<"handlLogoff result:"<<ret;
    if(ret){
        QMessageBox::information(&Index::getinstance(),"提示","账户已成功注销，即将返回登录界面。");
        // 关闭主界面，返回登录界面
        Index::getinstance().hide();
        Client::getinstance().show();
        Client::getinstance().flus();
        // 断开连接
        Client::getinstance().sorket.disconnectFromHost();
    }
    else {
        QMessageBox::information(&Index::getinstance(),"提示","注销失败：密码错误或用户不存在。");
    }
}

void MsgHandler::handleQueryFilePublishLink(PDU* pdu) {
    bool exists = false;
    memcpy(&exists, pdu->caData, sizeof(exists));
    quint32 state = PUBLINK_STATE_VALID;
    memcpy(&state, pdu->caData + PUBLINK_STATE_OFFSET, sizeof(state));

    if (!exists) {
        QMessageBox::information(&Index::getinstance(), "提示", "暂无分享链接");
        return;
    }
    // REQ-EXT-SHARE-001：链接已过期/已撤销时给出「重新生成 / 续期」的选择，
    // 不再把一条已知失效的链接当有效链接展示给主人
    if (state != PUBLINK_STATE_VALID) {
        Index::getinstance().getFile()->promptShareLinkRecovery(state);
        return;
    }

    QString link = QString::fromUtf8(pdu->caMsg);
    // 需要根据当前上下文打开对话框
    // 假设 Index 有当前选中的文件路径
    QString filePath = Index::getinstance().getFile()->m_strCurPath + "/" + Index::getinstance().getFile()->m_strShareFileName;
    ShareLinkDialog *dlg = new ShareLinkDialog(filePath, &Index::getinstance());
    dlg->setLink(link);
    dlg->exec();
    dlg->deleteLater();
}

void MsgHandler::handleCreatePublishLink(PDU* pdu) {
    bool ok = false;
    memcpy(&ok, pdu->caData, sizeof(ok));
    quint32 state = PUBLINK_STATE_VALID;
    memcpy(&state, pdu->caData + PUBLINK_STATE_OFFSET, sizeof(state));
    QString link = QString::fromUtf8(pdu->caMsg);
    if (ok && state == PUBLINK_STATE_VALID) {
        QMessageBox::information(&Index::getinstance(), "提示",
            QString("生成成功！\n分享链接：%1").arg(link));
    } else if (state != PUBLINK_STATE_VALID) {
        // 旧语义复用了一条已失效的链接，或续期被拒（例如链接已被撤销）→ 回到选择框，
        // 已撤销时「续期」按钮不可用，只能改走「重新生成链接」
        Index::getinstance().getFile()->promptShareLinkRecovery(
            state, QStringLiteral("（本次操作没有拿到可用链接）"));
    } else {
        QMessageBox::information(&Index::getinstance(), "提示", "生成分享链接失败");
    }
}
