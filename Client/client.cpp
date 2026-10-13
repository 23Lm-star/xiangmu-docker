#include "logging.h"
#include "client.h"
#include "index.h"
#include "ui_client.h"
#include "process.h"
#include "file.h"
#include "transfertask.h"
#include <string.h>
#include <QByteArray>
#include <QFile>
#include <QString>
#include <QDebug>
#include <QMessageBox>
#include <QHostAddress>
#include <QCryptographicHash>
#include <QGraphicsOpacityEffect>
#include <QTimer>
#include <QCoreApplication>
#include <QTextStream>
#include <QDateTime>
#include <QInputDialog>
#include <QSettings>
#include <QScreen>
#include <QUuid>
#include <string>

namespace {
QByteArray passwordHashForProtocol(const QString &password)
{
    return QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Md5).toHex();
}

void copyFixedField(char *dst, int dstSize, const QByteArray &src)
{
    memset(dst, 0, dstSize);
    memcpy(dst, src.constData(), qMin(src.size(), dstSize - 1));
}

void copyExactField(char *dst, int dstSize, const QByteArray &src)
{
    memset(dst, 0, dstSize);
    memcpy(dst, src.constData(), qMin(src.size(), dstSize));
}

// 把剩余秒数格式化成 MM:SS，用于获取验证码按钮的倒计时显示
QString formatCooldown(int sec)
{
    sec = qMax(0, sec);
    int m = sec / 60;
    int s = sec % 60;
    return QString("%1:%2").arg(m, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0'));
}
}

Client::Client(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Client)
{
    setAttribute(Qt::WA_StyledBackground, true);   // QSS 背景在 QWidget 子类上必须显式开启
    ui->setupUi(this);
    setFixedSize(scaledWindowSize(600,420));
    this->m_mhptr=new MsgHandler;
    loadconfig();
    // REQ-EXT-SESSION-001：持久化客户端唯一 ID（首启生成 32hex 落盘，之后复用；登录负载附加区携带）
    {
        QSettings st;
        m_clientId = st.value("client/unique_id").toString();
        if(m_clientId.isEmpty()){
            m_clientId = QUuid::createUuid().toString(QUuid::Id128);
            st.setValue("client/unique_id", m_clientId);
        }
    }
    ui->loadingLabel->hide();
    m_captchaCooldownSec=0;
    m_captchaLocked=false;
    m_captchaCooldownTimer=new QTimer(this);
    connect(m_captchaCooldownTimer,&QTimer::timeout,this,&Client::onCaptchaCooldownTick);
    
    m_heartbeatTimer = new QTimer(this);
    connect(m_heartbeatTimer, &QTimer::timeout, this, &Client::sendHeartbeat);
    m_heartbeatTimer->start(30000);

    // 服务端存活检测初始化
    m_lastPongMs = QDateTime::currentMSecsSinceEpoch();
    m_serverLostShown = false;
    m_loggedIn = false;
    m_reconnectDlg = new ReconnectDialog(this);
    connect(m_reconnectDlg, &ReconnectDialog::reconnectClicked,
            this, &Client::onReconnectRequested);

    sorket.connectToHost(QHostAddress(this->m_strlp),this->m_usPort);
    connect(&sorket,&QTcpSocket::connected,this,&Client::connectsort);
    connect(&sorket,&QTcpSocket::readyRead,this,&Client::recvmsg);
    connect(&sorket,&QTcpSocket::disconnected,this,&Client::onSocketDisconnected);
}

Client::~Client(){
    TransferManager::instance().saveSnapshotNow();   // 退出前把未完成传输落盘，下次登录恢复
    delete ui;
}

void Client::loadconfig()
{
    QFile f(":/connect.config");
    if(f.open(QIODevice::ReadOnly)){
        QString s=QString(f.readAll());
        QStringList str=s.split("\r\n");
        m_strlp=str[0];
        m_usPort=str[1].toUShort();
        m_strRootPath=str[2];
        f.close();
    }
    else {
        QMessageBox::critical(this,"提示","打开文件失败");
    }
}

Client &Client::getinstance(){
    static Client instance;
    return instance;
}

// REQ-UX-001 A5：基准尺寸 ×2，并收敛到主屏可用区域的 95%，避免大窗口超出显示器
QSize Client::scaledWindowSize(int baseW, int baseH)
{
    QSize s(baseW * 2, baseH * 2);
    if(QScreen *scr = QApplication::primaryScreen()){
        const QRect avail = scr->availableGeometry();
        s.setWidth(qMin(s.width(), int(avail.width() * 0.95)));
        s.setHeight(qMin(s.height(), int(avail.height() * 0.95)));
    }
    return s;
}

void Client::flus()
{
    ui->name_LE->clear();
    ui->pwd_LE->clear();
}

bool Client::s_heartbeatEnabled = true;  // 编译期默认开关（想永久关改成 false 再构建）
const qint64 Client::SERVER_ALIVE_TIMEOUT_MS = 90000;  // 与服务端 ZOMBIE 阈值一致

// 调试：心跳模拟假死开关（低耦合，仅作用于 sendHeartbeat，不触碰协议/UI/连接逻辑）
// 运行时切换：在客户端 exe 同目录放置 zombie_sim.txt，内容为 1=暂停发PING(模拟僵尸)，
//             0 或文件不存在=正常发PING。每次心跳都重读文件，因此把 1 改回 0 即可动态恢复。
void Client::sendHeartbeat() {
    // 调试开关：zombie_sim.txt 内容 1=停发(模拟僵尸)，0/不存在=发
    const QString flagFile = QCoreApplication::applicationDirPath() + "/zombie_sim.txt";
    QString rawVal;          // 文件原始内容，用于日志展示
    bool zombie = false;
    QFile f(flagFile);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        rawVal = f.readAll().trimmed();
        f.close();
        zombie = (rawVal == "1");
    } else {
        rawVal = "<文件不存在，默认发PING>";
    }
    // 决策：编译开关关闭 或 文件内容=1 → 不发
    const bool willSend = s_heartbeatEnabled && !zombie;
    qCDebug(lcCli) << "[HEARTBEAT] 读取" << flagFile << "值=" << rawVal
             << "| 编译开关=" << s_heartbeatEnabled
             << "| 决策=" << (willSend ? "发PING" : "不发PING");
    if (!willSend) {
        qCDebug(lcCli) << "[HEARTBEAT] PING 跳过（模拟假死）";
        return;
    }
    // 复用心跳检测服务端存活：距上次收到 PONG 已超过阈值，判定服务端失联
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastPongMs > SERVER_ALIVE_TIMEOUT_MS && !m_serverLostShown) {
        onServerLost();
    }

    PDU* pdu = mkPDU();
    pdu->uiType = ENUM_MSG_TYPE_HEARTBEAT_REQUEST;
    qCDebug(lcCli) << "[HEARTBEAT] send PING";
    seedMsg(pdu);
}

void Client::handlMsg(PDU *&pdu)
{
    m_mhptr->pdu=pdu;
    switch(pdu->uiType){
    case ENUM_MSG_TYPE_HEARTBEAT_RESPOND: {
        m_lastPongMs = QDateTime::currentMSecsSinceEpoch();
        qCDebug(lcCli) << "[HEARTBEAT] recv PONG";
        break;
    }
    case ENUM_MSG_TYPE_REGIST_RESPOND:{
        m_mhptr->handlrigist();
        break;
    }
    case ENUM_MSG_TYPE_LOGIN_RESPOND:{
        // R-1（REQ-EXT-TRANSFER-003）：登录名在同步路径就位——handllogin() 直接返回名字，
        // 不再等 1.5 s 的 UI 定时器写 m_LoginName（否则首次登录 setCurrentUser 收到空名 →
        // 快照整轮不落盘；同进程再登录则清表重建 → 在途任务从列表消失）。
        const QString loginName = m_mhptr->handllogin();
        if (!loginName.isEmpty()) m_LoginName = loginName;
        // 按用户载入未完成传输快照（切用户会先落盘上一个用户的内存任务）
        TransferManager::instance().setCurrentUser(m_LoginName);
        // REQ-EXT-TRANSFER-003 G7/C7：重连登录后恢复调度（不依赖 setCurrentUser 的提前返回——同用户重连会提前返回）
        TransferManager::instance().onConnectionRestored();
        m_loggedIn = true;   // 登录过才监控断开，避免登录界面误报
        break;
    }
    case ENUM_MSG_TYPE_findUser_RESPOND:{
        m_mhptr->handlfinduser();
        break;
    }
    case ENUM_MSG_TYPE_ONLINEUSER_RESPOND:{
        m_mhptr->handlonlineuser();
        break;
    }
    case ENUM_MSG_TYPE_ADDFRIEND_RESPOND:{
        m_mhptr->handlAddFriend();
        break;
    }
    case ENUM_MSG_TYPE_ADDFRIEND_REQUEST:{
        m_mhptr->handlAddFriendAgree(m_LoginName);
        break;
    }
    case ENUM_MSG_TYPE_FLUSHFRIEND_RESPOND:{
        m_mhptr->handlFlushFriend();
        break;
    }
    case ENUM_MSG_TYPE_DELETEFRIEND_RESPOND:{
        m_mhptr->handlDeleteFriend();
        break;
    }
    case ENUM_MSG_TYPE_CHAT_REQUEST:{
        m_mhptr->chat();
        break;
    }
    case ENUM_MSG_TYPE_GETCHATMSG_RESPOND:{
        m_mhptr->getcharmsg();
        break;
    }
    case ENUM_MSG_TYPE_MKDIR_RESPOND:{
        m_mhptr->mkdir();
        break;
    }
    case ENUM_MSG_TYPE_CHANGEDIR_RESPOND:{
        m_mhptr->changedir();
        break;
    }
    case ENUM_MSG_TYPE_DELFILE_RESPOND:{
        m_mhptr->deldir();
        break;
    }
    case ENUM_MSG_TYPE_RENAMEFILE_RESPOND:{
        m_mhptr->renamedir();
        break;
    }
    case ENUM_MSG_TYPE_FLUSHFILE_RESPOND:{
        m_mhptr->flushfile();
        break;
    }
    case ENUM_MSG_TYPE_SHAREFILE_RESPOND:{
        QMessageBox::information(&Index::getinstance(),"提示","文件分享完成");
        break;
    }
    case ENUM_MSG_TYPE_SHAREFILE_AGREE_REQUEST:{
        m_mhptr->sharefileagree();
        break;
    }
    case ENUM_MSG_TYPE_SHAREFILE_FINISH_RESPOND:{
        m_mhptr->sharefilefinsh();
        break;
    }
    // REQ-EXT-TRANSFER-001：32 确认按 transfer_id 路由到该上传任务（id=0 时无任务，忽略）
    case ENUM_MSG_TYPE_UPLOADFILE_DATA_RESPOND:{
        TransferManager::instance().onUploadDataRespond(pdu);
        break;
    }
    // REQ-EXT-TRANSFER-001：30 初始化响应按 id 路由（个人上传握手 = 97 → 29 → 30）
    case ENUM_MSG_TYPE_UPLOADFILE_INIT_RESPOND:{
        TransferManager::instance().onUploadInitRespond(pdu);
        break;
    }
    // REQ-EXT-TRANSFER-001：98 秒传预检查响应（无 transfer_id，按连接级单飞槽归属）
    case ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_RESPOND:{
        TransferManager::instance().onFastcheckRespond(pdu);
        break;
    }
    // REQ-EXT-TRANSFER-001：107/108 上传任务释放响应
    case ENUM_MSG_TYPE_UPLOADFILE_ABORT_RESPOND:{
        TransferManager::instance().onAbortRespond(pdu);
        break;
    }
    // REQ-EXT-TRANSFER-001：41/42/43 按 id 路由到该下载任务的接收槽（§5.4）
    case ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND:{
        TransferManager::instance().onDownloadRespond(pdu);
        break;
    }
    case ENUM_MSG_TYPE_DOWNLOADFILE_DATA:{
        TransferManager::instance().onDownloadData(pdu);
        break;
    }
    case ENUM_MSG_TYPE_DOWNLOADFILE_FINISH:{
        TransferManager::instance().onDownloadFinish(pdu);
        break;
    }
    case ENUM_MSG_TYPE_CAPTCHA_RESPOND:{
        m_mhptr->handleCaptcha();
        break;
    }
    case ENUM_MSG_TYPE_MOVEFILE_RESPOND:{
        m_mhptr->handlMoveFile();
        break;
    }
    case ENUM_MSG_TYPE_LISTDIR_RESPOND:{
        Index::getinstance().getFile()->handleListDirRespond(pdu);
        break;
    }
    case ENUM_MSG_TYPE_GETUSERINFORMATION_RESPOND:{
        m_mhptr->handlUserInformation();
        break;
    }
    case ENUM_MSG_TYPE_LOGOFF_RESPOND:{
        m_mhptr->handlLogoff();
        break;
    }
    case ENUM_MSG_TYPE_QUERY_FILE_PUBLINK_RESPOND:
        m_mhptr->handleQueryFilePublishLink(pdu);
        break;
    case ENUM_MSG_TYPE_CREATE_PUBLINK_RESPOND:
        m_mhptr->handleCreatePublishLink(pdu);
        break;
    case ENUM_MSG_TYPE_RESOLVE_PUBLINK_RESPOND:
        m_mhptr->handleResolvePublishLink(pdu);
        break;
    case ENUM_MSG_TYPE_CREATE_GROUP_RESPOND:
    case ENUM_MSG_TYPE_INVITE_GROUP_MEMBER_RESPOND:
    case ENUM_MSG_TYPE_ACCEPT_GROUP_INVITATION_RESPOND:
    case ENUM_MSG_TYPE_REJECT_GROUP_INVITATION_RESPOND:
    case ENUM_MSG_TYPE_LEAVE_GROUP_RESPOND:
    case ENUM_MSG_TYPE_REMOVE_GROUP_MEMBER_RESPOND:
    case ENUM_MSG_TYPE_SET_GROUP_ROLE_RESPOND:
    case ENUM_MSG_TYPE_TRANSFER_GROUP_OWNER_RESPOND:
    case ENUM_MSG_TYPE_DISSOLVE_GROUP_RESPOND:
    case ENUM_MSG_TYPE_LIST_GROUPS_RESPOND:
    case ENUM_MSG_TYPE_LIST_GROUP_MEMBERS_RESPOND:
    case ENUM_MSG_TYPE_QUERY_GROUP_INVITATIONS_RESPOND:
    case ENUM_MSG_TYPE_QUERY_GROUP_SUMMARY_RESPOND:
    case ENUM_MSG_TYPE_GROUP_MEMBER_EVENT:
    case ENUM_MSG_TYPE_GROUP_INVITATION_EVENT:
    case ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_RESPOND:
        Index::getinstance().getGroup()->handleResponse(pdu);
        break;
    // REQ-EXT-GROUP-002：消息类响应按信封 group_id 路由到对应群窗口
    case ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_RESPOND:
    case ENUM_MSG_TYPE_GROUP_PULL_MESSAGES_RESPOND:
    case ENUM_MSG_TYPE_GROUP_READ_CONFIRM_RESPOND:
        Index::getinstance().getGroup()->handleMessageResponse(pdu);
        break;
    case ENUM_MSG_TYPE_GROUP_MESSAGE_EVENT:
        Index::getinstance().getGroup()->handleMessageEvent(pdu);
        break;
    // REQ-EXT-GROUP-003：群文件响应。100/104 按 group_id 路由到群文件窗口。
    case ENUM_MSG_TYPE_GROUP_FILE_LIST_RESPOND:
    case ENUM_MSG_TYPE_GROUP_FILE_DELETE_RESPOND:
        Index::getinstance().getGroup()->handleFileResponse(pdu);
        break;
    // REQ-EXT-TRANSFER-001：102/106 按 transfer_id 路由到群上传/群下载任务
    case ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_RESPOND:
        TransferManager::instance().onGroupUploadInitRespond(pdu);
        break;
    case ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_RESPOND:
        TransferManager::instance().onGroupDownloadRespond(pdu);
        break;
    default:
        break;
    }
}

void Client::seedMsg(PDU *pdu){
    sorket.write((char*)pdu,pdu->uiTotalLen);
    free(pdu);
    pdu=nullptr;
}

void Client::connectsort(){
    qCDebug(lcCli)<<"服务器连接成功"<<endl;
    // 区分"首次连接"与"断线后重连"：重连弹窗可见过说明这是一次重连
    bool wasReconnect = (m_reconnectDlg && m_reconnectDlg->isVisible());
    if (wasReconnect) {
        qCDebug(lcCli) << "[HEARTBEAT] 重连成功，关闭重连窗口";
        m_reconnectDlg->accept();   // 触发 closeEvent → 停止 spinner
        m_serverLostShown = false;
        m_lastPongMs = QDateTime::currentMSecsSinceEpoch();
        forceReLogin();             // 服务端重启后需重新登录，否则新连接无身份
    }
}

// 服务端重启/连接重建后，本连接的服务端身份已丢失，必须重新登录。
// 这里直接回到登录界面，要求用户重新输入账号密码；服务端会为新连接重建
// m_strLoginName，从而修复"重连后 listdir/movefile 退化到 ./filesys 根目录"
// 以及"未重新鉴权就继续收发文件/聊天"的隐患。
void Client::forceReLogin() {
    m_loggedIn = false;
    // REQ-EXT-GROUP-004：旧连接的服务端身份已丢失，群读状态随之作废；重新登录后由新会话重建
    resetGroupStates();
    // 回到登录界面（Client 在登录成功后曾被 hide，这里重新显示）
    Index::getinstance().hide();
    this->show();
    this->raise();
    this->activateWindow();
    flus();                         // 清空账号/密码输入框
    QMessageBox::information(this, "提示",
        "与服务器的连接已恢复。\n由于服务端已重启，请重新登录。");
}

void Client::recvmsg(){
    m_pdulist.append(sorket.readAll());
    while (m_pdulist.size()>=(int)sizeof(PDU)) {
        PDU*pdu=(PDU*)m_pdulist.data();
        uint pdutotallen=pdu->uiTotalLen;
        if((uint)m_pdulist.size()>=pdutotallen){
            handlMsg(pdu);
            m_pdulist.remove(0,pdu->uiTotalLen);
        }
        else break;
    }
}

void Client::on_register_PB_clicked(){
    if (m_captchaLocked) {
        QMessageBox::information(this,"提示","账号已锁定，请等待倒计时结束后重试");
        return;
    }
    QString strname=ui->name_LE->text();
    QString strpwd=ui->pwd_LE->text();
    if(strname.isEmpty()||strpwd.isEmpty()||strname.toUtf8().size()>31){
        QMessageBox::information(this,"提示","用户名或密码长度非法");
        return ;
    }
    QByteArray captchaBytes=ui->randcode_LE->text().toUtf8();
    PDU* pdu=mkPDU(captchaBytes.size()+1);
    pdu->uiType=ENUM_MSG_TYPE_REGIST_REQUEST;
    QByteArray passwordHash=passwordHashForProtocol(strpwd);
    copyFixedField(pdu->caData,32,strname.toUtf8());
    copyExactField(pdu->caData+32,32,passwordHash);
    memcpy(pdu->caMsg,captchaBytes.constData(),captchaBytes.size()+1);
    seedMsg(pdu);
}

void Client::on_login_PB_clicked(){
    if (m_captchaLocked) {
        QMessageBox::information(this,"提示","账号已锁定，请等待倒计时结束后重试");
        return;
    }
    QString captchaInput=ui->randcode_LE->text();
    if(captchaInput.isEmpty()){
        QMessageBox::information(this,"提示","请输入验证码");
        return;
    }
    QString strname=ui->name_LE->text();
    QString strpwd=ui->pwd_LE->text();
    if(strname.isEmpty()||strpwd.isEmpty()||strname.toUtf8().size()>31){
        QMessageBox::information(this,"提示","用户名或密码长度非法");
        return;
    }
    QByteArray captchaBytes=captchaInput.toUtf8();
    // REQ-EXT-SESSION-001 D1：验证码 NUL 之后追加 32 字节客户端 ID（零协议改动，老服务端按 uiMsgLen 忽略）
    QByteArray clientIdBytes=m_clientId.toUtf8();
    PDU* pdu=mkPDU(captchaBytes.size()+1+clientIdBytes.size());
    pdu->uiType=ENUM_MSG_TYPE_LOGIN_REQUEST;
    QByteArray passwordHash=passwordHashForProtocol(strpwd);
    copyFixedField(pdu->caData,32,strname.toUtf8());
    copyExactField(pdu->caData+32,32,passwordHash);
    memcpy(pdu->caMsg,captchaBytes.constData(),captchaBytes.size()+1);
    memcpy(pdu->caMsg+captchaBytes.size()+1,clientIdBytes.constData(),clientIdBytes.size());
    seedMsg(pdu);
}

//请求服务端验证码
void Client::on_pushButton_clicked(){
    // 客户端拦截：冷却/锁定中直接拒绝发送"获取验证码"请求（按钮本就禁用，这里兜底）
    if (m_captchaLocked || m_captchaCooldownSec > 0) {
        return;
    }
    if(sorket.state()!=QAbstractSocket::ConnectedState){
        QMessageBox::information(this,"提示","未连接服务器，无法获取验证码");
        return;
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_CAPTCHA_REQUEST;
    seedMsg(pdu);
}

void Client::updateCaptchaDisplay(const QString& code){
    ui->randcode_LE->setPlaceholderText(code);
}

void Client::resetCaptchaButton(){
    ui->pushButton->setEnabled(true);
}

void Client::startCaptchaCooldown(int sec, bool locked){
    if (sec <= 0) sec = locked ? 600 : 60;
    m_captchaCooldownSec=sec;
    m_captchaLocked=locked;
    ui->pushButton->setEnabled(false);
    ui->pushButton->setText(formatCooldown(m_captchaCooldownSec));
    if (locked) {
        // 锁定期间：注册/登录按钮也禁用，点击将被客户端直接拦截
        ui->register_PB->setEnabled(false);
        ui->login_PB->setEnabled(false);
    }
    m_captchaCooldownTimer->start(1000);
}

void Client::onCaptchaCooldownTick(){
    m_captchaCooldownSec--;
    if(m_captchaCooldownSec<=0){
        m_captchaCooldownTimer->stop();
        resetCaptchaButton();
        ui->pushButton->setText("获取验证码");
        if (m_captchaLocked) {
            // 10 分钟锁定结束：恢复注册/登录，后续冷却回到 1 分钟（由服务端下发时长驱动）
            m_captchaLocked = false;
            ui->register_PB->setEnabled(true);
            ui->login_PB->setEnabled(true);
        }
        return;
    }
    ui->pushButton->setText(formatCooldown(m_captchaCooldownSec));
}

//发送注销账户请求
void Client::sendLogoffRequest()
{
    //弹出密码确认框
    bool ok=false;
    QString pwdInput=QInputDialog::getText(this,"注销确认",
        "输入密码以确认注销账户：\n（此操作不可逆，将永久删除您的账户、好友关系和所有文件）",
        QLineEdit::Password,"",&ok);
    if(!ok||pwdInput.isEmpty()){
        return;
    }

    //二次确认
    int confirm=QMessageBox::warning(this,"最后确认",
        "确定要永久删除您的账户吗？\n此操作不可撤销！",
        QMessageBox::Yes|QMessageBox::No,QMessageBox::No);
    if(confirm!=QMessageBox::Yes){
        return;
    }

    QByteArray passwordHash=passwordHashForProtocol(pwdInput);
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_LOGOFF_REQUEST;
    copyFixedField(pdu->caData,32,m_LoginName.toUtf8());
    copyExactField(pdu->caData+32,32,passwordHash);
    seedMsg(pdu);
}

void Client::showLoadingAnimation(bool success){}

// ===== 服务端存活检测 + 重连 =====
void Client::onServerLost() {
    if (m_serverLostShown) return;
    m_serverLostShown = true;
    // REQ-EXT-TRANSFER-003 G7/C8②：心跳丢失（服务端假死）同样触发在途任务收敛，任务是"失败可重试"
    TransferManager::instance().onConnectionLost(QStringLiteral("与服务端断开连接，可重试"));
    qCDebug(lcCli) << "[HEARTBEAT] 服务端失联，弹出重连窗口";
    m_reconnectDlg->show();
    m_reconnectDlg->raise();
    m_reconnectDlg->activateWindow();
}

void Client::onSocketDisconnected() {
    qCDebug(lcCli) << "[HEARTBEAT] socket disconnected 信号";
    // REQ-EXT-TRANSFER-003 G7/C8①：连接断开即在途任务收敛（服务端被杀/网络断），
    // 不再让任务永久停在"传输中"占着名额（本地分片与服务端暂存保留，重试即续传）
    TransferManager::instance().onConnectionLost(QStringLiteral("与服务端断开连接，可重试"));
    // 已登录才弹窗（未登录在登录界面，连接失败不应弹重连窗口）
    if (m_loggedIn && !m_serverLostShown) {
        // REQ-EXT-SESSION-001：被服务端版本校验切断时的提示（零协议，仅文案）；后续流程复用既有重连窗口
        QMessageBox::information(this,"连接已关闭","连接被服务端关闭，账号可能已在其他设备登录。重新连接后请重新登录。");
        onServerLost();
    }
}

void Client::onReconnectRequested() {
    qCDebug(lcCli) << "[HEARTBEAT] 用户点击重新连接，发起 connectToHost";
    if (sorket.state() == QAbstractSocket::ConnectedState) {
        sorket.disconnectFromHost();
    }
    m_lastPongMs = QDateTime::currentMSecsSinceEpoch(); // 重置，避免重连过程中立即再判失联
    sorket.connectToHost(QHostAddress(m_strlp), m_usPort);
}
