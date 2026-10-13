#include "logging.h"
#include "msghandler.h"
#include "QDebug"
#include "operatedb.h"
#include "mytcpserver.h"
#include "server.h"
#include "sharelinkservice.h"
#include "fileobjectstore.h"
#include "memalloc.h"
#include <QByteArray>

#include <QFile>
#include <QFileInfo>
#include <QTime>
#include <QVector>
#include <QSet>
#include <cerrno>
#include <cstring>

namespace {
QString fixedString(const char *data, int maxLen)
{
    QByteArray bytes(data, maxLen);
    const int end = bytes.indexOf('\0');
    if (end >= 0) {
        bytes.truncate(end);
    }
    return QString::fromUtf8(bytes);
}
// REQ-EXT-SEC-001：将服务端派生的身份写入 32 字节定长字段（覆盖客户端提交的"自己"字段）
void copySelfField(char *dst, const QString &name)
{
    memset(dst, 0, 32);
    const QByteArray bytes = name.toUtf8();
    memcpy(dst, bytes.constData(), qMin(bytes.size(), 31));
}
}

MsgHandler::MsgHandler(){
    pdu=nullptr;
    m_captchaFailCount = 0;
    static bool seeded=false;
    if(!seeded){
        qsrand(QTime::currentTime().msec());
        seeded=true;
    }
}

QString MsgHandler::generateCaptchaCode(int length)
{
    const QString chars = "RSTnoprsXYZabcdeJK0QtuvwxyzUVWq123456789LMNPfghijklABCDEFGHm";
    QString code;
    for (int i = 0; i < length; ++i) {
        code.append(chars.at(qrand() % chars.length()));
    }
    return code;
}

int MsgHandler::validateCaptcha(const char* caMsg, uint uiMsgLen, int& remainingSec)
{
    remainingSec = 0;
    QDateTime now = QDateTime::currentDateTime();

    // 若之前锁定且已过期，恢复正常（冷却回到 1 分钟逻辑）
    if (m_captchaLockUntil.isValid() && now >= m_captchaLockUntil) {
        m_captchaLockUntil = QDateTime();
        m_captchaFailCount = 0;
    }

    // 锁定中：直接拒绝并回传剩余秒数
    if (m_captchaLockUntil.isValid() && now < m_captchaLockUntil) {
        remainingSec = now.secsTo(m_captchaLockUntil);
        qCDebug(lcAuth) << "验证码校验失败: 账号已锁定，剩余" << remainingSec << "秒";
        return -4;
    }

    if (uiMsgLen < 4) {
        m_captchaFailCount++;
        return -2;
    }
    QString userInput = QString::fromUtf8(caMsg).trimmed();

    // 无有效码或已过期：不累计失败次数，避免误锁
    if (m_captchaCode.isEmpty() || now > m_captchaExpiry) {
        m_captchaCode.clear();
        qCDebug(lcAuth) << "验证码校验失败: 无有效码或已过期";
        return -3;
    }

    if (userInput != m_captchaCode) {
        m_captchaFailCount++;
        // 连续错误达上限 → 触发锁定，冷却升级为锁定时长，作废当前码
        if (m_captchaFailCount >= CAPTCHA_MAX_FAILS) {
            m_captchaLockUntil = now.addSecs(CAPTCHA_LOCKOUT_SEC);
            m_captchaNextAllowed = m_captchaLockUntil;
            m_captchaCode.clear();
            remainingSec = CAPTCHA_LOCKOUT_SEC;
            qCWarning(lcAuth) << "验证码连续错误" << m_captchaFailCount << "次，账号锁定" << CAPTCHA_LOCKOUT_SEC << "秒";
            return -4;
        }
        qCDebug(lcAuth) << "验证码校验失败: 输入不符";
        return -2;
    }

    // 校验成功：一次性使用，作废当前码并重置错误计数
    m_captchaCode.clear();
    m_captchaFailCount = 0;
    qCDebug(lcAuth) << "验证码校验成功";
    return 0;
}

//处理注销账户请求
// REQ-EXT-SEC-001：授权依据为连接级身份（m_actorName），PDU 用户字段只作请求数据；
// 删除目录同样以服务端派生的 actor 生成，密码仅作为对本人账号的二次确认。
PDU *MsgHandler::logoff(bool &revoked)
{
    revoked=false;
    const QString actor=m_actorName;
    if(actor.isEmpty()){
        qCWarning(lcAuth)<<"[logoff] 拒绝：连接无认证上下文 conn="<<m_connId;
        PDU* respdu=mkPDU();
        respdu->uiType=ENUM_MSG_TYPE_LOGOFF_RESPOND;
        bool ret=false;
        memcpy(respdu->caData,&ret,sizeof(ret));
        return respdu;
    }

    QString claimedName = fixedString(pdu->caData, 32);
    QString userPwd  = fixedString(pdu->caData + 32, 32);

    if(claimedName!=actor){
        qCWarning(lcAuth)<<"[logoff] PDU 用户名字段与连接身份不符，按连接身份受理 conn="<<m_connId;
    }
    qCDebug(lcAuth) << "logoff request for:" << actor;

    bool ret = OperateDB::instance().handleLogoff(
        actor.toUtf8().constData(),
        userPwd.toUtf8().constData()
    );

    // 如果数据库删除成功，清理用户的文件目录（目录名 = 服务端 actor，非客户端字段）
    if(ret){
        QString userDir=QString("%1/%2").arg(Server::getinstance().filesPath).arg(actor);
        QDir dir(userDir);
        if(dir.exists()){
            bool removed=dir.removeRecursively();
            qCDebug(lcAuth)<<"logoff: remove user dir"<<userDir<<"result:"<<removed;
        }
        revoked=true;
    }

    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_LOGOFF_RESPOND;
    memcpy(respdu->caData,&ret,sizeof(ret));
    return respdu;
}

// ===== REQ-EXT-SEC-001：actor 用户根与规范路径校验 =====
QString MsgHandler::actorUserRoot() const
{
    if(m_actorName.isEmpty()){
        return QString();
    }
    return QDir(Server::getinstance().filesPath).absoluteFilePath(m_actorName);
}

bool MsgHandler::pathInActorRoot(const QString &path) const
{
    const QString root=actorUserRoot();
    if(root.isEmpty()){
        return false;
    }
    return isPathInSandbox(path, root);
}

// REQ-EXT-FILE-001 D5：对象文件不落用户路径，改校验其所在目录在 actor 根内且存在
bool MsgHandler::parentDirInActorRoot(const QString &filePath) const
{
    const QString parent = QFileInfo(filePath).absolutePath();
    const QString root = actorUserRoot();
    if(root.isEmpty()){
        return false;
    }
    return isPathInSandbox(parent, root);
}

// REQ-EXT-FILE-001 design §6：词法校验（cleanPath+绝对化后须等于或在 actor 根前缀内），
// 不要求物理存在——逻辑条目改名/移动/分享登记的唯一越权门。
bool MsgHandler::logicPathInActorRoot(const QString &path) const
{
    const QString root = actorUserRoot();
    if(root.isEmpty() || path.isEmpty()){
        return false;
    }
    const QString p = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QString r = QDir::cleanPath(root);
    return p == r || p.startsWith(r + "/");
}

PDU *MsgHandler::captcha()
{
    QDateTime now = QDateTime::currentDateTime();
    int status = 0;
    int remaining = 0;

    // 锁定中：禁止获取新码
    if (m_captchaLockUntil.isValid() && now < m_captchaLockUntil) {
        status = -6;
        remaining = now.secsTo(m_captchaLockUntil);
    }
    // 正常冷却中：拒绝并回传剩余秒数
    else if (now < m_captchaNextAllowed) {
        status = -5;
        remaining = now.secsTo(m_captchaNextAllowed);
    }
    else {
        // 覆盖式单码：直接覆盖，旧码自然失效
        m_captchaCode = generateCaptchaCode(6);
        m_captchaExpiry = now.addSecs(CAPTCHA_EXPIRE_SEC);
        m_captchaNextAllowed = now.addSecs(CAPTCHA_COOLDOWN_SEC);
        remaining = CAPTCHA_COOLDOWN_SEC;   // 成功时回传正常冷却时长
        status = 0;
        qCDebug(lcAuth) << "生成验证码:" << m_captchaCode << "过期时间:" << m_captchaExpiry;
    }

    PDU* respdu;
    if (status == 0) {
        QByteArray codeBytes = m_captchaCode.toUtf8();
        respdu = mkPDU(codeBytes.size() + 1);
        memcpy(respdu->caMsg, codeBytes.constData(), codeBytes.size() + 1);
    } else {
        respdu = mkPDU();
    }
    respdu->uiType = ENUM_MSG_TYPE_CAPTCHA_RESPOND;
    memcpy(respdu->caData, &status, sizeof(status));
    memcpy(respdu->caData + 4, &remaining, sizeof(remaining));
    return respdu;
}

//处理注册请求
PDU *MsgHandler::rigist()
{
    int remaining = 0;
    int captchaRet = validateCaptcha(pdu->caMsg, pdu->uiMsgLen, remaining);
    if (captchaRet != 0) {
        PDU* respdu = mkPDU();
        respdu->uiType = ENUM_MSG_TYPE_REGIST_RESPOND;
        memcpy(respdu->caData, &captchaRet, sizeof(captchaRet));
        if (captchaRet == -4) {
            memcpy(respdu->caData + 4, &remaining, sizeof(remaining));
        }
        return respdu;
    }
    //取出用户名和密码
    QString strname=fixedString(pdu->caData,32);
    QString strpwd=fixedString(pdu->caData+32,32);
    //利用数据库操作对象进行数据库的注册操作，返回注册结果并输出
    bool ret=OperateDB::instance().handleRegit(strname.toUtf8().data(),strpwd.toUtf8().data());
    qCDebug(lcAuth)<<"regist ret "<<ret<<endl;
    if(ret){
        QDir dir;
        bool ret1=dir.mkdir(QString("%1").arg(Server::getinstance().filesPath));
        bool ret2=dir.mkdir(QString("%1/%2").arg(Server::getinstance().filesPath).arg(strname));
        qCDebug(lcAuth)<<"创建用户目录:"<<"ret1 "<<ret1<<" "<<QString("%1").arg(Server::getinstance().filesPath)<<endl;
        qCDebug(lcAuth)<<"创建用户目录:"<<"ret2 "<<ret2<<" "<<QString("%1/%2").arg(Server::getinstance().filesPath).arg(strname)<<endl;
    }
    //将注册结果打包进响应报文返回
    PDU* respdu=mkPDU();
    memcpy(respdu->caData,&ret,sizeof(ret));
    QByteArray strnameBytes=strname.toUtf8();
    memcpy(respdu->caData+32,strnameBytes.constData(),qMin(strnameBytes.size(),31));
    respdu->uiType=ENUM_MSG_TYPE_REGIST_RESPOND;
    return respdu;
}
//处理登录请求，同时需要更新socket对象的客户用户名
// REQ-EXT-SEC-001：authOk 仅在服务端凭据校验通过时为 true，由连接层据此绑定 actor_context
// REQ-EXT-SESSION-001 D1：客户端唯一 ID 走登录 caMsg 验证码 NUL 之后的附加区（零协议改动）；
// loginVersion=服务端事务内分配的版本号，由连接层绑定为权威会话版本
PDU *MsgHandler::login(QString &strname, bool &authOk, quint64 &loginVersion)
{
    authOk = false;
    loginVersion = 0;
    int remaining = 0;
    int captchaRet = validateCaptcha(pdu->caMsg, pdu->uiMsgLen, remaining);
    if (captchaRet != 0) {
        PDU* respdu = mkPDU();
        respdu->uiType = ENUM_MSG_TYPE_LOGIN_RESPOND;
        memcpy(respdu->caData, &captchaRet, sizeof(captchaRet));
        if (captchaRet == -4) {
            memcpy(respdu->caData + 4, &remaining, sizeof(remaining));
        }
        return respdu;
    }
    //取出用户名和密码
    QString username=fixedString(pdu->caData,32);
    QString userpwd=fixedString(pdu->caData+32,32);
    // D1：验证码串（首个 NUL 前，NUL 必在 uiMsgLen 界内查找）之后的剩余字节=客户端 ID；
    // 无附加区/越界=空 ID（老客户端兼容）；只保留可打印 ASCII，截断 64（DB 列宽）
    QString clientId;
    {
        const uint cap=pdu->uiMsgLen;
        uint nul=0;
        while(nul<cap && pdu->caMsg[nul]!='\0'){ ++nul; }
        if(nul<cap && cap>nul+1){
            QByteArray raw(pdu->caMsg+nul+1, static_cast<int>(cap-nul-1));
            QByteArray clean;
            for(char c: raw){
                if(c=='\0'){ break; }
                if(c>=0x21 && c<=0x7e){ clean.append(c); }
                if(clean.size()>=64){ break; }
            }
            clientId=QString::fromUtf8(clean);
        }
    }
    //在数据库中检查用户名和密码是否一致或有问题
    int ret=OperateDB::instance().handleLogin(username.toUtf8().data(),userpwd.toUtf8().data(),clientId,loginVersion);
    qCDebug(lcAuth)<<"login ret"<<ret<<endl;
    if(ret==1){//记录已登录的用户名
        strname=username;
        authOk=true;
        // REQ-UX-001 A2：登录即幂等确保 actor 个人目录存在。
        // 目录此前只在注册成功时创建（rigist()），存量账号随部署更换 filesPath 后目录缺失；
        // 而沙箱校验要求路径物理存在（isPathInSandbox 取 canonicalFilePath，空即拒），
        // 于是自身根目录被判越权 → 列目录/建目录/上传全被拒、且用户无法自助恢复。
        const QString fsRoot = Server::getinstance().filesPath;
        if(!fsRoot.isEmpty()){
            QDir dir;
            dir.mkpath(fsRoot);
            dir.mkpath(QString("%1/%2").arg(fsRoot).arg(username));
        }
    }else{
        loginVersion=0;
    }
    //将登录结果打包进响应报文并返回
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_LOGIN_RESPOND;
    memcpy(respdu->caData,&ret,sizeof(ret));
    QByteArray usernameBytes=username.toUtf8();
    memcpy(respdu->caData+32,usernameBytes.constData(),qMin(usernameBytes.size(),31));
    return respdu;
}
//查找用户
PDU *MsgHandler::finduser(){
    //将需要查找的用户取出
    char userName[32]={'\0'};
    memcpy(userName,pdu->caData,32);
    //在数据库中查找所需要查找的用户，并返回其状态
    int ret=OperateDB::instance().handleFindUser(userName);
    qCDebug(lcNet)<<"findUser ret"<<ret<<endl;
    //将查找用户的状态打包进响应报文中并返回
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_findUser_RESPOND;
    memcpy(respdu->caData,&ret,sizeof(ret));
    return respdu;
}
//查找所有用户
PDU *MsgHandler::onlineuser(const QString &myname)
{
    //在数据库中查找所有在线用户
    QStringList userNames=OperateDB::instance().handleOnlineUsers(myname);
    //打印查找信息
    qCDebug(lcNet)<<"所有在线用户个数"<<userNames.size()<<" ";
    for(int i=0;i<userNames.size();i++){
        qCDebug(lcNet)<<userNames[i]<<" ";
    } qCDebug(lcNet)<<endl;
    //将查找的在线用户的信息打包进响应报文中并返回
    PDU* respdu=mkPDU(userNames.size()*32);
    respdu->uiType=ENUM_MSG_TYPE_ONLINEUSER_RESPOND;
    for(int i=0;i<userNames.size();i++){
        memcpy(respdu->caMsg+i*32,userNames[i].toStdString().c_str(),32);
    }
    return respdu;
}
//添加好友
PDU *MsgHandler::addfriend()
{
    //取出添加人和目标人
    // REQ-EXT-SEC-001：添加人（自己）以连接级 actor 覆盖
    const QString claimedSelf = fixedString(pdu->caData, 32);
    if(claimedSelf != m_actorName){
        qCWarning(lcNet)<<"[addfriend] 添加人字段与连接身份不符，按连接身份改写 conn="<<m_connId;
    }
    copySelfField(pdu->caData, m_actorName);
    char userName[32]={'\0'};
    char seedName[32]={'\0'};
    memcpy(userName,pdu->caData,32);
    memcpy(seedName,pdu->caData+32,32);
    //在数据库中检查用户名和密码是否一致或有问题
    int ret=OperateDB::instance().handleAddFriend(userName,seedName);
    qCDebug(lcNet)<<"addfriend ret"<<ret<<endl;
    PDU* respdu=nullptr;
    //将若是添加的好友不在线打包进响应报文并返回
    if(ret!=1){
        respdu=mkPDU();
        respdu->uiType=ENUM_MSG_TYPE_ADDFRIEND_RESPOND;
        memcpy(respdu->caData,&ret,sizeof(ret));
    }
    else {//在线给目标添加的好友发送请求
        PDU*pdu=mkPDU();
        pdu->uiType=ENUM_MSG_TYPE_ADDFRIEND_REQUEST;
        memcpy(pdu->caData,userName,32);
        memcpy(pdu->caData+32,seedName,32);
        MyTcpServer::getinstance().sendAddFriendAndRelease(seedName,pdu);
    }
    return respdu;
}

PDU *MsgHandler::addfriendFin()
{
    // REQ-EXT-SEC-001：同意人（自己）以连接级 actor 覆盖
    const QString claimedSelf = fixedString(pdu->caData, 32);
    if(claimedSelf != m_actorName){
        qCWarning(lcNet)<<"[addfriendFin] 同意人字段与连接身份不符，按连接身份改写 conn="<<m_connId;
    }
    copySelfField(pdu->caData, m_actorName);
    char agreeName[32]={'\0'};
    char seedName[32]={'\0'};
    memcpy(agreeName,pdu->caData,32);
    memcpy(seedName,pdu->caData+32,32);
    //在数据库中检查用户名和密码是否一致或有问题
    int ret=OperateDB::instance().handleAddFriendFin(seedName,agreeName);
    qCDebug(lcNet)<<" addfriendFin ret"<<ret<<endl;
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_ADDFRIEND_RESPOND;
    memcpy(pdu->caData,&ret,sizeof(ret));
    MyTcpServer::getinstance().seedAddFriend(seedName,pdu);
    MyTcpServer::getinstance().seedAddFriend(agreeName,pdu);
    memfree(pdu);
    return nullptr;
}

PDU *MsgHandler::flushfriend(const QString &username)
{
    QStringList ret=OperateDB::instance().handleFlushFriend(username);
    qCDebug(lcNet)<<"ret.size()="<<ret.size();
    for(int i=0;i<ret.size();i++){
        qCDebug(lcNet)<<ret[i]<<" ";
    }qCDebug(lcNet)<<endl;
    PDU* respdu=mkPDU(ret.size()*32);
    respdu->uiType=ENUM_MSG_TYPE_FLUSHFRIEND_RESPOND;
    for(int i=0;i<ret.size();i++){
        memcpy(respdu->caMsg+i*32,ret[i].toStdString().c_str(),32);
    }
    return respdu;
}

PDU *MsgHandler::deletefriend()
{
    // REQ-EXT-SEC-001：删除发起人（自己）在 caData+32，以连接级 actor 覆盖
    const QString claimedSelf = fixedString(pdu->caData + 32, 32);
    if(claimedSelf != m_actorName){
        qCWarning(lcNet)<<"[deletefriend] 发起人字段与连接身份不符，按连接身份改写 conn="<<m_connId;
    }
    copySelfField(pdu->caData + 32, m_actorName);
    char strName[32]={'\0'};
    char tarName[32]={'\0'};
    memcpy(strName,pdu->caData,32);
    memcpy(tarName,pdu->caData+32,32);
    bool ret=OperateDB::instance().handleDeleteFriend(strName,tarName);
    qCDebug(lcNet)<<"deletefriend ret"<<ret<<endl;

    // 跨功能联动：好友删除成功后，清理两人互发的私聊记录
    if(ret){
        OperateDB::instance().deleteFriendMessages(QString::fromUtf8(strName), QString::fromUtf8(tarName));
        qCDebug(lcNet) << "[deletefriend] 已联动清理双方私聊记录: " << strName << " - " << tarName;
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_DELETEFRIEND_RESPOND;
    memcpy(pdu->caData,&ret,sizeof(ret));
    return pdu;
}

PDU *MsgHandler::chat()
{
    // REQ-EXT-SEC-001：发送人字段以连接级 actor 强制覆盖（入库与转发报文同步改写），客户端提交只作请求数据
    const QString claimedSender = fixedString(pdu->caData, 32);
    if(claimedSender != m_actorName){
        qCWarning(lcNet)<<"[chat] 发送人字段与连接身份不符，按连接身份改写 conn="<<m_connId<<" claimed="<<claimedSender;
    }
    copySelfField(pdu->caData, m_actorName);
    char seedName[32]={'\0'};//发送人
    char tarName[32]={'\0'};//接收人
    memcpy(seedName,pdu->caData,32);
    memcpy(tarName,pdu->caData+32,32);
    char msg[120]={'\0'};//发送信息
    char currTime[30]={'\0'};//发送时间
    Msg* pdu_msg=(Msg*)pdu->caMsg;
    memcpy(msg,pdu_msg->msg,pdu_msg->msgLen);
    memcpy(currTime,pdu_msg->Time,30);
    qCDebug(lcNet)<<"接收到的msg"<<QString(msg)<<" 时间为"<<QString(currTime)<<endl;
    bool ret= OperateDB::instance().seedchatMsg(seedName,tarName,QString(msg),QString(currTime));
    qCDebug(lcNet)<<"char ret"<<ret<<endl;
    if(ret){
        MyTcpServer::getinstance().reseed(tarName,pdu);//转发给接收人
    }
    return nullptr;
}
//创建目录
// REQ-FS-003: 统一路径沙箱校验实现（唯一实现，所有入口复用）
bool MsgHandler::isPathInSandbox(const QString &path, const QString &root)
{
    if (path.isEmpty()) return false;

    const QString rootRaw = root.isEmpty() ? Server::getinstance().filesPath : root;
    // connect.config 实测 filesPath 为相对路径 "./filesys"，先绝对化归一再 canonical
    QFileInfo rootInfo(QDir(rootRaw).absolutePath());
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    if (canonicalRoot.isEmpty()) return false;      // 沙箱根不存在 → 一律拒绝

    QFileInfo targetInfo(path);
    const QString canonicalTarget = targetInfo.canonicalFilePath();
    if (canonicalTarget.isEmpty()) return false;    // 路径不存在（含非法路径）→ 拒绝

    QString prefix = canonicalRoot;
    if (!prefix.endsWith('/')) prefix.append('/');

    return canonicalTarget == canonicalRoot || canonicalTarget.startsWith(prefix);
}

PDU *MsgHandler::MKDIR(){
    //获取目录名和路径
    char fileName[32]={'\0'};
    QString path(QString::fromUtf8(pdu->caMsg));
    memcpy(fileName,pdu->caData,32);
    //构建QDir对象，创建目录，分两步
    bool ret1=false;
    bool ret2=false;
    const QString dirName = fixedString(fileName, 32);
    // REQ-FS-003⑤: path 为待建目录的所在父目录（须存在且在沙箱内）；新目录名不得含路径分隔符
    // REQ-EXT-SEC-001: 收敛为 actor 本人用户根（filesPath/<actor>），跨用户目录一律拒绝
    if(pathInActorRoot(path) && !dirName.contains('/') && !dirName.contains('\\')){
        QDir dir;
        ret1=dir.mkdir(QString("%1").arg(path));
        ret2=dir.mkdir(QString("%1/%2").arg(path).arg(dirName));
        // REQ-EXT-FILE-001 D5/T3：目录条目与物理 mkdir 同步建立（条目为权威展示源）。
        // 条目失败不回滚物理结果——flushfile 物理回退保证可见，日志留痕。
        if(ret2 && !m_actorName.isEmpty()){
            QString dirErr;
            if(!FileObjectStore::instance().insertDirEntry(m_actorName, path, dirName, dirErr)){
                qCWarning(lcFs)<<"[MKDIR] 目录条目建立失败 conn="<<m_connId<<":"<<dirErr;
            }
        }
    } else {
        qCWarning(lcFs)<<"[MKDIR] 拒绝越权建目录 conn="<<m_connId<<" 路径:"<<path<<" 名:"<<dirName;
    }
    qCDebug(lcFs)<<"mkdir" <<QString("%1").arg(path)<<"ret1"<<ret1<<" ";
    qCDebug(lcFs)<<"mkdir" <<QString("%1/%2").arg(path).arg(fileName)<<"ret2"<<ret2<<" ";
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_MKDIR_RESPOND;
    memcpy(respdu->caData,&ret2,sizeof(ret2));
    return respdu;
}
//删除目录
PDU *MsgHandler::deldir(const QString& loginName)
{
    //获取删除文件路径和文件类型
    QString delFilePath(QString::fromUtf8(pdu->caMsg));
    uint delFileType=0;
    memcpy(&delFileType,pdu->caData,sizeof(uint));
    qCDebug(lcFs)<<"删除文件:"<<delFilePath<<"文件类型"<<delFileType<<endl;
    bool ans=0;//记录删除结果，默认删除失败
    // REQ-FS-003②: 先校验后删除，顺序不可颠倒（removeRecursively 不可逆）
    // REQ-EXT-SEC-001: 只能删除 actor 本人用户根内的路径（loginName 参数即连接级身份）
    // REQ-EXT-FILE-001 D5：对象文件不物理落用户路径，改为校验所在目录在 actor 根内
    if(!parentDirInActorRoot(delFilePath)){
        qCWarning(lcFs)<<"[deldir] 拒绝越权删除 conn="<<m_connId<<" 路径:"<<delFilePath;
    }
    else {
        // REQ-EXT-FILE-001 D5：先按逻辑条目解引用（验收3：减引用，归零→待回收，不物理删对象）；
        // 无条目则回退物理删除（未迁移存量文件）。
        const QFileInfo delInfo(delFilePath);
        const QString delParent = FileObjectStore::normalizeParent(loginName, delInfo.absolutePath());
        const QString delName = delInfo.fileName();
        const FileObjectStore::EntryInfo e =
                FileObjectStore::instance().entryByPath(loginName, delParent, delName);
        if(e.valid()){
            if(e.isDir){
                ans = FileObjectStore::instance().releaseSubtree(e.fileId, loginName);
            } else {
                ans = FileObjectStore::instance().releaseEntry(e.fileId, loginName);
            }
        } else if(delFileType==1){
            QFile file(delFilePath);
            ans=file.remove();
        } else if(delFileType==0){
            QDir dir(delFilePath);
            ans=dir.removeRecursively();
            // REQ-EXT-FILE-001：旧目录物理删除后，子树内已迁移条目同步软删+减引用，防幽灵条目
            if(ans){
                QString relErr;
                if(!FileObjectStore::instance().releaseSubtreeByPath(loginName, delFilePath, relErr)){
                    qCWarning(lcFs)<<"[deldir] 子树条目级联软删失败 conn="<<m_connId<<":"<<relErr;
                }
            }
        }
    }

    // 跨功能联动：删除成功后，清理该文件（或目录下所有文件）对应的分享链接记录
    if(ans){
        // owner 直接使用登录名；relPath 以用户根目录为基准取相对路径，
        // 避免相对 filesPath 反推时把 filesys 误判为 owner 段
        QString userRoot = QDir(Server::getinstance().filesPath).absoluteFilePath(loginName);
        QString relPath = QDir(userRoot).relativeFilePath(QFileInfo(delFilePath).absoluteFilePath());
        if(!relPath.startsWith("..") && !relPath.startsWith("/") && !relPath.isEmpty()){
            OperateDB::instance().deleteShareLinkByPath(loginName, relPath);
            qCDebug(lcFs) << "[deldir] 已联动清理分享链接: owner=" << loginName << " relPath=" << relPath;
        }
    }

    //返回报文
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_DELFILE_RESPOND;
    memcpy(respdu->caData,&ans,sizeof(ans));
    return respdu;
}
//切换目录
PDU *MsgHandler::changedir(){
    QString tarPath=QString(pdu->caMsg).toUtf8();
    qCDebug(lcFs)<<"tarPath"<<tarPath<<" ";
    QFileInfo FileInfoi(tarPath.toStdString().c_str());
    qCDebug(lcFs)<<tarPath.toStdString().c_str()<<" ";
    PDU* respdu=nullptr;
    bool isExist=FileInfoi.exists();
    // REQ-FS-003⑤: 沙箱外路径按"不存在"处理，不回显任意路径的存在性与名称
    // REQ-EXT-SEC-001: 探测范围收敛为 actor 本人用户根，他人目录按"不存在"处理
    if(isExist && !pathInActorRoot(tarPath)){
        isExist=false;
        qCWarning(lcFs)<<"[changedir] 拒绝越权路径探测 conn="<<m_connId<<":"<<tarPath;
    }
    qCDebug(lcFs)<<" isExist "<<isExist<<" ";
    //如果文件存在
    if(isExist){
        if(FileInfoi.isDir()){//如果是目录
            //目标目录长度
            int PathLen=tarPath.toStdString().size();
            respdu=mkPDU(PathLen+1);//创建pdu，长度为目标目录长度+1
            memcpy(respdu->caMsg,tarPath.toStdString().c_str(),PathLen);
            qCDebug(lcFs)<<" isdir "<<"tarPath"<<tarPath<<endl;
        }
        else if(FileInfoi.isFile()){//如果是文件
            int index=tarPath.lastIndexOf('/');
            tarPath=tarPath.remove(index,tarPath.size()-index);
            //目标目录长度
            int PathLen=tarPath.toStdString().size();
            respdu=mkPDU(PathLen+1);//创建pdu，长度为目标目录长度+1
            memcpy(respdu->caMsg,tarPath.toStdString().c_str(),PathLen);
            qCDebug(lcFs)<<" isFile "<<"tarPath"<<tarPath<<endl;
        }
    }
    //如果文件不存在
    else {
        respdu=mkPDU();
    }
    respdu->uiType=ENUM_MSG_TYPE_CHANGEDIR_RESPOND;
    memcpy(respdu->caData,&isExist,sizeof (isExist));
    return respdu;
}
//刷新目录
PDU *MsgHandler::flushfile()
{
    QString path(QString::fromUtf8(pdu->caMsg));
    // REQ-FS-003⑤: 沙箱外目录一律不回显条目
    // REQ-EXT-SEC-001: 列目录范围收敛为 actor 本人用户根
    if(!pathInActorRoot(path)){
        qCWarning(lcFs)<<"[flushfile] 拒绝越权列目录 conn="<<m_connId<<":"<<path;
        PDU* respdu=mkPDU();
        respdu->uiType=ENUM_MSG_TYPE_FLUSHFILE_RESPOND;
        return respdu;
    }
    // REQ-EXT-FILE-001 D5：逻辑条目为权威；未迁移物理文件按名并入（不与 active 条目重名）。
    struct Row { QString name; bool isDir; };
    QVector<Row> rows;
    QSet<QString> seen;
    const QString parent = FileObjectStore::normalizeParent(m_actorName, path);
    const QList<FileObjectStore::EntryInfo> entries =
            FileObjectStore::instance().listEntries(m_actorName, parent);
    for(const FileObjectStore::EntryInfo& e : entries){
        Row r; r.name=e.displayName; r.isDir=e.isDir; rows.append(r); seen.insert(e.displayName);
    }
    QDir dir(path);
    if(dir.exists()){
        const QFileInfoList phys = dir.entryInfoList();
        for(int i=0;i<phys.size();++i){
            const QString nm = phys[i].fileName();
            if(nm=="."||nm==".."||nm==".staging"||nm=="objects") continue;
            if(seen.contains(nm)) continue;
            Row r; r.name=nm; r.isDir=phys[i].isDir(); rows.append(r);
        }
    }
    PDU* respdu=mkPDU(sizeof(FileInfo)*rows.size());
    respdu->uiType=ENUM_MSG_TYPE_FLUSHFILE_RESPOND;
    for(int i=0;i<rows.size();++i){
        FileInfo* pFileInfo=(FileInfo*)respdu->caMsg+i;
        memcpy(pFileInfo->caName,rows[i].name.toUtf8().constData(),32);
        pFileInfo->uiType=rows[i].isDir?0:1;
    }
    return respdu;
}
//文件/目录重命名
PDU *MsgHandler::renamefile(const QString& loginName)
{
    //拼接旧文件路径和新文件路径
    char oldFileName[32]={'\0'};
    char newFileName[32]={'\0'};
    memcpy(oldFileName,pdu->caData,32);
    memcpy(newFileName,pdu->caData+32,32);
    qCDebug(lcFs)<<"旧文件名:"<<oldFileName<<" "<<"新文件名:"<<newFileName<<" ";
    // 旧名/新名共用同一所在目录 dirPath（现状布局），校验目录一处即可；
    // 不校验 newFilePath：新文件尚不存在，canonical 为空会误拒所有正常重命名
    QString dirPath(QString::fromUtf8(pdu->caMsg+sizeof(uint)));
    // F-1 修复：名称段同 MKDIR 模式拒绝 '/'、'\\'，堵 "dirPath/../x" 拼接逃逸
    const QString oldName = fixedString(oldFileName, 32);
    const QString newName = fixedString(newFileName, 32);
    QString oldFilePath(dirPath+"/"+oldName);
    QString newFilePath(dirPath+"/"+newName);
    uint fileType=0;
    memcpy(&fileType,pdu->caMsg,sizeof(fileType));
    qCDebug(lcFs)<<"文件类型:"<<fileType<<" "<<"旧文件路径:"<<oldFilePath<<" "<<"新文件路径:"<<newFilePath<<" ";
    //进行文件或目录的重命名
    bool ret=1;
    // REQ-FS-003③: 先校验后重命名
    // REQ-EXT-SEC-002: fileType 不再采信——条目 is_dir 优先、无条目按物理推导；未知/错位声明只告警
    // REQ-EXT-SEC-001: 目录收敛为 actor 本人用户根（loginName 参数即连接级身份）
    if(!pathInActorRoot(dirPath)){
        ret=0;
        qCWarning(lcFs)<<"[renamefile] 拒绝越权重命名目录 conn="<<m_connId<<":"<<dirPath;
    }
    else if(oldName.contains('/') || oldName.contains('\\')
            || newName.contains('/') || newName.contains('\\')){
        // F-1: 名称段含分隔符时 dirPath 校验可被 "dirPath/../x" 拼接绕过，一律拒绝
        ret=0;
        qCWarning(lcFs)<<"[renamefile] 拒绝越权重命名名称段 conn="<<m_connId;
    }
    else {
        // REQ-EXT-FILE-001 D5：条目优先。有条目→只动逻辑键（目录条目级联子树，同事务），
        // 物理实体若存在（已迁移旧文件）尽力同步改名防回退视图幽灵；无条目→回退纯物理改名（未迁移）。
        const QString renParent = FileObjectStore::normalizeParent(loginName, dirPath);
        const FileObjectStore::EntryInfo e =
                FileObjectStore::instance().entryByPath(loginName, renParent, oldName);
        // REQ-EXT-SEC-002：类型权威=服务端（条目 is_dir 优先，无条目查物理）；客户端声明只做对照告警
        const QFileInfo oldPhys(oldFilePath);
        const bool serverIsDir = e.valid() ? e.isDir : oldPhys.isDir();
        const bool sourceExists = e.valid() || oldPhys.exists();
        if(fileType!=0 && fileType!=1){
            qCWarning(lcFs)<<"[renamefile] 客户端类型声明无效，按服务端推导执行 conn="<<m_connId<<":"<<fileType;
        } else if(bool(fileType==0)!=serverIsDir){
            qCWarning(lcFs)<<"[renamefile] 客户端类型声明与服务端推导不一致，按服务端推导执行 conn="<<m_connId;
        }
        if(oldName==newName){
            // REQ-EXT-SEC-002：同名幂等由服务端判定（改前只靠客户端早退，服务端会命中自身条目误报占用）
            ret = sourceExists;
            if(!ret){
                qCWarning(lcFs)<<"[renamefile] 同名幂等拒绝：源不存在 conn="<<m_connId;
            }
        }
        else if(e.valid()){
            if(!logicPathInActorRoot(dirPath)){
                ret=0;
                qCWarning(lcFs)<<"[renamefile] 拒绝越权重命名（逻辑）conn="<<m_connId<<":"<<dirPath;
            } else if(QFileInfo(newFilePath).exists()){
                // REQ-EXT-SEC-002：物理占用前置拒绝（与 movefile 混合检查同口径；改前条目已改、物理撞车只留告警）
                ret=0;
                qCWarning(lcFs)<<"[renamefile] 拒绝：目标名已被物理实体占用 conn="<<m_connId<<":"<<newFilePath;
            } else {
                QString relErr;
                ret = FileObjectStore::instance().relocateEntry(e.fileId, loginName, renParent, newName, relErr);
                if(!ret){
                    qCWarning(lcFs)<<"[renamefile] 条目改名失败 conn="<<m_connId<<":"<<relErr;
                } else if(oldPhys.exists()){
                    bool physOk = serverIsDir ? QDir().rename(oldFilePath,newFilePath)
                                              : QFile().rename(oldFilePath,newFilePath);
                    if(!physOk){
                        qCWarning(lcFs)<<"[renamefile] 物理实体同步改名失败（条目为准）conn="<<m_connId<<":"<<oldFilePath;
                    }
                }
            }
        }
        else if(!sourceExists){
            // REQ-EXT-SEC-002：源不存在一律拒绝（改前未知 fileType 不回退任何分支，ret=1 谎报成功）
            ret=0;
            qCWarning(lcFs)<<"[renamefile] 拒绝：重命名源不存在 conn="<<m_connId<<":"<<oldFilePath;
        }
        else if(serverIsDir){//目录（未迁移：物理改名 + 子树条目前缀级联，防幽灵键）
            QDir dir;
            ret=dir.rename(oldFilePath,newFilePath);
            if(ret){
                QString casErr;
                if(!FileObjectStore::instance().renameEntriesDir(loginName, oldFilePath, newFilePath, casErr)){
                    qCWarning(lcFs)<<"[renamefile] 旧目录改名条目级联失败 conn="<<m_connId<<":"<<casErr;
                }
            }
        }
        else {//文件（未迁移）
            QFile file;
            ret=file.rename(oldFilePath,newFilePath);
        }
    }
    qCDebug(lcFs)<<"rename ret"<<ret<<endl;

    // 跨功能联动：重命名成功后，平移该文件的分享链接 relative_path
    if(ret){
        // owner 直接使用登录名；relDir 以用户根目录为基准取相对路径，
        // 避免相对 filesPath 反推时把 filesys 误判为 owner 段
        // dirPath 已在重命名前取得（校验对象即此目录）
        // relDir：去掉"用户根目录"前缀得到相对路径。
        // 直接字符串剥离，避免 relativeFilePath 在 CWD 不一致时算出 ".." 开头，
        // 从而被下方守卫跳过整段联动（即重命名未关联 share_link 的根因）。
        QString userRootRel = QDir(Server::getinstance().filesPath).filePath(loginName); // 如 ./filesys/wcy
        QString relDir = dirPath;
        if (relDir.startsWith(userRootRel)) {
            relDir = relDir.mid(userRootRel.length());
            if (relDir.startsWith('/')) relDir = relDir.mid(1);
        } else {
            // 兜底：dirPath 非预期格式时退回 relativeFilePath 计算
            relDir = QDir(QDir(Server::getinstance().filesPath).absoluteFilePath(loginName))
                         .relativeFilePath(QFileInfo(dirPath).absoluteFilePath());
        }
        if(!relDir.startsWith("..") && !relDir.startsWith("/")){
            QString oldRel = relDir.isEmpty() ? oldName
                                             : relDir + "/" + oldName;
            QString newRel = relDir.isEmpty() ? newName
                                             : relDir + "/" + newName;
            OperateDB::instance().updateShareLinkPath(loginName, oldRel, newRel);
            qCDebug(lcFs) << "[renamefile] 已联动更新分享链接: owner=" << loginName
                     << " oldRel=" << oldRel << " newRel=" << newRel;
        }
    }

    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_RENAMEFILE_RESPOND;
    memcpy(respdu->caData,&ret,sizeof(ret));
    return respdu;
}
//分享文件（询问）
PDU *MsgHandler::sharefile()
{
    //发送用户名
    // REQ-EXT-SEC-001：分享人（自己）以连接级 actor 覆盖，转发给他人的署名即服务端身份
    const QString claimedSharer = fixedString(pdu->caData, 32);
    if(claimedSharer != m_actorName){
        qCWarning(lcFs)<<"[sharefile] 分享人字段与连接身份不符，按连接身份改写 conn="<<m_connId;
    }
    copySelfField(pdu->caData, m_actorName);
    char seedName[32]={'\0'};
    memcpy(seedName,pdu->caData,32);
    int revUserCnt=0;
    memcpy(&revUserCnt,pdu->caData+32,sizeof (revUserCnt));
    qCDebug(lcFs)<<"seedName "<<seedName<<" revUserCnt "<<revUserCnt<<" ";
    PDU* resendpdu=mkPDU(pdu->uiMsgLen-revUserCnt*32);
    resendpdu->uiType=ENUM_MSG_TYPE_SHAREFILE_AGREE_REQUEST;
    memcpy(resendpdu->caData,seedName,32);
    memcpy(resendpdu->caMsg,pdu->caMsg+32*revUserCnt,resendpdu->uiMsgLen);
    qCDebug(lcFs)<<"resendpdu->caMsg"<<resendpdu->caMsg<<endl;
    // REQ-EXT-SEC-001 M-1：分享源以服务端 canonical 登记为对每位接收者的握手授权；
    // 接收者同意移动时仅放行有登记的源，伪造 AGREE_RESPOND 拿不到未分享给自己的文件
    int sharePathLen = int(pdu->uiMsgLen) - 32*revUserCnt;
    if(sharePathLen < 0){
        sharePathLen = 0;
    }
    const QByteArray sharePathBytes(pdu->caMsg+32*revUserCnt, sharePathLen);
    const QString shareSrcRaw = QString::fromUtf8(sharePathBytes).trimmed();
    const QString shareCanonicalSrc = QFileInfo(shareSrcRaw).canonicalFilePath();
    // REQ-EXT-FILE-001 D5：对象文件无物理实体（canonical 为空）。握手键统一为
    // "canonical 存在用 canonical，否则用词法绝对路径"，sharefilemove 侧同式计算；
    // 归属仍由登记门（分享人本人条目/本人用户根）断言。
    const QString shareKey = !shareCanonicalSrc.isEmpty()
            ? shareCanonicalSrc
            : QDir::cleanPath(QFileInfo(shareSrcRaw).absoluteFilePath());
    const QFileInfo srcRawInfo(shareSrcRaw);
    const FileObjectStore::EntryInfo srcEntry = FileObjectStore::instance().entryByPath(
        m_actorName, FileObjectStore::normalizeParent(m_actorName, srcRawInfo.absolutePath()), srcRawInfo.fileName());
    // B-1 修复（增量复审）：分享源必须落在分享人本人用户根内——只能分享自己的文件；
    // 源不存在或属他人时不登记握手也不转发邀请，杜绝"自我登记+自我同意"复制他人文件
    const bool sourceOwned = (srcEntry.valid() && !srcEntry.isDir && logicPathInActorRoot(shareSrcRaw))
            || (!shareCanonicalSrc.isEmpty() && pathInActorRoot(shareSrcRaw));
    if(!sourceOwned){
        qCWarning(lcFs)<<"[sharefile] 拒绝：分享源不属于分享人，不登记不转发 conn="<<m_connId<<" 源:"<<shareSrcRaw;
    }
    for(int i=0;i<revUserCnt && sourceOwned;i++){
        char revName[32]={'\0'};
        memcpy(revName,pdu->caMsg+32*i,32);
        qCDebug(lcFs)<<"revName"<<revName<<" ";
        MyTcpServer::getinstance().addPendingShare(QString::fromUtf8(revName).trimmed(), shareKey);
        MyTcpServer::getinstance().reseed(revName,resendpdu);
    }
    memfree(resendpdu);
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_SHAREFILE_RESPOND;
    return respdu;
}
//分享文件（移动）
PDU *MsgHandler::sharefilemove()
{
    // REQ-EXT-SEC-001：接收者目录以连接级 actor 生成（PDU caData 用户字段仅作请求数据）；
    // 源路径必须持有分享人对本接收者的握手登记（M-1 修复），未授权的任意沙箱源被拒绝
    QString sourcePath=QString::fromUtf8(pdu->caMsg);
    int indexi=sourcePath.lastIndexOf('/');
    QString fileName=sourcePath.right(sourcePath.size()-indexi-1);
    QString recvName=m_actorName;
    if(recvName.isEmpty()){
        qCWarning(lcFs)<<"[sharefilemove] 拒绝：连接无认证上下文 conn="<<m_connId;
        PDU* respdu=mkPDU();
        bool fail=false;
        memcpy(respdu->caData,&fail,sizeof(fail));
        respdu->uiType=ENUM_MSG_TYPE_SHAREFILE_FINISH_RESPOND;
        return respdu;
    }
    const QString claimedRecv=QString::fromUtf8(pdu->caData);
    if(claimedRecv!=recvName){
        qCWarning(lcFs)<<"[sharefilemove] 接收者字段与连接身份不符，按连接身份生成目标 conn="<<m_connId;
    }
    QString recvDir=QString("%1/%2").arg(Server::getinstance().filesPath).arg(recvName);
    QString tarPath=QString("%1/%2").arg(recvDir).arg(fileName);
    qCDebug(lcFs)<<"sourcePath:"<<sourcePath<<"tarPath："<<tarPath<<endl;
    // REQ-FS-003⑤: 目标目录须在 actor 根内；目标文件名不得含路径分隔符（Windows 下 "\.." 逃逸）
    const QString canonicalSrc=QFileInfo(sourcePath).canonicalFilePath();
    // REQ-EXT-FILE-001 D5：握手键与登记侧同式（canonical 存在用 canonical，否则词法绝对路径）
    const QString srcKey = !canonicalSrc.isEmpty() ? canonicalSrc
                           : QDir::cleanPath(QFileInfo(sourcePath).absoluteFilePath());
    bool ret=false;
    if(!pathInActorRoot(recvDir)){
        qCWarning(lcFs)<<"[sharefilemove] 拒绝越权目标目录 conn="<<m_connId<<":"<<recvDir;
    } else if(fileName.contains('\\') || fileName.contains('/')){
        qCWarning(lcFs)<<"[sharefilemove] 拒绝含路径分隔符的目标名 conn="<<m_connId;
    } else if(!MyTcpServer::getinstance().hasPendingShare(recvName, srcKey)){
        // REQ-EXT-SEC-001 M-1 + FILE-001 D5: 无握手登记的键一律拒绝（登记门已断言源属分享人本人根/本人条目）
        qCWarning(lcFs)<<"[sharefilemove] 拒绝：源无分享握手登记 conn="<<m_connId<<":"<<sourcePath;
    } else {
        const QString fsRoot = QDir(Server::getinstance().filesPath).absolutePath();
        const QString rel = QDir(fsRoot).relativeFilePath(srcKey);
        FileObjectStore::EntryInfo e;
        if(!rel.startsWith("..")
           && QDir::cleanPath(QDir(fsRoot).filePath(rel)) == srcKey){
            const QFileInfo ki(srcKey);
            e = FileObjectStore::instance().entryByPath(
                    rel.section('/',0,0),
                    FileObjectStore::normalizeParent(rel.section('/',0,0), ki.absolutePath()), ki.fileName());
        }
        if(e.valid() && !e.isDir){
            // 条目优先：接收者新条目引用同一 object（+1 引用，物理零复制）；
            // 同名已占（物理或条目）→ (n) 后缀安置，与 movefile 规则一致
            QString base=fileName, ext;
            const int dot=fileName.lastIndexOf('.');
            if(dot>0){ base=fileName.left(dot); ext=fileName.mid(dot); }
            QString shareName=fileName;
            for(int n=1; n<=1000 && QFileInfo(QDir(recvDir).filePath(shareName)).exists(); ++n){
                shareName=QString("%1(%2)%3").arg(base).arg(n).arg(ext);
            }
            QString attachErr;
            const qint64 newFileId = FileObjectStore::instance().attachEntry(
                recvName, recvDir, shareName, e.objectId, attachErr);
            ret = newFileId >= 0;
            if(!ret){
                qCWarning(lcFs)<<"[sharefilemove] 接收者条目建立失败 conn="<<m_connId<<":"<<attachErr;
            } else {
                MyTcpServer::getinstance().consumePendingShare(recvName, srcKey);
                qCDebug(lcFs)<<"[sharefilemove] 条目引用完成（物理零复制）fileId="<<newFileId<<" objId="<<e.objectId;
            }
        } else if(!canonicalSrc.isEmpty()){
            // 回退：未迁移旧文件物理复制（M-1 原语义，执行用服务端 canonical）
            ret=QFile::copy(canonicalSrc,tarPath);
            if(ret){
                MyTcpServer::getinstance().consumePendingShare(recvName, srcKey);
            }
        } else {
            qCWarning(lcFs)<<"[sharefilemove] 拒绝：源条目与物理实体均不存在 conn="<<m_connId<<":"<<sourcePath;
        }
    }
    PDU* respdu=mkPDU();
    memcpy(respdu->caData,&ret,sizeof (ret));
    respdu->uiType=ENUM_MSG_TYPE_SHAREFILE_FINISH_RESPOND;
    return respdu;

}

//移动文件（任意目录间移动）
PDU *MsgHandler::movefile(const QString& loginName)
{
    char srcFileName[32]={'\0'};
    memcpy(srcFileName,pdu->caData,32);
    const QString claimedName=QString::fromUtf8(srcFileName).trimmed();

    // caMsg 格式：srcRel + '\0' + dstRel（两个以\0结尾的C串，srcRel 含文件名）
    QByteArray caMsgBytes(pdu->caMsg, pdu->uiMsgLen);
    int sep = caMsgBytes.indexOf('\0');
    // REQ-EXT-SEC-002：无 '\0' 分隔的畸形报文显式拒绝（改前 left(-1) 返回整串，靠目标非目录碰巧拒掉）
    QString srcRel = (sep < 0) ? QString() : QString::fromUtf8(caMsgBytes.left(sep)).trimmed();
    QString dstRel = (sep < 0) ? QString() : QString::fromUtf8(caMsgBytes.mid(sep+1)).trimmed();

    QString userRoot=QDir(Server::getinstance().filesPath).filePath(loginName);
    QString srcPath=QDir(userRoot).filePath(srcRel);   // srcRel 已含文件名
    QString dstDir =QDir(userRoot).filePath(dstRel);    // 目标目录
    // REQ-EXT-SEC-002：移动后的文件名权威=服务端从 srcRel 末段派生（回退分支 dstPath 因此不再受未校验字段影响，
    // 堵 "fileName=../x" 逃出已校验 dstDir）；caData 声称名只做对照告警，不一致按派生名执行（sharefile 同式）
    const QString fileName = QFileInfo(srcPath).fileName();
    if(!claimedName.isEmpty() && claimedName != fileName){
        qCWarning(lcFs)<<"movefile 声称名与 srcRel 末段不一致，按服务端派生执行 conn="<<m_connId<<":"<<fileName;
    }
    QString dstPath=QDir(dstDir).filePath(fileName);
    QString finalDstPath = dstPath;

    //路径合法性校验：源路径必须在用户根目录内
    // REQ-FS-003④: 统一走 isPathInSandbox，根传用户目录（原校验根即 userRoot，隔离级别不变；
    // canonicalSrc/canonicalDstDir 仍保留，供下方"移入自身子目录"与原地移动判断使用）
    QFileInfo srcInfo(srcPath);
    QString canonicalSrc=srcInfo.canonicalFilePath();
    QFileInfo dstInfo(dstDir);
    QString canonicalDstDir = dstInfo.canonicalFilePath(); // 目标目录必定存在

    bool ret=false;
    bool entryMoved=false;
    QString entryFinalName;
    // REQ-EXT-FILE-001 D5：条目优先——relocateEntry 只动逻辑键（目录条目同事务级联子树）；
    // 物理实体若存在（已迁移旧文件/旧目录）尽力同步搬移防回退视图幽灵；无条目→回退纯物理。
    const QString mvSrcParent = FileObjectStore::normalizeParent(loginName, srcInfo.absolutePath());
    const FileObjectStore::EntryInfo mvEntry =
            FileObjectStore::instance().entryByPath(loginName, mvSrcParent, fileName);
    // REQ-EXT-SEC-002：显式拒畸形分隔符与非法派生名（改前 caData 名零校验：可含 '/'/'\'落脏 display_name、可与 srcRel 错位）
    if(sep < 0){
        qCWarning(lcFs)<<"movefile 拒绝：caMsg 缺少 '\\0' 分隔符（畸形请求）conn="<<m_connId;
    }
    else if(fileName.isEmpty() || fileName.contains('/') || fileName.contains('\\')){
        qCWarning(lcFs)<<"movefile 拒绝：服务端派生名非法 conn="<<m_connId<<":"<<srcRel;
    }
    else if(mvEntry.valid()){
        const QString dstNorm = FileObjectStore::normalizeParent(loginName, dstDir);
        // F-3/D5：mvSrcParent/dstNorm 为逻辑键；"目录自身键"只在逻辑空间拼接，
        // 物理存在性检查一律用原始物理路径 dstDir（键与物理已解耦）。
        const QString dirSelfKey = mvSrcParent + fileName + "/";
        if(!logicPathInActorRoot(srcPath) || !logicPathInActorRoot(dstDir)){
            qCWarning(lcFs)<<"movefile 拒绝：越权移动（逻辑）conn="<<m_connId<<":"<<srcPath<<" -> "<<dstDir;
        }
        else if(!QFileInfo(dstDir).isDir()){
            qCWarning(lcFs)<<"movefile 拒绝：目标目录不存在 conn="<<m_connId<<":"<<dstDir;
        }
        else if(dstNorm == mvSrcParent){
            ret=true;
            qCDebug(lcFs)<<"movefile 原地移动（条目），无需操作";
        }
        else if(mvEntry.isDir && dstNorm.startsWith(dirSelfKey)){
            qCWarning(lcFs)<<"movefile 拒绝：不能把目录移进自身或其子目录";
        }
        else {
            QString base=fileName, ext;
            const int dot=fileName.lastIndexOf('.');
            if(dot>0){ base=fileName.left(dot); ext=fileName.mid(dot); }
            QString finalName=fileName;
            for(int n=1; n<=1000 && (FileObjectStore::instance().entryByPath(loginName, dstNorm, finalName).valid()
                                     || QFileInfo(QDir(dstDir).filePath(finalName)).exists()); ++n){
                finalName=QString("%1(%2)%3").arg(base).arg(n).arg(ext);
            }
            QString relErr;
            ret = FileObjectStore::instance().relocateEntry(mvEntry.fileId, loginName,
                                                            dstNorm, finalName, relErr);
            if(!ret){
                qCWarning(lcFs)<<"movefile 条目移动失败 conn="<<m_connId<<":"<<relErr;
            } else {
                entryMoved=true;
                entryFinalName=finalName;
                if(srcInfo.exists()){
                    const QString physDst = QDir(dstDir).filePath(finalName);
                    bool physOk = mvEntry.isDir
                            ? QDir().rename(srcPath, physDst)
                            : (QFile::rename(srcPath, physDst)
                               || (QFile::copy(srcPath, physDst) && QFile::remove(srcPath)));
                    if(!physOk){
                        qCWarning(lcFs)<<"movefile 物理实体同步搬移失败（条目为准）conn="<<m_connId<<":"<<srcPath;
                    }
                }
            }
        }
    }
    else if(isPathInSandbox(srcPath, userRoot) && srcInfo.exists()){
        //确保目标目录也在用户根目录内
        if(dstInfo.isDir() && isPathInSandbox(dstDir, userRoot)){
            // 防止把目录移进自身或其子目录
            if(srcInfo.isDir() && (canonicalDstDir==canonicalSrc || canonicalDstDir.startsWith(canonicalSrc+"/"))){
                qCWarning(lcFs)<<"movefile 拒绝：不能把目录移进自身或其子目录";
            }
            // 原地移动（目标目录就是源所在目录）：无需操作
            else if(canonicalDstDir == srcInfo.dir().canonicalPath()){
                ret=true;
                qCDebug(lcFs)<<"movefile 原地移动，无需操作";
            }
            else {
                // 自动重命名处理逻辑
                finalDstPath = dstPath;
                if (QFileInfo::exists(finalDstPath)) {
                    QFileInfo checkInfo(finalDstPath);
                    QDir parentDir = checkInfo.dir(); // 获取父目录 QDir 对象
                    QString baseName = checkInfo.completeBaseName();
                    QString suffix = checkInfo.suffix();
                    if (!suffix.isEmpty()) suffix = "." + suffix;

                    int count = 1;
                    const int MAX_RETRIES = 1000;
                    // 使用 QDir::filePath() 构建路径，确保跨平台安全
                    do {
                        QString numberedName = QString("%1(%2)%3").arg(baseName).arg(count++).arg(suffix);
                        finalDstPath = parentDir.filePath(numberedName);
                        if (count > MAX_RETRIES) { ret = false; break; }
                    } while (QFileInfo::exists(finalDstPath));
                    qCDebug(lcFs) << "文件移动重名，自动重命名为:" << finalDstPath;
                }

                //先尝试 rename；若跨文件系统失败则 copy+remove
                QFile srcFile(srcPath);
                ret = srcFile.rename(finalDstPath);
                if (!ret) {
                    qCWarning(lcFs) << "rename 失败, errorString:" << srcFile.errorString()
                             << "src:" << srcPath << "dst:" << finalDstPath;
                    // 降级策略：copy + remove
                    ret = QFile::copy(srcPath, finalDstPath);
                    if (!ret) {
                        qCWarning(lcFs) << "copy 降级也失败, src:" << srcPath << "dst:" << finalDstPath
                                 << "errno:" << errno << strerror(errno);
                    } else {
                        if (QFile::remove(srcPath)) {
                            qCDebug(lcFs) << "copy+remove 降级成功:" << finalDstPath;
                        } else {
                            qCWarning(lcFs) << "copy+remove 降级: copy 成功但 remove 失败, src:" << srcPath;
                            QFile::remove(finalDstPath);  // 清理不完整的目标文件
                            ret = false;
                        }
                    }
                }
            }
        } else {
            qCWarning(lcFs) << "movefile 目标路径校验失败: canonicalDstDir=" << canonicalDstDir;
        }
    } else {
        qCWarning(lcFs) << "movefile 源路径校验失败: canonicalSrc=" << canonicalSrc;
    }

    qCDebug(lcFs)<<"movefile src:"<<srcPath<<"dst:"<<dstPath<<"ret:"<<ret;

    const QString echoName = entryMoved ? entryFinalName : fileName;

    // 旧目录（无条目走物理分支）移动成功后：子树已迁移条目 parent_path 前缀平移（无条目空操作）
    if(ret && !entryMoved && srcInfo.isDir()){
        QString casErr;
        if(!FileObjectStore::instance().renameEntriesDir(loginName, srcPath, finalDstPath, casErr)){
            qCWarning(lcFs)<<"[movefile] 旧目录移动条目级联失败 conn="<<m_connId<<":"<<casErr;
        }
    }

    // 跨功能联动：移动成功后，平移该文件的分享链接 relative_path
    if(ret){
        QString newRel = dstRel.isEmpty() ? echoName : (dstRel + "/" + echoName);
        OperateDB::instance().updateShareLinkPath(loginName, srcRel, newRel);
        qCDebug(lcFs) << "[movefile] 已联动更新分享链接: owner=" << loginName
                 << " oldRel=" << srcRel << " newRel=" << newRel;
    }

    PDU* respdu=mkPDU();
    memcpy(respdu->caData,&ret,sizeof(ret));
    QByteArray fileNameBytes=echoName.toUtf8();
    memcpy(respdu->caData+32,fileNameBytes.constData(),qMin(fileNameBytes.size(),31));
    respdu->uiType=ENUM_MSG_TYPE_MOVEFILE_RESPOND;
    return respdu;
}
//列出某相对目录下的子目录（移动文件选择目标用）
PDU *MsgHandler::listdir(const QString& loginName)
{
    QString relPath=QString::fromUtf8(pdu->caMsg).trimmed(); // 相对根目录的浏览路径（空=根）
    QString userRoot=QDir(Server::getinstance().filesPath).filePath(loginName);
    QString absDir=QDir(userRoot).filePath(relPath);

    // 越权校验：浏览目录必须在用户根目录内
    // REQ-FS-003④: 统一走 isPathInSandbox，根传用户目录（原校验根即 userRoot，隔离级别不变）
    QFileInfo dirInfo(absDir);

    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_LISTDIR_RESPOND;
    if(isPathInSandbox(absDir, userRoot) && dirInfo.isDir()){
        QFileInfoList list=QDir(absDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        respdu=mkPDU(sizeof(FileInfo)*list.size());
        respdu->uiType=ENUM_MSG_TYPE_LISTDIR_RESPOND;
        for(int i=0;i<list.size();i++){
            FileInfo* p=(FileInfo*)respdu->caMsg+i;
            memcpy(p->caName,list[i].fileName().toStdString().c_str(),32);
            p->uiType=0;
        }
        qCDebug(lcFs)<<"listdir rel="<<relPath<<" 子目录数="<<list.size();
    } else {
        qCWarning(lcFs)<<"listdir 越权或目录不存在: rel="<<relPath;
    }
    return respdu;
}

//上传文件初始化
PDU *MsgHandler::uploadfileinit()
{
    char strname[32]={'\0'};
    qint64 fileSize=0;
    uint resumeMagic=0;
    memcpy(strname,pdu->caData,32);
    memcpy(&fileSize,pdu->caData+UPLOAD_FILE_SIZE_OFFSET,sizeof(qint64));
    memcpy(&resumeMagic,pdu->caData+UPLOAD_RESUME_MAGIC_OFFSET,sizeof(uint));

    const bool resumeEnabled = (resumeMagic == UPLOAD_RESUME_MAGIC);
    const QString dirPath = QString::fromUtf8(pdu->caMsg);
    const QString fileName = fixedString(strname, 32);
    const quint64 tid = m_currentTransferId;
    bool ret = false;
    bool complete = false;
    qint64 resumeOffset = 0;
    bool adoptedFastcheck = false;   // AUD-1：本次 29 消费了前序 97 的暂存 → 随后释放 id=0 占位

    // D7 硬上限 / T-17 重复 id：未通过则不建任何状态（P7：不回退到 id=0）
    if (acquireUploadSlot(tid)) {
        UploadCtx& ctx = m_uploads[tid];
        ctx.id = tid;
        ctx.sessionId = -1;
        ctx.expectedSha.clear();
        ctx.groupId = 0;
        ctx.groupDisplayName.clear();
        ctx.writeInFlight = false;
        ctx.lastProgressMs = QDateTime::currentMSecsSinceEpoch();

        // REQ-EXT-FILE-001 T4：内容寻址暂存——写进 .staging 而非最终目录，完成时 commitUpload 转正。
        // REQ-FS-003①/SEC-001：所在目录须在 actor 本人用户根内；父路径规范化后作逻辑条目键。
        const QString parentPath = FileObjectStore::normalizeParent(m_actorName, dirPath);
        // 采纳前序 97 预检查建立的会话（同 parent/display/size）：不重复建会话，保留摘要对照
        const bool adopt = (tid != TRANSFER_ID_LEGACY) && m_pendingFastcheck.active
                && !m_actorName.isEmpty()
                && m_pendingFastcheck.parent == parentPath
                && m_pendingFastcheck.display == fileName
                && m_pendingFastcheck.size == fileSize;
        if (pathInActorRoot(dirPath) && !m_actorName.isEmpty()) {
            const ResumableUploadSession::BeginResult result =
                    ctx.session.beginStaged(m_actorName, parentPath, fileName, fileSize, resumeEnabled);
            if (result.ok) {
                if (adopt) {
                    ctx.sessionId = m_pendingFastcheck.sessionId;
                    ctx.expectedSha = m_pendingFastcheck.shaHex;
                    adoptedFastcheck = true;
                } else {
                    // REQ-EXT-TRANSFER-003 S13/D7：先按暂存键找未完成行复用（同一 .part 只有一行）；
                    // 29 不携带请求键 → 不改写 client_request_id。未命中才新登记。
                    const qint64 reuseId = OperateDB::instance().findReusableUploadSession(
                                m_actorName, ctx.session.stagingFilePath());
                    if (reuseId >= 0) {
                        OperateDB::instance().restartUploadSession(
                                    reuseId, parentPath, fileName, fileSize, QString(),
                                    ctx.session.stagingFilePath(), result.offset, 3600);
                        ctx.sessionId = reuseId;
                    } else {
                        // 仅预检元数据（客户端不提交摘要，服务端复算为准）；登记会话行
                        ctx.sessionId = OperateDB::instance().createUploadSession(
                            m_actorName, parentPath, fileName, fileSize,
                            QString(), ctx.session.stagingFilePath(), result.offset, 3600);
                        if (ctx.sessionId < 0) {
                            qCWarning(lcFs) << "uploadfileinit 会话登记失败 conn=" << m_connId;
                        }
                    }
                }
                ret = true;
                resumeOffset = result.offset;
                complete = result.complete;
                // REQ-EXT-TRANSFER-003 S8：节流基准以本次续传起点与当前时刻初始化
                ctx.lastPersistOffset = result.offset;
                ctx.lastPersistMs = QDateTime::currentMSecsSinceEpoch();
                if (result.complete) {
                    // 0 字节或断点已到尾部：立即落定（仍走服务端复算，验收4）
                    finalizeUploadOnComplete();
                }
            }
        } else {
            qCWarning(lcFs) << "uploadfileinit 拒绝越权目录 conn=" << m_connId << ":" << dirPath;
        }
        m_pendingFastcheck.active = false;   // 一次 29 之后失效（无论是否采纳）
        if (!ret) {
            m_uploads.remove(tid);   // 未成功不保留空上下文
        } else if (adoptedFastcheck) {
            // AUD-1：97 的暂存已被本次 29 采纳（会话/摘要已转入本槽），id=0 占位不再需要——
            // 释放以免残留条目长期滞留、并与真实槽重复引用同一 upload_session 行。
            // 注：此处 ctx 引用不再使用，remove 可能引起 QHash 重排，故放在最后。
            m_uploads.remove(TRANSFER_ID_LEGACY);
        }
    } else {
        qCWarning(lcFs) << "uploadfileinit 拒绝：并发上限或 id 冲突 conn=" << m_connId
                        << " id=" << tid << " active=" << activeUploadCount();
    }

    qCDebug(lcFs)<<"uploadfileinit file"<<fileName
            <<"size"<<fileSize
            <<"resume"<<resumeEnabled
            <<"offset"<<resumeOffset
            <<"complete"<<complete
            <<"ret"<<ret<<"id"<<tid<<endl;
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_UPLOADFILE_INIT_RESPOND;
    memcpy(respdu->caData,&ret,sizeof (ret));
    memcpy(respdu->caData+UPLOAD_RESUME_OFFSET,&resumeOffset,sizeof(resumeOffset));
    if(resumeEnabled){
        memcpy(respdu->caData+UPLOAD_RESUME_MAGIC_OFFSET,&resumeMagic,sizeof(resumeMagic));
    }
    memcpy(respdu->caData+TRANSFER_ID_OFFSET,&tid,sizeof(tid));   // R3：响应回带 id
    return respdu;
}

// ===== REQ-EXT-TRANSFER-001：per-transfer 上传上下文访问 =====
MsgHandler::UploadCtx* MsgHandler::findUpload(quint64 id)
{
    QHash<quint64, UploadCtx>::iterator it = m_uploads.find(id);
    return it == m_uploads.end() ? nullptr : &it.value();
}

// REQ-EXT-TRANSFER-003 S5 补充：释放除 keepId 外所有指向同一暂存路径的句柄。
// 同一逻辑上传可能同时挂在两个上下文上（97 预检查的 id=0 槽 + 真实传输槽；幂等重放的旧槽），
// 它们指向同一个 .part——留着的第二个写句柄会让 rename/delete/truncate 在 Windows 上失败。
void MsgHandler::releaseOtherStagingHandles(quint64 keepId, const QString &stagingPath)
{
    if (stagingPath.isEmpty()) {
        return;
    }
    for (QHash<quint64, UploadCtx>::iterator it = m_uploads.begin(); it != m_uploads.end(); ++it) {
        if (it.key() == keepId) {
            continue;
        }
        if (it->session.stagingFilePath() == stagingPath) {
            it->session.releaseStagingFile();
        }
    }
}

MsgHandler::UploadCtx& MsgHandler::currentUpload()
{
    UploadCtx& ctx = m_uploads[m_currentTransferId];
    ctx.id = m_currentTransferId;
    return ctx;
}

// id=0 为旧单流槽，每次 INIT 重置；非 0 遵循 D7 上限与 T-17 重复 id 拒绝。
bool MsgHandler::acquireUploadSlot(quint64 id)
{
    if (id == TRANSFER_ID_LEGACY) {
        m_uploads.remove(TRANSFER_ID_LEGACY);
        return true;
    }
    if (m_uploads.contains(id)) {
        return false;
    }
    if (activeUploadCount() >= int(MAX_PARALLEL_UPLOADS)) {
        return false;
    }
    return true;
}

// Active→Aborted：会话置 expired，staging 保留（与暂停同处置，区别在客户端）
void MsgHandler::abortUpload(quint64 id)
{
    UploadCtx* cp = findUpload(id);
    if (!cp) {
        return;
    }
    // AUD-4：释放前把暂存对齐到"已确认偏移"——在途写若失败会留下尾部字节，会让暂存物理大小
    // 越过 upload_session.confirmed_offset，续传起点（beginStaged 按文件大小派生）与 DB 记录不一致。
    const QString staging = cp->session.stagingFilePath();
    const qint64 confirmed = cp->session.receivedSize();
    // REQ-EXT-TRANSFER-003 S5②：动文件（截断）前必须先释放共享暂存句柄（无在途写才能走到这里）
    cp->session.releaseStagingFile();
    releaseOtherStagingHandles(id, staging);
    if (!staging.isEmpty()) {
        QFile f(staging);
        if (f.open(QIODevice::ReadWrite) && f.size() > confirmed) {
            f.resize(confirmed);
        }
        if (f.isOpen()) f.close();
    }
    if (cp->sessionId >= 0) {
        OperateDB::instance().updateUploadOffset(cp->sessionId, confirmed);
        OperateDB::instance().finishUploadSession(cp->sessionId, "expired");
    }
    // N-1 连带：97 占位被空闲回收时，其"待 29 采纳"的登记（m_pendingFastcheck）必须一并失效——
    // 否则 60s 后到达的 29 会采纳一条已 expired 的 upload_session 行。按 sessionId 精确匹配，
    // 只影响被回收的正是该挂起会话的场合。
    if (m_pendingFastcheck.active && m_pendingFastcheck.sessionId == cp->sessionId) {
        m_pendingFastcheck.active = false;
    }
    cp->session.reset();
    m_uploads.remove(id);
    qCWarning(lcFs) << "[transfer] 释放上传槽 id=" << id << " conn=" << m_connId
                    << " 剩余=" << m_uploads.size();
}

// D5：上传上下文 60s 无进展回收（写盘在途不回收）
void MsgHandler::reapIdleUploads(qint64 nowMs, int idleSec)
{
    if (m_uploads.isEmpty()) {
        return;
    }
    const qint64 idleMs = qint64(idleSec) * 1000;
    QList<quint64> victims;
    for (QHash<quint64, UploadCtx>::const_iterator it = m_uploads.constBegin();
         it != m_uploads.constEnd(); ++it) {
        if (!it->writeInFlight && it->lastProgressMs > 0
                && nowMs - it->lastProgressMs > idleMs) {
            victims.append(it.key());
        }
    }
    for (quint64 id : victims) {
        qCWarning(lcFs) << "[transfer] 空闲回收上传槽 id=" << id << " conn=" << m_connId;
        abortUpload(id);
    }
}
// REQ-EXT-FILE-002 D2：秒传预检查（新 PDU 97/98）。
// 三段顺序：请求级幂等短路 → 摘要预检查（命中零正文） → 未命中登记暂存会话交回原顺序上传状态机。
// 不读客户端物理路径、不复制物理文件；拒绝路径只记长度与类型，不落内容/路径明文（lcFs）。
PDU *MsgHandler::uploadfilefastcheck()
{
    // 97 无 transfer_id（P2：客户端预检查保持单飞）→ 始终落在旧单流槽（id=0）
    m_uploads.remove(TRANSFER_ID_LEGACY);
    UploadCtx& ctx = currentUpload();
    // N-1：97 纯占位此前从不置 lastProgressMs，永不满足 D5 的空闲回收条件（要求 lastProgressMs > 0），
    // 残留的 id=0 暂存槽与 upload_session 行只能等 3600s TTL 或下一次 97 覆盖。建槽即起空闲计时，
    // 使其纳入既有回收口径（60s 无进展即 Active→Aborted，会话 expired、staging 保留）。
    ctx.lastProgressMs = QDateTime::currentMSecsSinceEpoch();
    // AUD-1 修复：97 是**预检查/暂存登记**，不是一次上传，其 id=0 槽不得占用上传并发名额
    // （activeUploadCount() 按此标记排除）。否则并行客户端"每文件先 97 再 29"会为每个文件留下
    // 一个被计入的占位，使同一连接第 3 路新个人上传的 INIT 被上限误拒（违背 D6 上传=3 / 验收 5）。
    // 注意：不得在此清掉该槽——旧客户端（REQ-EXT-FILE-002 形态）未命中后直接发 31 分块（无 29），
    // 正是靠这个 id=0 上下文接收正文；该槽在 31 推进时转为普通活跃上下文（见 handleUploadWriteFinished），
    // 或在被随后的 29 采纳后由 uploadfileinit 释放。
    ctx.fastcheckPlaceholder = true;
    bool ret = false;
    bool hit = false;
    qint64 fileId = -1;
    qint64 sessionId = -1;
    qint64 nextOffset = 0;
    QByteArray finalName;
    // 拒绝响应也带协议版本：客户端据此区分"版本不匹配"与"服务端拒绝"
    // AUD-3：同时尽力回带请求标识（97 的 reqId），使客户端能把迟到 98 归还到发起它的任务。
    // 拒绝路径下长度字段可能已非法，故按"caMsg 末尾 reqIdLen 字节"取；仍越界则不回带（客户端按无匹配丢弃）。
    auto rejectResponse = [&]() {
        QByteArray echo;
        if (pdu->caMsg != nullptr && pdu->uiMsgLen > 0) {
            uint rl = 0;
            memcpy(&rl, pdu->caData + FASTCHECK_REQID_LEN_OFFSET, sizeof(rl));
            if (rl >= 8 && rl <= 96 && pdu->uiMsgLen > 0 && rl <= uint(pdu->uiMsgLen)) {
                echo = QByteArray(pdu->caMsg + (pdu->uiMsgLen - int(rl)), int(rl));
            }
        }
        PDU* r = mkPDU(uint(echo.size()));
        const uint respVersion = FASTCHECK_VERSION;
        r->uiType = ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_RESPOND;
        memcpy(r->caData + FASTCHECK_RESP_RET_OFFSET, &ret, sizeof(ret));
        memcpy(r->caData + FASTCHECK_RESP_VERSION_OFFSET, &respVersion, sizeof(respVersion));
        if (!echo.isEmpty()) {
            memcpy(r->caMsg, echo.constData(), size_t(echo.size()));   // nameLen=0，客户端从 caMsg[0..] 读标识
        }
        return r;
    };

    // 1) 解析与校验：形态任一不合即拒绝，不解析内容
    uint version = 0, magic = 0, shaHexLen = 0, nameLen = 0, dirLen = 0, reqIdLen = 0;
    qint64 fileSize = 0;
    memcpy(&version, pdu->caData + FASTCHECK_VERSION_OFFSET, sizeof(version));
    memcpy(&magic, pdu->caData + FASTCHECK_MAGIC_OFFSET, sizeof(magic));
    memcpy(&fileSize, pdu->caData + FASTCHECK_FILE_SIZE_OFFSET, sizeof(fileSize));
    memcpy(&shaHexLen, pdu->caData + FASTCHECK_SHA_LEN_OFFSET, sizeof(shaHexLen));
    memcpy(&nameLen, pdu->caData + FASTCHECK_NAME_LEN_OFFSET, sizeof(nameLen));
    memcpy(&dirLen, pdu->caData + FASTCHECK_DIR_LEN_OFFSET, sizeof(dirLen));
    memcpy(&reqIdLen, pdu->caData + FASTCHECK_REQID_LEN_OFFSET, sizeof(reqIdLen));

    const uint total = shaHexLen + nameLen + dirLen + reqIdLen;
    const bool shapeOk = (magic == FASTCHECK_MAGIC) && (version == FASTCHECK_VERSION)
            && (fileSize >= 0)
            && (shaHexLen == uint(FASTCHECK_SHA_HEX_LEN))
            && (nameLen > 0 && nameLen <= 210)
            && (dirLen > 0 && dirLen <= 450)
            && (reqIdLen >= 8 && reqIdLen <= 96)
            && (total <= uint(FASTCHECK_MAX_MSG_LEN))
            && (total == pdu->uiMsgLen) && (pdu->caMsg != nullptr);

    const QByteArray body = shapeOk ? QByteArray(pdu->caMsg, int(pdu->uiMsgLen)) : QByteArray();
    QByteArray shaBytes, nameBytes, dirBytes, reqIdBytes;
    bool shaOk = false;
    if (shapeOk) {
        int off = 0;
        shaBytes = body.mid(off, int(shaHexLen)); off += int(shaHexLen);
        nameBytes = body.mid(off, int(nameLen)); off += int(nameLen);
        dirBytes = body.mid(off, int(dirLen)); off += int(dirLen);
        reqIdBytes = body.mid(off, int(reqIdLen));
        shaOk = true;
        for (int i = 0; i < shaBytes.size(); ++i) {      // 严格 [0-9a-f]（客户端 toHex 产出小写）
            const char c = shaBytes.at(i);
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) { shaOk = false; break; }
        }
    }
    if (!shapeOk || !shaOk) {
        qCWarning(lcFs) << "[fastcheck] 拒绝：请求形态非法 conn=" << m_connId
                        << " type=" << pdu->uiType << " shapeOk=" << shapeOk << " shaOk=" << shaOk;
        return rejectResponse();
    }

    const QString shaHex = QString::fromLatin1(shaBytes);
    const QString displayName = QString::fromUtf8(nameBytes);
    const QString dirPath = QString::fromUtf8(dirBytes);
    const QString reqId = QString::fromLatin1(reqIdBytes);

    // 2) 授权与目录归一：目标目录尚未创建，用逻辑校验（物理存在性留给仍走磁盘的操作）
    const bool dirOk = !m_actorName.isEmpty()
            && !displayName.isEmpty()
            && !displayName.contains('/') && !displayName.contains('\\')
            && logicPathInActorRoot(dirPath);
    const QString parent = dirOk ? FileObjectStore::normalizeParent(m_actorName, dirPath) : QString();
    if (!dirOk || parent.isEmpty()) {
        qCWarning(lcFs) << "[fastcheck] 拒绝：目录越权或无法归一 conn=" << m_connId
                        << " dirLen=" << dirLen;
        return rejectResponse();
    }

    // 本地：以本次请求的 parent/display/size 建立暂存会话（未命中与"失败重试"共用）。
    // retrySessionId >= 0 = 复用既有会话行（uk_req 下不可再插一行，只能原地重置）。
    // REQ-EXT-TRANSFER-003 S14/D7：入参来源扩展——请求键未命中时再按暂存键找同一 .part 的
    // 未完成行；按暂存键命中即把请求键改写为本次 reqId（任何 reqId 重放都收敛到同一行）。
    auto beginStagedSession = [&](qint64 retrySessionId) {
        const ResumableUploadSession::BeginResult br = ctx.session.beginStaged(
                    m_actorName, parent, displayName, fileSize, true);
        if (!br.ok) {
            qCWarning(lcFs) << "[fastcheck] 暂存会话建立失败 conn=" << m_connId;
            return;
        }
        QString rewriteReqId;
        if (retrySessionId < 0) {
            retrySessionId = OperateDB::instance().findReusableUploadSession(
                        m_actorName, ctx.session.stagingFilePath());
            if (retrySessionId >= 0) {
                rewriteReqId = reqId;
            }
        }
        if (retrySessionId >= 0) {
            if (!OperateDB::instance().restartUploadSession(
                        retrySessionId, parent, displayName, fileSize, shaHex,
                        ctx.session.stagingFilePath(), br.offset, 3600, rewriteReqId)) {
                qCWarning(lcFs) << "[fastcheck] 会话重置失败 conn=" << m_connId;
                return;
            }
            ctx.sessionId = retrySessionId;
            ctx.lastPersistOffset = br.offset;   // S8：节流基准随本次续传起点
            ctx.lastPersistMs = QDateTime::currentMSecsSinceEpoch();
        } else {
            ctx.sessionId = OperateDB::instance().createUploadSession(
                        m_actorName, parent, displayName, fileSize, shaHex,
                        ctx.session.stagingFilePath(), br.offset, 3600, reqId);
            if (ctx.sessionId < 0) {
                // 同请求键并发插入冲突：回读既有行复用（幂等重放不返回空会话）
                const OperateDB::UploadSessionLookup ex =
                        OperateDB::instance().lookupUploadSessionByRequestId(m_actorName, reqId);
                if (!ex.found || ex.sessionId < 0) {
                    qCWarning(lcFs) << "[fastcheck] 会话登记失败 conn=" << m_connId;
                    return;
                }
                ctx.sessionId = ex.sessionId;
            }
        }
        ctx.expectedSha = shaHex;
        ret = true; sessionId = ctx.sessionId; nextOffset = br.offset;
        if (br.complete) {
            const qint64 pending = ctx.sessionId;
            const FileObjectStore::CommitResult cr = finalizeUploadOnComplete();
            if (cr.ok) {
                hit = true; fileId = cr.fileId; sessionId = pending;
                finalName = cr.displayName.toUtf8();
            } else {
                ret = false;
            }
        }
    };

    // 3) 请求级幂等短路（零写入）
    const OperateDB::UploadSessionLookup prior =
            OperateDB::instance().lookupUploadSessionByRequestId(m_actorName, reqId);
    bool handled = false;
    if (prior.found && prior.status == "done" && prior.fileId >= 0) {
        ret = true; hit = true; fileId = prior.fileId; sessionId = prior.sessionId;
        finalName = prior.displayName.toUtf8();
        handled = true;
    } else if (prior.found && prior.status == "uploading") {
        // 断点续传：用会话行（而非本次请求）的 parent/display/size 复原同一 staging 令牌
        const ResumableUploadSession::BeginResult br = ctx.session.beginStaged(
                    m_actorName, prior.parentPath, prior.displayName, prior.expectedSize, true);
        if (br.ok) {
            ctx.sessionId = prior.sessionId;
            ctx.expectedSha = prior.clientSha256;
            ctx.lastPersistOffset = br.offset;   // S8：节流基准随本次续传起点
            ctx.lastPersistMs = QDateTime::currentMSecsSinceEpoch();
            ret = true; sessionId = prior.sessionId; nextOffset = br.offset;
            if (br.complete) {
                const qint64 pending = ctx.sessionId;
                const FileObjectStore::CommitResult cr = finalizeUploadOnComplete();
                if (cr.ok) { hit = true; fileId = cr.fileId; sessionId = pending; finalName = cr.displayName.toUtf8(); }
                else { ret = false; }
            }
        }
        handled = true;
    } else if (prior.found && (prior.status == "failed" || prior.status == "expired"
                               || prior.status == "verifying")) {
        // 同请求键重试：原地重置会话行后重新走暂存登记（uk_req 下不可再插一行）。
        // 注：status='done' 但 file_id 为空的历史行不在此列——落到步骤 4 由内容预检查收敛。
        beginStagedSession(prior.sessionId);
        handled = true;
    }

    // 4) 摘要预检查（去重命中）：命中则建条目 + 引用计数，零正文传输
    if (!handled) {
        const qint64 objectId = FileObjectStore::instance().findObjectBySha(shaHex, fileSize);
        if (objectId >= 0) {
            const FileObjectStore::AttachResult ar = FileObjectStore::instance().attachExistingObject(
                        m_actorName, parent, displayName, objectId, fileSize);
            if (ar.ok) {
                qint64 sid = OperateDB::instance().insertCompletedUploadSession(
                            m_actorName, parent, ar.displayName, fileSize, shaHex, reqId, ar.fileId);
                if (sid < 0) {
                    // 同请求键并发登记冲突：回读既有行（条目已落，不回滚）
                    const OperateDB::UploadSessionLookup ex =
                            OperateDB::instance().lookupUploadSessionByRequestId(m_actorName, reqId);
                    sid = ex.found ? ex.sessionId : -1;
                    qCWarning(lcFs) << "[fastcheck] 命中登记幂等行失败，回读既有会话 conn=" << m_connId;
                }
                ret = true; hit = true; fileId = ar.fileId; sessionId = sid;
                finalName = ar.displayName.toUtf8();
                qCDebug(lcFs) << "[fastcheck] 命中 fileId=" << ar.fileId << " objId=" << objectId
                              << " 幂等=" << ar.idempotent << " conn=" << m_connId;
            } else {
                qCWarning(lcFs) << "[fastcheck] 条目安置失败 conn=" << m_connId << " 原因=" << ar.error;
            }
        } else {
            // 未命中：不建对象、不建条目，只登记暂存会话（后续走既有分块 PDU，零改动）
            ctx.sessionId = -1;
            ctx.expectedSha.clear();
            beginStagedSession(-1);
        }
    }

    // 未命中且会话可续：暂存供随后的 29 INIT 采纳（个人并行上传握手 = 97 → 29 → 31/32）
    m_pendingFastcheck.active = false;
    if (ret && !hit) {
        UploadCtx& pc = currentUpload();
        m_pendingFastcheck.active = true;
        m_pendingFastcheck.sessionId = pc.sessionId;
        m_pendingFastcheck.shaHex = pc.expectedSha;
        m_pendingFastcheck.parent = pc.session.parentPath();
        m_pendingFastcheck.display = pc.session.displayName();
        m_pendingFastcheck.size = pc.session.totalSize();
    }

    qCDebug(lcFs) << "[fastcheck] conn=" << m_connId << " ret=" << ret << " hit=" << hit
                  << " fileId=" << fileId << " sessionId=" << sessionId
                  << " nextOffset=" << nextOffset << " nameLen=" << finalName.size();

    // AUD-3：98 回带 97 的请求标识（追加在名字之后，nameLen 不含它）——客户端据此按标识归属，
    // 丢弃无匹配的迟到 98。旧读法 msg[:nameLen] 不受影响（附加字段只增不减）。
    PDU* respdu = mkPDU(uint(finalName.size()) + uint(reqIdBytes.size()));
    respdu->uiType = ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_RESPOND;
    const uint respVersion = FASTCHECK_VERSION;
    const uint nameLenOut = uint(finalName.size());
    memcpy(respdu->caData + FASTCHECK_RESP_RET_OFFSET, &ret, sizeof(ret));
    memcpy(respdu->caData + FASTCHECK_RESP_HIT_OFFSET, &hit, sizeof(hit));
    memcpy(respdu->caData + FASTCHECK_RESP_VERSION_OFFSET, &respVersion, sizeof(respVersion));
    memcpy(respdu->caData + FASTCHECK_RESP_FILE_ID_OFFSET, &fileId, sizeof(fileId));
    memcpy(respdu->caData + FASTCHECK_RESP_SESSION_ID_OFFSET, &sessionId, sizeof(sessionId));
    memcpy(respdu->caData + FASTCHECK_RESP_NEXT_OFFSET, &nextOffset, sizeof(nextOffset));
    memcpy(respdu->caData + FASTCHECK_RESP_NAME_LEN_OFFSET, &nameLenOut, sizeof(nameLenOut));
    if (!finalName.isEmpty()) {
        memcpy(respdu->caMsg, finalName.constData(), size_t(finalName.size()));
    }
    if (!reqIdBytes.isEmpty()) {
        memcpy(respdu->caMsg + finalName.size(), reqIdBytes.constData(), size_t(reqIdBytes.size()));
    }
    return respdu;
}

//上传文件数据（此路径实际由 MyTcpSocket::handleUploadData 派发；保留按 id 取上下文）
PDU *MsgHandler::uploadfiledata()
{
    UploadCtx* cp = findUpload(m_currentTransferId);
    uint resumeMagic=0;
    qint64 chunkOffset=0;
    memcpy(&resumeMagic,pdu->caData+UPLOAD_RESUME_MAGIC_OFFSET,sizeof(uint));
    if(resumeMagic == UPLOAD_RESUME_MAGIC && cp){
        memcpy(&chunkOffset,pdu->caData+UPLOAD_CHUNK_OFFSET,sizeof(qint64));
        if(chunkOffset != cp->session.receivedSize()){
            bool ret=false;
            PDU* respdu=mkPDU();
            respdu->uiType=ENUM_MSG_TYPE_UPLOADFILE_DATA_RESPOND;
            memcpy(respdu->caData,&ret,sizeof(ret));
            memcpy(respdu->caData+UPLOAD_RESUME_MAGIC_OFFSET,&resumeMagic,sizeof(resumeMagic));
            return respdu;
        }
    }

    bool ret=false; // Upload writes are dispatched by MyTcpSocket to UploadWriteTask.
    qCDebug(lcFs)<<"uploadfiledata len"<<pdu->uiMsgLen
            <<"received"<<(cp?cp->session.receivedSize():-1)
            <<"total"<<(cp?cp->session.totalSize():-1)
            <<"ret"<<ret<<endl;
    if(ret && cp && !cp->session.isComplete()){
        return nullptr;
    }
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_UPLOADFILE_DATA_RESPOND;
    memcpy(respdu->caData,&ret,sizeof(ret));
    resumeMagic=UPLOAD_RESUME_MAGIC;
    memcpy(respdu->caData+UPLOAD_RESUME_MAGIC_OFFSET,&resumeMagic,sizeof(resumeMagic));
    return respdu;
}
// REQ-EXT-FILE-001 T4：上传完成→服务端复算转正 + 建逻辑条目 + 引用计数（同事务）。
// commitUpload 失败即拒绝转正（验收4）；staging 由其转正搬移或丢弃（验收1）。
// REQ-EXT-FILE-002 D2：传入会话登记的客户端摘要做复算对照——不符则不转正、不建条目、
// 会话置 failed、staging 删除（验收3）。摘要只作对照，绝不作为转正依据。
FileObjectStore::CommitResult MsgHandler::finalizeUploadOnComplete()
{
    FileObjectStore::CommitResult r;
    UploadCtx* cp = findUpload(m_currentTransferId);
    if (!cp) {
        r.error = "会话上下文缺失";
        qCWarning(lcFs) << "[finalizeUpload] 无上下文，拒绝转正 conn=" << m_connId
                        << " id=" << m_currentTransferId;
        return r;
    }
    const quint64 tid = cp->id;
    const QString owner = m_actorName;
    const QString parent = cp->session.parentPath();
    const QString display = cp->session.displayName();
    const qint64 expected = cp->session.totalSize();
    const QString staging = cp->session.stagingFilePath();
    const qint64 sessionId = cp->sessionId;
    const QString expectedSha = cp->expectedSha;
    const quint64 groupId = cp->groupId;
    const QString groupName = cp->groupDisplayName;
    if (owner.isEmpty() || parent.isEmpty() || display.isEmpty() || staging.isEmpty()) {
        r.error = "会话上下文缺失";
        qCWarning(lcFs) << "[finalizeUpload] 会话上下文缺失，拒绝转正 conn=" << m_connId;
        m_uploads.remove(tid);
        return r;
    }
    qCDebug(lcFs) << "[finalizeUpload] verifying conn=" << m_connId << " id=" << tid
                  << " owner=" << owner << " display=" << display;
    OperateDB::instance().finishUploadSession(sessionId, "verifying");
    // REQ-EXT-TRANSFER-003 S5①③：复算/rename/删除都发生在提交内部 → 提交前一并释放共享句柄
    // （含同一 .part 的别名上下文句柄：97 预检查槽可能仍开着同一文件）
    cp->session.releaseStagingFile();
    releaseOtherStagingHandles(tid, staging);
    r = FileObjectStore::instance().commitUpload(owner, parent, display, expected, staging,
                                                /*copyOnly=*/false, expectedSha);
    // REQ-EXT-GROUP-003：群作用域上传在个人条目落定后，同规则建群目录引用（+1 引用计数）。
    // 群引用建立失败则撤销个人条目（releaseEntry 减引用）并整体判失败，绝不留下半成品。
    m_lastGroupFileId = 0;
    if (r.ok && groupId != 0) {
        const FileObjectStore::GroupAttachResult ga = FileObjectStore::instance().attachGroupFile(
                    owner, groupId, groupName.isEmpty() ? r.displayName : groupName,
                    r.objectId, expected, /*createUserEntry=*/false, r.fileId);
        if (ga.ok) {
            m_lastGroupFileId = quint64(ga.groupFileId);
            qCDebug(lcFs) << "[finalizeUpload] 群引用落定 group=" << groupId
                          << " gf=" << ga.groupFileId << " 群名=" << ga.groupDisplayName;
        } else {
            QString relErr;
            FileObjectStore::instance().releaseEntry(r.fileId, owner);   // 撤销个人条目，保持一致性
            r.ok = false;
            r.error = "群引用建立失败：" + ga.error;
            qCCritical(lcFs) << "[finalizeUpload] 群引用失败，已撤销个人条目 conn=" << m_connId
                             << " group=" << groupId << " 原因=" << ga.error;
        }
    }
    if (r.ok) {
        // 同写 file_id：使"已完成会话"能被请求级幂等短路直接命中（§2 步骤3）
        // REQ-EXT-TRANSFER-003 S10：完成态强制写 confirmed_offset == expected_size（不依赖节流）
        OperateDB::instance().finishUploadSession(sessionId, "done", r.fileId, expected);
        qCDebug(lcFs) << "[finalizeUpload] 落定 fileId=" << r.fileId << " objId=" << r.objectId
                      << " dedup=" << r.dedupHit << " 定名=" << r.displayName;
    } else {
        OperateDB::instance().finishUploadSession(sessionId, "failed", -1, 0);   // S10：暂存已删，偏移归零
        qCWarning(lcFs) << "[finalizeUpload] 拒绝转正 conn=" << m_connId << " 原因=" << r.error;
    }
    m_uploads.remove(tid);   // 上下文释放（Committed → [无]）
    return r;
}

//下载文件（仅做校验，流式分块由 MyTcpSocket 完成）
PDU *MsgHandler::downloadfile()
{
    QString filePath=QString::fromUtf8(pdu->caMsg);
    QFileInfo fileInfo(filePath);
    // REQ-FS-003④: 统一走 isPathInSandbox（原校验根即整个 filesPath，与默认根一致；
    // canonicalFilePath 仍保留，供 MyTcpSocket 流式下载使用）
    QString canonicalFilePath=fileInfo.canonicalFilePath();
    QString fileName=fixedString(pdu->caData,32);
    bool ret=false;
    qint64 fileSize=0;
    m_downloadCanonicalPath.clear();
    if (!m_downloadSlotAvailable) {
        // D7 硬兜底：本连接并发下载已达上限（客户端正常排队时不触发）
        qCWarning(lcFs) << "[downloadfile] 拒绝：下载并发上限 conn=" << m_connId
                        << " id=" << m_currentTransferId;
        PDU* respdu=mkPDU();
        respdu->uiType=ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND;
        memcpy(respdu->caData,&ret,sizeof(ret));
        memcpy(respdu->caData+sizeof(bool),&fileSize,sizeof(fileSize));
        memcpy(respdu->caData+DOWNLOAD_RESP_TRANSFER_ID_OFFSET,&m_currentTransferId,sizeof(m_currentTransferId));
        return respdu;
    }

    // REQ-EXT-SEC-001: 下载范围收敛为 actor 本人用户根，他人文件不可下载
    // REQ-EXT-FILE-001 D5：对象文件不物理落用户路径，改为校验所在目录在 actor 根内
    if(parentDirInActorRoot(filePath) && !fileName.isEmpty()){
        // REQ-EXT-FILE-001 D5：优先解析逻辑条目→对象绝对路径；对象缺失回退物理文件（未迁移）。
        const QString dlParent = FileObjectStore::normalizeParent(m_actorName, fileInfo.absolutePath());
        const FileObjectStore::EntryInfo e =
                FileObjectStore::instance().entryByPath(m_actorName, dlParent, fileName);
        const QString objAbs = (e.valid() && !e.isDir)
                ? FileObjectStore::instance().objectAbsPath(e.objectId) : QString();
        if(!objAbs.isEmpty() && QFileInfo(objAbs).isFile()){
            fileSize = e.sizeBytes > 0 ? e.sizeBytes : QFileInfo(objAbs).size();
            m_downloadCanonicalPath = objAbs;
            ret = true;
        } else if(fileInfo.isFile()){
            fileSize=fileInfo.size();
            m_downloadCanonicalPath=canonicalFilePath;
            ret=true;
        }
    }
    if(!ret){
        qCWarning(lcFs)<<"[downloadfile] 拒绝越权/不存在下载 conn="<<m_connId<<" 路径:"<<filePath;
    }
    // REQ-EXT-TRANSFER-002 D1/§2.3：声明的续传起点 > 文件大小 → 明确拒绝、不建流
    if(ret && m_downloadResumeOffset > fileSize){
        qCWarning(lcFs)<<"[downloadfile] 拒绝：续传起点非法 conn="<<m_connId
                       <<" id="<<m_currentTransferId<<" offset="<<m_downloadResumeOffset
                       <<" size="<<fileSize;
        ret=false;
        m_downloadCanonicalPath.clear();
    }

    //仅返回元数据 PDU（不含文件内容），实际数据由 MyTcpSocket 流式发送
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND;
    memcpy(respdu->caData,&ret,sizeof(ret));
    memcpy(respdu->caData+sizeof(bool),&fileSize,sizeof(fileSize));
    // 41 的 [32..62] 被文件名字段占用 → id 落在独立的 [16..23]（D2）
    memcpy(respdu->caData+DOWNLOAD_RESP_TRANSFER_ID_OFFSET,&m_currentTransferId,sizeof(m_currentTransferId));
    // REQ-EXT-TRANSFER-002 S4/D6：受理并建流时回带采纳起点（R4：拒绝/无流时该区间保持 0）
    if(ret){
        const qint64 accepted=m_downloadResumeOffset;
        memcpy(respdu->caData+DOWNLOAD_ACCEPTED_OFFSET,&accepted,sizeof(accepted));
    }
    QByteArray fileNameBytes=fileName.toUtf8();
    memcpy(respdu->caData+32,fileNameBytes.constData(),qMin(fileNameBytes.size(),31));
    qCDebug(lcFs)<<"downloadfile validate path"<<filePath<<"ret"<<ret<<"size"<<fileSize;
    return respdu;
}

//获取聊天信息
PDU *MsgHandler::chatmsg()
{
    // REQ-EXT-SEC-001：查询者（自己）在 caData 首段，以连接级 actor 覆盖，防止读取他人聊天历史
    const QString claimedSelf = fixedString(pdu->caData, 32);
    if(claimedSelf != m_actorName){
        qCWarning(lcNet)<<"[chatmsg] 查询者字段与连接身份不符，按连接身份改写 conn="<<m_connId;
    }
    copySelfField(pdu->caData, m_actorName);
    char seedName[32]={'\0'};
    char revName[32]={'\0'};
    memcpy(revName,pdu->caData,32);
    memcpy(seedName,pdu->caData+32,32);
    QStringList Msglist= OperateDB::instance().getchatMsg(seedName,revName);
    qCDebug(lcNet)<<"chatmsg msglist.size"<<Msglist.size()<<" ";
    int Msgtotallen=0;
    for(int i=0;i<Msglist.size();i+=2){
        Msgtotallen+=Msglist[i].toUtf8().size()+1+sizeof(Msg);
    }
    PDU* respdu=mkPDU(Msgtotallen);
    respdu->uiType=ENUM_MSG_TYPE_GETCHATMSG_RESPOND;
    for(int i=0,dis=0;i<Msglist.size();i+=2){
        Msg* filei=(Msg*)(respdu->caMsg+dis);
        filei->totalLen=sizeof(Msg)+Msglist[i].toUtf8().size()+1;
        filei->msgLen=Msglist[i].toUtf8().size()+1;
        memcpy(filei->Time,Msglist[i+1].toUtf8().toStdString().c_str(),30);
        memcpy(filei->msg,Msglist[i].toUtf8().toStdString().c_str(),filei->msgLen);
        qCDebug(lcNet)<<"filei->msg "<<filei->msg<<" filei->Time"<<filei->Time<<",";
        dis+=filei->totalLen;
    }
    return respdu;
}

//获取用户信息
PDU *MsgHandler::getuserinformation()
{
    char userName[32]={'\0'};
    memcpy(userName,pdu->caData,32);

    QString outName;
    int outOnline=0;
    bool found=OperateDB::instance().handleGetUserInfo(userName,outName,outOnline);

    int resultCode=found ? 0 : -1;
    PDU* respdu=mkPDU();
    respdu->uiType=ENUM_MSG_TYPE_GETUSERINFORMATION_RESPOND;
    memcpy(respdu->caData,&resultCode,sizeof(resultCode));
    memcpy(respdu->caData+sizeof(int),&outOnline,sizeof(outOnline));
    QByteArray nameBytes=outName.toUtf8();
    memcpy(respdu->caData+32,nameBytes.constData(),qMin(nameBytes.size(),31));
    qCDebug(lcNet)<<"getuserinformation userName:"<<userName<<"found:"<<found<<"online:"<<outOnline;
    return respdu;
}

PDU* MsgHandler::createPublishLink(const QString& loginName) {
    QString relativePath = fixedString(pdu->caMsg, pdu->uiMsgLen);
    QString ownerName = loginName;
    qCDebug(lcFs) << "[createPublishLink] 收到生成请求: owner=" << ownerName << " relativePath=" << relativePath;

    // REQ-EXT-SHARE-001：解析 caMsg 附加动作区（首个 NUL 之后）。旧客户端不带该区 → action 为空，
    // 走"有记录复用、无则新建"的旧语义；旧服务端用 fixedString 读路径即止，附加区被忽略。
    QString action;
    int renewSeconds = 0;
    {
        const uint cap = pdu->uiMsgLen;
        uint nul = 0;
        while(nul < cap && pdu->caMsg[nul] != '\0'){ ++nul; }
        if(nul + 1 < cap){
            const QByteArray raw(pdu->caMsg + nul + 1, static_cast<int>(cap - nul - 1));
            const int sep = raw.indexOf('\0');
            action = QString::fromUtf8(sep >= 0 ? raw.left(sep) : raw).trimmed();
            if(sep >= 0 && sep + 1 < raw.size()){
                bool numOk = false;
                renewSeconds = QString::fromUtf8(raw.mid(sep + 1)).trimmed().toInt(&numOk);
                if(!numOk) renewSeconds = 0;
            }
        }
    }
    if(renewSeconds <= 0){
        renewSeconds = PUBLINK_RENEW_DEFAULT_SECONDS;
    }

    OperateDB::ShareLinkInfo existing;
    const bool hasExisting = OperateDB::instance().getShareLinkInfoByPath(ownerName, relativePath, existing);
    quint32 state = PUBLINK_STATE_VALID;
    if(hasExisting){
        const bool expired = QDateTime::currentDateTime() >= existing.expireTime;
        state = existing.revoked ? PUBLINK_STATE_REVOKED
                                 : (expired ? PUBLINK_STATE_EXPIRED : PUBLINK_STATE_VALID);
    }

    QString linkValue;
    bool ok = false;
    if(action == PUBLINK_ACTION_NEW){
        // 重新生成：先作废旧行（旧链接立即失效），再建新 token——"重新获取"要能换掉泄露出去的旧链接
        if(hasExisting && !OperateDB::instance().removeShareLinkExact(ownerName, relativePath)){
            qCWarning(lcFs) << "[createPublishLink] 作废旧链接失败，放弃重新生成 owner=" << ownerName;
        } else {
            const QString token = ShareLinkService::generateToken();
            ok = OperateDB::instance().createShareLink(ShareLinkService::hashToken(token), ownerName,
                                                       relativePath, ShareLinkService::defaultExpireTime());
            if(ok){
                linkValue = token;
                state = PUBLINK_STATE_VALID;
            }
        }
    } else if(action == PUBLINK_ACTION_RENEW){
        if(!hasExisting){
            // 没有记录可续期 → 等同首次创建，按请求的时长生效
            const QString token = ShareLinkService::generateToken();
            ok = OperateDB::instance().createShareLink(ShareLinkService::hashToken(token), ownerName,
                                                       relativePath,
                                                       QDateTime::currentDateTime().addSecs(renewSeconds));
            if(ok){
                linkValue = token;
                state = PUBLINK_STATE_VALID;
            }
        } else if(existing.revoked){
            qCWarning(lcFs) << "[createPublishLink] 拒绝续期：链接已撤销 owner=" << ownerName;
        } else {
            ok = OperateDB::instance().renewShareLink(ownerName, relativePath,
                                                      QDateTime::currentDateTime().addSecs(renewSeconds));
            if(ok){
                linkValue = existing.tokenHash;   // 续期不改 token，链接文本不变、只是重新可用
                state = PUBLINK_STATE_VALID;
            }
        }
    } else if(hasExisting){
        // F-1 修复：同一 owner+path 已有链接时，原实现仍返回"新生成但未入库"的 token，
        // 该链接永远无法解析（库里只有旧记录的 hash）。原始 token 不可反推，但
        // resolvePublishLink 支持 64 位十六进制 hash 直接查库，故复用时返回已存 hash 形式链接。
        linkValue = existing.tokenHash;
        ok = true;
        qCDebug(lcFs) << "[createPublishLink] 复用已有链接，返回 hash 形式链接（可直接解析）owner=" << ownerName;
    } else {
        const QString token = ShareLinkService::generateToken();
        ok = OperateDB::instance().createShareLink(ShareLinkService::hashToken(token), ownerName,
                                                   relativePath, ShareLinkService::defaultExpireTime());
        if(ok){
            linkValue = token;
        } else {
            qCWarning(lcFs) << "[createPublishLink] 写入数据库失败，生成分享链接未完成";
        }
    }

    qCDebug(lcFs) << "[createPublishLink] action=" << action << " ok=" << ok << " state=" << state;
    PDU* respdu = mkPDU(128);
    respdu->uiType = ENUM_MSG_TYPE_CREATE_PUBLINK_RESPOND;
    memcpy(respdu->caData, &ok, sizeof(ok));
    // 状态字回真实状态而不是"ok 就当有效"：旧语义复用的可能正是一条过期链接，
    // 客户端要靠这一格才知道拿到的是死链、该给重新生成/续期的选择
    memcpy(respdu->caData + PUBLINK_STATE_OFFSET, &state, sizeof(state));
    if(ok) {
        QString link = QString("cloud-disk://share/%1").arg(linkValue);
        QByteArray linkBytes = link.toUtf8();
        memcpy(respdu->caMsg, linkBytes.constData(), linkBytes.size());
    }
    return respdu;
}



// 新增：安全外链分享（BUG-ARC-PUBLINK-001）
PDU* MsgHandler::resolvePublishLink(const QString& loginName) {
    QString token = fixedString(pdu->caMsg, pdu->uiMsgLen);
    qCDebug(lcFs) << "[resolvePublishLink] 收到解析请求: token_len=" << token.size()
             << " token_head=" << token.left(16);

    // 兼容两种输入：
    //  - 原始 token（22 位 base64url，来自「生成分享链接」弹窗）
    //  - token_hash 本身（64 位十六进制，来自「获取分享链接」弹窗 / 模拟脚本输出）
    // 若输入正好是 64 位十六进制，则直接当作 hash 查库；否则先 hash(token) 再查。
    bool isHex64 = (token.size() == 64);
    if (isHex64) {
        for (const QChar &c : token) {
            if (!c.isDigit() && !((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
                isHex64 = false;
                break;
            }
        }
    }
    QString hash = isHex64 ? token : ShareLinkService::hashToken(token);
    qCDebug(lcFs) << "[resolvePublishLink] hash 来源:" << (isHex64 ? "直接输入(64hex)" : "hashToken(输入)");

    OperateDB::ShareLinkInfo info;
    PDU* respdu = mkPDU(1024); // 预留 JSON 空间
    respdu->uiType = ENUM_MSG_TYPE_RESOLVE_PUBLINK_RESPOND;

    bool ok = false;
    QString savedRelPath; // 形如 "分享链接/2.pdf"，返回给客户端展示
    if (OperateDB::instance().getShareLinkByHash(hash, info)) {
        qCDebug(lcFs) << "[resolvePublishLink] 命中记录: owner=" << info.ownerName
                 << " relativePath=" << info.relativePath
                 << " revoked=" << info.revoked
                 << " expireTime=" << info.expireTime.toString("yyyy-MM-dd HH:mm:ss");
        if (info.revoked) {
            qCWarning(lcFs) << "[resolvePublishLink] 链接已撤销，拒绝";
        } else if (QDateTime::currentDateTime() >= info.expireTime) {
            qCWarning(lcFs) << "[resolvePublishLink] 链接已过期，拒绝";
        } else {
            QString ownerRoot = QDir(Server::getinstance().filesPath).filePath(info.ownerName);
            QString srcPath = QDir(ownerRoot).filePath(info.relativePath);
            QFileInfo srcFi(srcPath);
            if (ShareLinkService::validatePath(ownerRoot, info.relativePath)
                    && srcFi.exists() && srcFi.isFile()) {
                // 目标目录：服务端【请求者】用户目录下的“分享链接”文件夹
                QString userRoot = QDir(Server::getinstance().filesPath).filePath(loginName);
                QString shareDir = QDir(userRoot).filePath("分享链接");
                QDir().mkpath(shareDir);

                QString baseName = srcFi.completeBaseName();
                QString suffix = srcFi.suffix();
                if (!suffix.isEmpty()) suffix = QString(".") + suffix;

                // 同名去重，复用 movefile 的 (1)(2) 规则
                QString finalDstPath = QDir(shareDir).filePath(srcFi.fileName());
                if (QFileInfo::exists(finalDstPath)) {
                    int count = 1;
                    const int MAX_RETRIES = 1000;
                    do {
                        QString numberedName = QString("%1(%2)%3").arg(baseName).arg(count++).arg(suffix);
                        finalDstPath = QDir(shareDir).filePath(numberedName);
                        if (count > MAX_RETRIES) { finalDstPath.clear(); break; }
                    } while (QFileInfo::exists(finalDstPath));
                }

                if (!finalDstPath.isEmpty()) {
                    QFile srcFile(srcPath);
                    if (srcFile.copy(finalDstPath)) {
                        ok = true;
                        savedRelPath = "分享链接/" + QFileInfo(finalDstPath).fileName();
                        qCDebug(lcFs) << "[resolvePublishLink] 已复制分享文件到:" << finalDstPath;
                    } else {
                        qCWarning(lcFs) << "[resolvePublishLink] 复制失败:" << srcFile.errorString()
                                 << " src=" << srcPath << " dst=" << finalDstPath;
                    }
                } else {
                    qCWarning(lcFs) << "[resolvePublishLink] 同名文件过多，放弃复制";
                }
            } else {
                qCWarning(lcFs) << "[resolvePublishLink] 源文件不存在或越权: " << srcPath;
            }
        }
    } else {
        qCDebug(lcFs) << "[resolvePublishLink] 未找到匹配记录 (hash_head="
                 << hash.left(16) << "...)";
    }

    memcpy(respdu->caData, &ok, sizeof(ok));
    if (ok) {
        QString json = QString("{\"saved_path\":\"%1\"}").arg(savedRelPath);
        QByteArray jsonBytes = json.toUtf8();
        memcpy(respdu->caMsg, jsonBytes.constData(), jsonBytes.size());
    }
    return respdu;
}

PDU* MsgHandler::queryFilePublishLink() {
    // REQ-EXT-SEC-001：owner 以连接级 actor 生成，PDU 用户字段仅作请求数据
    QString relativePath = fixedString(pdu->caMsg, pdu->uiMsgLen);
    QString ownerName = m_actorName;
    if(fixedString(pdu->caData, 32) != ownerName){
        qCWarning(lcFs)<<"[queryFilePublishLink] owner 字段与连接身份不符，按连接身份查询 conn="<<m_connId;
    }
    
    QString token = OperateDB::instance().getShareLinkByPath(ownerName, relativePath);

    PDU* respdu = mkPDU(128);
    respdu->uiType = ENUM_MSG_TYPE_QUERY_FILE_PUBLINK_RESPOND;
    bool exists = !token.isEmpty();
    memcpy(respdu->caData, &exists, sizeof(exists));
    // REQ-EXT-SHARE-001：回带状态字——只有 exists 时旧版客户端看不出链接是否已过期/撤销，
    // 只能等解析失败才知道；这一格让"获取分享链接"当场给出重新生成/续期的选择
    quint32 state = PUBLINK_STATE_VALID;
    if(exists){
        OperateDB::ShareLinkInfo info;
        if(OperateDB::instance().getShareLinkInfoByPath(ownerName, relativePath, info)){
            const bool expired = QDateTime::currentDateTime() >= info.expireTime;
            state = info.revoked ? PUBLINK_STATE_REVOKED
                                 : (expired ? PUBLINK_STATE_EXPIRED : PUBLINK_STATE_VALID);
        }
    }
    memcpy(respdu->caData + PUBLINK_STATE_OFFSET, &state, sizeof(state));
    if (exists) {
        QString link = QString("cloud-disk://share/%1").arg(token);
        QByteArray linkBytes = link.toUtf8();
        memcpy(respdu->caMsg, linkBytes.constData(), linkBytes.size());
    }
    return respdu;
}

namespace {
uint groupRespondType(uint requestType)
{
    switch (requestType) {
    case ENUM_MSG_TYPE_CREATE_GROUP_REQUEST: return ENUM_MSG_TYPE_CREATE_GROUP_RESPOND;
    case ENUM_MSG_TYPE_INVITE_GROUP_MEMBER_REQUEST: return ENUM_MSG_TYPE_INVITE_GROUP_MEMBER_RESPOND;
    case ENUM_MSG_TYPE_ACCEPT_GROUP_INVITATION_REQUEST: return ENUM_MSG_TYPE_ACCEPT_GROUP_INVITATION_RESPOND;
    case ENUM_MSG_TYPE_REJECT_GROUP_INVITATION_REQUEST: return ENUM_MSG_TYPE_REJECT_GROUP_INVITATION_RESPOND;
    case ENUM_MSG_TYPE_LEAVE_GROUP_REQUEST: return ENUM_MSG_TYPE_LEAVE_GROUP_RESPOND;
    case ENUM_MSG_TYPE_REMOVE_GROUP_MEMBER_REQUEST: return ENUM_MSG_TYPE_REMOVE_GROUP_MEMBER_RESPOND;
    case ENUM_MSG_TYPE_SET_GROUP_ROLE_REQUEST: return ENUM_MSG_TYPE_SET_GROUP_ROLE_RESPOND;
    case ENUM_MSG_TYPE_TRANSFER_GROUP_OWNER_REQUEST: return ENUM_MSG_TYPE_TRANSFER_GROUP_OWNER_RESPOND;
    case ENUM_MSG_TYPE_DISSOLVE_GROUP_REQUEST: return ENUM_MSG_TYPE_DISSOLVE_GROUP_RESPOND;
    case ENUM_MSG_TYPE_LIST_GROUPS_REQUEST: return ENUM_MSG_TYPE_LIST_GROUPS_RESPOND;
    case ENUM_MSG_TYPE_LIST_GROUP_MEMBERS_REQUEST: return ENUM_MSG_TYPE_LIST_GROUP_MEMBERS_RESPOND;
    case ENUM_MSG_TYPE_QUERY_GROUP_INVITATIONS_REQUEST: return ENUM_MSG_TYPE_QUERY_GROUP_INVITATIONS_RESPOND;
    case ENUM_MSG_TYPE_QUERY_GROUP_SUMMARY_REQUEST: return ENUM_MSG_TYPE_QUERY_GROUP_SUMMARY_RESPOND;
    // REQ-EXT-GROUP-002：群消息
    case ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_REQUEST: return ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_RESPOND;
    case ENUM_MSG_TYPE_GROUP_PULL_MESSAGES_REQUEST: return ENUM_MSG_TYPE_GROUP_PULL_MESSAGES_RESPOND;
    case ENUM_MSG_TYPE_GROUP_READ_CONFIRM_REQUEST: return ENUM_MSG_TYPE_GROUP_READ_CONFIRM_RESPOND;
    case ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_REQUEST: return ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_RESPOND;
    default: return ENUM_MSG_TYPE_MAX;
    }
}

OperateDB::GroupOperation groupOperation(uint requestType)
{
    switch (requestType) {
    case ENUM_MSG_TYPE_CREATE_GROUP_REQUEST: return OperateDB::GroupCreate;
    case ENUM_MSG_TYPE_INVITE_GROUP_MEMBER_REQUEST: return OperateDB::GroupInvite;
    case ENUM_MSG_TYPE_ACCEPT_GROUP_INVITATION_REQUEST: return OperateDB::GroupAcceptInvitation;
    case ENUM_MSG_TYPE_REJECT_GROUP_INVITATION_REQUEST: return OperateDB::GroupRejectInvitation;
    case ENUM_MSG_TYPE_LEAVE_GROUP_REQUEST: return OperateDB::GroupLeave;
    case ENUM_MSG_TYPE_REMOVE_GROUP_MEMBER_REQUEST: return OperateDB::GroupRemoveMember;
    case ENUM_MSG_TYPE_SET_GROUP_ROLE_REQUEST: return OperateDB::GroupSetRole;
    case ENUM_MSG_TYPE_TRANSFER_GROUP_OWNER_REQUEST: return OperateDB::GroupTransferOwner;
    case ENUM_MSG_TYPE_DISSOLVE_GROUP_REQUEST: return OperateDB::GroupDissolve;
    case ENUM_MSG_TYPE_LIST_GROUPS_REQUEST: return OperateDB::GroupList;
    case ENUM_MSG_TYPE_LIST_GROUP_MEMBERS_REQUEST: return OperateDB::GroupListMembers;
    case ENUM_MSG_TYPE_QUERY_GROUP_INVITATIONS_REQUEST: return OperateDB::GroupQueryInvitations;
    case ENUM_MSG_TYPE_QUERY_GROUP_SUMMARY_REQUEST: return OperateDB::GroupSummary;
    // REQ-EXT-GROUP-002：群消息
    case ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_REQUEST: return OperateDB::GroupSendMessage;
    case ENUM_MSG_TYPE_GROUP_PULL_MESSAGES_REQUEST: return OperateDB::GroupPullMessages;
    case ENUM_MSG_TYPE_GROUP_READ_CONFIRM_REQUEST: return OperateDB::GroupReadConfirm;
    case ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_REQUEST: return OperateDB::GroupUnreadSummary;
    default: return OperateDB::GroupList;
    }
}
}

PDU* MsgHandler::groupRequest(uint requestType)
{
    const uint responseType = groupRespondType(requestType);
    if (responseType == ENUM_MSG_TYPE_MAX) return nullptr;
    const QByteArray payload(pdu->caMsg, int(pdu->uiMsgLen));
    int offset = 0;
    quint16 version = 0;
    quint16 envelopeLength = 0;
    quint64 requestId = 0;
    quint64 groupId = 0;
    auto response = [&](int code, const QByteArray &body, quint64 id, quint64 gid) {
        QByteArray encoded;
        groupAppendU16(encoded, GROUP_PROTOCOL_VERSION);
        groupAppendU16(encoded, GROUP_ENVELOPE_LENGTH);
        groupAppendU64(encoded, id);
        groupAppendU64(encoded, gid);
        groupAppendU32(encoded, quint32(code));
        encoded.append(body);
        PDU *out = mkPDU(uint(encoded.size()));
        out->uiType = responseType;
        memcpy(out->caMsg, encoded.constData(), size_t(encoded.size()));
        return out;
    };
    if (!groupReadU16(payload, offset, version) || !groupReadU16(payload, offset, envelopeLength)
            || !groupReadU64(payload, offset, requestId) || !groupReadU64(payload, offset, groupId)
            || envelopeLength != GROUP_ENVELOPE_LENGTH || version != GROUP_PROTOCOL_VERSION
            || payload.size() < envelopeLength) {
        qCWarning(lcNet) << "[GROUP] 拒绝非法信封 type=" << requestType << "conn=" << m_connId;
        return response(-2001, QByteArray(), requestId, groupId);
    }

    OperateDB::GroupRequest request;
    request.operation = groupOperation(requestType);
    request.groupId = groupId;
    request.invitationId = 0;
    QByteArray value;
    auto readText = [&](QString &target) {
        if (!groupReadString(payload, offset, value)) return false;
        target = QString::fromUtf8(value);
        return true;
    };
    bool parsed = true;
    switch (request.operation) {
    case OperateDB::GroupCreate:
        parsed = readText(request.groupName); break;
    case OperateDB::GroupInvite:
        parsed = readText(request.targetName) && readText(request.requestKey); break;
    case OperateDB::GroupAcceptInvitation:
    case OperateDB::GroupRejectInvitation:
        parsed = groupReadU64(payload, offset, request.invitationId); break;
    case OperateDB::GroupRemoveMember:
    case OperateDB::GroupTransferOwner:
        parsed = readText(request.targetName); break;
    case OperateDB::GroupSetRole:
        parsed = readText(request.targetName) && readText(request.role); break;
    // REQ-EXT-GROUP-002：群消息（发送方身份一律取 actor，载荷无 sender 字段）
    case OperateDB::GroupSendMessage:
        parsed = readText(request.clientMsgId) && readText(request.content);
        if (parsed && request.clientMsgId.toUtf8().size() > 64) parsed = false;
        if (parsed && request.content.toUtf8().size() > 2000) parsed = false;
        break;
    case OperateDB::GroupPullMessages: {
        if (offset + 1 > payload.size()) { parsed = false; break; }
        request.pullMode = quint8(payload.at(offset++));
        parsed = (request.pullMode <= 1)
                && groupReadU64(payload, offset, request.cursorSeq)
                && groupReadU16(payload, offset, request.limit);
        if (parsed && (request.limit < 1 || request.limit > 100)) parsed = false; // 越界载荷拒绝（客户端同值钳制）
        break;
    }
    case OperateDB::GroupReadConfirm:
        parsed = groupReadU64(payload, offset, request.readSeq); break;
    case OperateDB::GroupUnreadSummary:
        break;
    default:
        break;
    }
    if (!parsed || offset != payload.size()) {
        qCWarning(lcNet) << "[GROUP] 拒绝越界载荷 type=" << requestType << "conn=" << m_connId;
        // REQ-EXT-GROUP-002：发送请求解析失败也回带幂等键（可解析到时），客户端据此同键重试
        QByteArray rejectBody;
        if (request.operation == OperateDB::GroupSendMessage && !request.clientMsgId.isEmpty()) {
            groupAppendString(rejectBody, request.clientMsgId.toUtf8());
        }
        return response(-2002, rejectBody, requestId, groupId);
    }

    const OperateDB::GroupResult result = OperateDB::instance().handleGroupOperation(request, m_actorName);
    // REQ-EXT-GROUP-002 T3/T5：先提交后广播——仅在 DB 提交成功（code==0 且 broadcastMessage）
    // 后触发；89=已持久化，90=在线送达，语义分离。幂等重放不重复广播。
    if (result.code == 0 && result.broadcastMessage && !result.notifyUsers.isEmpty()) {
        QByteArray msgBody;
        groupAppendU16(msgBody, GROUP_PROTOCOL_VERSION);
        groupAppendU16(msgBody, GROUP_ENVELOPE_LENGTH);
        groupAppendU64(msgBody, 0);
        groupAppendU64(msgBody, result.groupId);
        groupAppendU32(msgBody, 0);
        groupAppendU64(msgBody, result.groupId);
        groupAppendU64(msgBody, result.message.seq);
        groupAppendU64(msgBody, result.message.msgId);
        groupAppendString(msgBody, result.message.sender.toUtf8());
        msgBody.append(char(result.message.msgType));
        groupAppendU64(msgBody, result.message.createdAtMs);
        groupAppendString(msgBody, result.message.content.toUtf8());
        PDU *msgEvent = mkPDU(uint(msgBody.size()));
        msgEvent->uiType = ENUM_MSG_TYPE_GROUP_MESSAGE_EVENT;
        memcpy(msgEvent->caMsg, msgBody.constData(), size_t(msgBody.size()));
        MyTcpServer::getinstance().broadcastGroupMessage(result.notifyUsers, msgEvent);
    }
    if (!result.notifyUsers.isEmpty() && result.eventType != 0) {
        QByteArray eventBody;
        groupAppendU16(eventBody, GROUP_PROTOCOL_VERSION);
        groupAppendU16(eventBody, GROUP_ENVELOPE_LENGTH);
        groupAppendU64(eventBody, 0);
        groupAppendU64(eventBody, result.groupId);
        groupAppendU32(eventBody, 0);
        groupAppendU64(eventBody, result.version);
        groupAppendU32(eventBody, quint32(result.eventType));
        PDU *event = mkPDU(uint(eventBody.size()));
        event->uiType = result.eventType == 2 ? ENUM_MSG_TYPE_GROUP_INVITATION_EVENT
                                              : ENUM_MSG_TYPE_GROUP_MEMBER_EVENT;
        memcpy(event->caMsg, eventBody.constData(), size_t(eventBody.size()));
        MyTcpServer::getinstance().sendGroupEvent(result.notifyUsers, event);
    }
    if (request.operation == OperateDB::GroupSendMessage && result.code != 0
            && result.responseBody.isEmpty() && !request.clientMsgId.isEmpty()) {
        // 失败体回带幂等键：客户端据此把对应 pending 项标为「发送失败，双击重试」
        QByteArray retryBody;
        groupAppendString(retryBody, request.clientMsgId.toUtf8());
        return response(result.code, retryBody, requestId, request.groupId == 0 ? groupId : request.groupId);
    }
    return response(result.code, result.responseBody, requestId, result.groupId == 0 ? groupId : result.groupId);
}

namespace {
uint groupFileRespondType(uint requestType)
{
    switch (requestType) {
    case ENUM_MSG_TYPE_GROUP_FILE_LIST_REQUEST: return ENUM_MSG_TYPE_GROUP_FILE_LIST_RESPOND;
    case ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_REQUEST: return ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_RESPOND;
    case ENUM_MSG_TYPE_GROUP_FILE_DELETE_REQUEST: return ENUM_MSG_TYPE_GROUP_FILE_DELETE_RESPOND;
    case ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_REQUEST: return ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_RESPOND;
    default: return ENUM_MSG_TYPE_MAX;
    }
}
}

// ===== REQ-EXT-GROUP-003：群文件请求（99/101/103/105）=====
// 客户端不自报身份、不给出任何物理路径；载荷只有 offset/limit、size/sha/name、group_file_id。
// 信封与边界先验（version/envelope_length/长度域/尾随字节），越界一律拒绝且不落明文。
// 授权判定全部在 OperateDB::handleGroupFileOperation（服务端每请求即时判定）。
PDU* MsgHandler::groupFileRequest(uint requestType)
{
    const uint responseType = groupFileRespondType(requestType);
    if (responseType == ENUM_MSG_TYPE_MAX) return nullptr;
    m_downloadCanonicalPath.clear();

    const QByteArray payload(pdu->caMsg, int(pdu->uiMsgLen));
    int offset = 0;
    quint16 version = 0, envelopeLength = 0;
    quint64 requestId = 0, groupId = 0;
    auto response = [&](int code, const QByteArray &body) {
        QByteArray encoded;
        groupAppendU16(encoded, GROUP_PROTOCOL_VERSION);
        groupAppendU16(encoded, GROUP_ENVELOPE_LENGTH);
        groupAppendU64(encoded, requestId);
        groupAppendU64(encoded, groupId);
        groupAppendU32(encoded, quint32(code));
        encoded.append(body);
        PDU *out = mkPDU(uint(encoded.size()));
        out->uiType = responseType;
        // 102/106 回带 transfer_id（R3）；其余群文件响应 caData 保持全零
        if (responseType == ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_RESPOND
                || responseType == ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_RESPOND) {
            memcpy(out->caData + TRANSFER_ID_OFFSET, &m_currentTransferId, sizeof(m_currentTransferId));
        }
        // REQ-EXT-TRANSFER-002 S4/D4：106 的 [24..31] = 服务端实际采纳的续传起点，
        // 仅受理且将建流时写入（R4）；其余区间保持全零，caMsg 信封零改动（I7）。
        if (responseType == ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_RESPOND
                && !m_downloadCanonicalPath.isEmpty()) {
            const qint64 accepted = m_downloadResumeOffset;
            memcpy(out->caData + DOWNLOAD_ACCEPTED_OFFSET, &accepted, sizeof(accepted));
        }
        memcpy(out->caMsg, encoded.constData(), size_t(encoded.size()));
        return out;
    };
    if (!groupReadU16(payload, offset, version) || !groupReadU16(payload, offset, envelopeLength)
            || !groupReadU64(payload, offset, requestId) || !groupReadU64(payload, offset, groupId)
            || envelopeLength != GROUP_ENVELOPE_LENGTH || version != GROUP_PROTOCOL_VERSION
            || payload.size() < envelopeLength) {
        qCWarning(lcNet) << "[GROUPFILE] 拒绝非法信封 type=" << requestType << " conn=" << m_connId;
        return response(-2001, QByteArray());
    }

    OperateDB::GroupFileRequest request;
    request.groupId = groupId;
    QByteArray value;
    bool parsed = true;
    if (requestType == ENUM_MSG_TYPE_GROUP_FILE_LIST_REQUEST) {
        request.operation = OperateDB::GroupFileList;
        parsed = groupReadU32(payload, offset, request.offset)
                && groupReadU16(payload, offset, request.limit);
        if (parsed && (request.limit < 1 || request.limit > uint(GROUP_FILE_MAX_LIST_LIMIT))) parsed = false;
    } else if (requestType == ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_REQUEST) {
        request.operation = OperateDB::GroupFileUploadInit;
        quint64 fileSize = 0;
        parsed = groupReadU64(payload, offset, fileSize);
        request.fileSize = qint64(fileSize);
        if (parsed && !groupReadString(payload, offset, value)) parsed = false;
        else if (parsed) request.shaHex = QString::fromLatin1(value);
        if (parsed && !groupReadString(payload, offset, value)) parsed = false;
        else if (parsed) request.displayName = QString::fromUtf8(value);
        if (parsed) {
            // 摘要严格 [0-9a-f]{64}（客户端 toHex 产出小写）
            if (request.shaHex.size() != 64) parsed = false;
            for (int i = 0; parsed && i < request.shaHex.size(); ++i) {
                const QChar c = request.shaHex.at(i);
                if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9'))
                      || (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) parsed = false;
            }
            // P6 名称合法性：非空、≤255B、无 '/' '\\' NUL
            const QByteArray nb = request.displayName.toUtf8();
            if (parsed && (request.displayName.isEmpty() || nb.size() > 255
                           || request.displayName.contains(QLatin1Char('/'))
                           || request.displayName.contains(QLatin1Char('\\'))
                           || request.displayName.contains(QChar('\0')))) parsed = false;
        }
    } else if (requestType == ENUM_MSG_TYPE_GROUP_FILE_DELETE_REQUEST) {
        request.operation = OperateDB::GroupFileDelete;
        quint64 gfid = 0;
        parsed = groupReadU64(payload, offset, gfid);
        request.groupFileId = gfid;
    } else {
        request.operation = OperateDB::GroupFileDownload;
        quint64 gfid = 0;
        parsed = groupReadU64(payload, offset, gfid);
        request.groupFileId = gfid;
    }
    if (!parsed || offset != payload.size() || request.groupId == 0) {
        qCWarning(lcNet) << "[GROUPFILE] 拒绝越界载荷 type=" << requestType << " conn=" << m_connId;
        return response(-2002, QByteArray());
    }

    const OperateDB::GroupFileResult result =
            OperateDB::instance().handleGroupFileOperation(request, m_actorName);
    int code = result.code;

    if (request.operation == OperateDB::GroupFileList) {
        return response(code, code == 0 ? result.responseBody : QByteArray());
    }
    if (request.operation == OperateDB::GroupFileDelete) {
        QByteArray body;
        groupAppendU64(body, request.groupFileId);
        return response(code, body);
    }
    if (request.operation == OperateDB::GroupFileDownload) {
        QByteArray body;
        if (code == 0 && !m_downloadSlotAvailable) {
            code = -2107;   // D7：本连接并发下载已达上限
            qCWarning(lcFs) << "[GROUPFILE] 拒绝：下载并发上限 conn=" << m_connId
                            << " id=" << m_currentTransferId;
        }
        if (code == 0) {
            // 服务端解析物理路径（group_file.object_id → file_object.storage_path），
            // 绝不接受客户端路径；后续正文走既有 42/43 分块（MyTcpSocket 触发）。
            const QString absPath = FileObjectStore::instance().objectAbsPath(request.objectId);
            const qint64 effSize = request.sizeBytes < 0 ? 0 : request.sizeBytes;
            if (absPath.isEmpty() || !QFileInfo(absPath).isFile()) {
                code = -2105;
            } else if (m_downloadResumeOffset > effSize) {
                // REQ-EXT-TRANSFER-002 D1/§2.3：续传起点非法 → 明确拒绝、不建流（-2107 已被并发上限占用）
                code = -2108;
                qCWarning(lcFs) << "[GROUPFILE] 拒绝：续传起点非法 conn=" << m_connId
                                << " id=" << m_currentTransferId << " offset=" << m_downloadResumeOffset
                                << " size=" << effSize;
            } else {
                m_downloadCanonicalPath = absPath;
                const QByteArray nameBytes = request.displayName.toUtf8();
                groupAppendU64(body, quint64(effSize));
                groupAppendU32(body, quint32(nameBytes.size()));
                body.append(nameBytes);
            }
        }
        if (code != 0) body.clear();
        return response(code, body);
    }

    // UPLOAD_INIT（101）：命中=零正文；未命中=建立暂存会话（staging + upload_session）后走既有分块
    bool hit = result.hit;
    qint64 fileId = -1, sessionId = -1, nextOffset = 0;
    quint64 groupFileIdOut = 0;
    QString finalName = request.displayName;
    if (code == 0) {
        if (hit) {
            fileId = request.hostFileId;
            groupFileIdOut = result.groupFileId;
            nextOffset = request.fileSize;   // 命中零正文：续传起点=文件尾
        } else if (!acquireUploadSlot(m_currentTransferId)) {
            // D7 硬上限 / T-17 重复 id：拒绝且不建状态（P7：不回退到 id=0）
            code = -2106;
            qCWarning(lcFs) << "[GROUPFILE] 拒绝：并发上限或 id 冲突 conn=" << m_connId
                            << " id=" << m_currentTransferId << " active=" << activeUploadCount();
        } else {
            const quint64 tid = m_currentTransferId;
            UploadCtx& ctx = m_uploads[tid];
            ctx.id = tid;
            ctx.sessionId = -1;
            ctx.expectedSha.clear();
            ctx.groupId = 0;
            ctx.groupDisplayName.clear();
            ctx.writeInFlight = false;
            ctx.lastProgressMs = QDateTime::currentMSecsSinceEpoch();

            const QString parent = QStringLiteral("/");
            const ResumableUploadSession::BeginResult br = ctx.session.beginStaged(
                        m_actorName, parent, request.displayName, request.fileSize, true);
            if (!br.ok) {
                code = -2105;
                m_uploads.remove(tid);
                qCWarning(lcFs) << "[GROUPFILE] 暂存会话建立失败 conn=" << m_connId
                                << " group=" << request.groupId;
            } else {
                // REQ-EXT-TRANSFER-003 S15/D7：同个人路径，先按暂存键复用未完成行（101 不携带请求键）
                const qint64 reuseId = OperateDB::instance().findReusableUploadSession(
                            m_actorName, ctx.session.stagingFilePath());
                if (reuseId >= 0) {
                    OperateDB::instance().restartUploadSession(
                                reuseId, parent, request.displayName, request.fileSize,
                                request.shaHex, ctx.session.stagingFilePath(), br.offset, 3600);
                    sessionId = reuseId;
                } else {
                    sessionId = OperateDB::instance().createUploadSession(
                                m_actorName, parent, request.displayName, request.fileSize,
                                request.shaHex, ctx.session.stagingFilePath(), br.offset, 3600);
                }
                if (sessionId < 0) {
                    code = -2105;
                    m_uploads.remove(tid);
                    qCWarning(lcFs) << "[GROUPFILE] 上传会话登记失败 conn=" << m_connId;
                } else {
                    ctx.lastPersistOffset = br.offset;   // S8：节流基准随本次续传起点
                    ctx.lastPersistMs = QDateTime::currentMSecsSinceEpoch();
                    ctx.sessionId = sessionId;
                    ctx.expectedSha = request.shaHex;
                    ctx.groupId = request.groupId;
                    ctx.groupDisplayName = request.displayName;
                    nextOffset = br.offset;
                    if (br.complete) {
                        // 0 字节或断点已在尾部：立即落定（服务端复算 + 群引用，同个人路径）
                        const FileObjectStore::CommitResult cr = finalizeUploadOnComplete();
                        if (cr.ok) {
                            hit = true;
                            fileId = cr.fileId;
                            groupFileIdOut = m_lastGroupFileId;
                            nextOffset = request.fileSize;
                        } else {
                            code = -2105;
                        }
                    }
                }
            }
        }
    }
    QByteArray body;
    if (code == 0) {
        const QByteArray nameBytes = finalName.toUtf8();
        body.append(char(hit ? 1 : 0));
        body.append(char(0)); body.append(char(0)); body.append(char(0));   // reserved 3B
        groupAppendU64(body, quint64(fileId < 0 ? 0 : fileId));
        groupAppendU64(body, groupFileIdOut);
        groupAppendU64(body, quint64(sessionId < 0 ? 0 : sessionId));
        groupAppendU64(body, quint64(nextOffset < 0 ? 0 : nextOffset));
        groupAppendU32(body, quint32(nameBytes.size()));
        body.append(nameBytes);
    }
    return response(code, body);
}
