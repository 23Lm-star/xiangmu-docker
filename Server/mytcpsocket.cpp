#include "logging.h"
#include "mytcpsocket.h"
#include "operatedb.h"
#include "protocol.h"
#include "mytcpserver.h"
#include "memalloc.h"
#include "uploadwritetask.h"

#include <QByteArray>
#include <QFileInfo>
#include <QTimer>
#include <QFile>
#include <QDateTime>
#include <QThreadPool>
#include <cstring>

// REQ-EXT-TRANSFER-003 D6：上传进度落库节流阈值（字节数 / 毫秒，满足其一即落库）
static constexpr qint64 UPLOAD_PERSIST_MIN_BYTES = 1024 * 1024;
static constexpr qint64 UPLOAD_PERSIST_MIN_MS = 2000;

MyTcpSocket::MyTcpSocket()
{
    connect(this,&MyTcpSocket::readyRead,this,&MyTcpSocket::recvmsg);
    connect(this,&MyTcpSocket::bytesWritten,this,&MyTcpSocket::onBytesWritten);
    connect(this,&MyTcpSocket::disconnected,this,&MyTcpSocket::clientOffLine);
    m_MsgHandler=new MsgHandler;
    a=0;
    m_downloadNextChunkScheduled=false;
    m_rrCursor=0;
    m_connectionId=0;
    m_invalidPduCount=0;
    m_lastActiveMs = QDateTime::currentMSecsSinceEpoch();
    // D5：上传上下文空闲回收（连接级；参照 MyTcpServer 僵尸清扫的定时器模式）
    m_reapTimer=new QTimer(this);
    connect(m_reapTimer,&QTimer::timeout,this,&MyTcpSocket::onReapTimeout);
    m_reapTimer->start(15000);
}

MyTcpSocket::~MyTcpSocket(){
    for(QHash<quint64,DownloadStream>::iterator it=m_downloads.begin();it!=m_downloads.end();++it){
        if(it->file){
            it->file->close();
            delete it->file;
            it->file=nullptr;
        }
    }
    m_downloads.clear();
    m_downloadRing.clear();
    delete m_MsgHandler;
}

// D5：空闲上传上下文回收（写盘在途不回收）
void MyTcpSocket::onReapTimeout()
{
    if(m_MsgHandler==nullptr){
        return;
    }
    m_MsgHandler->reapIdleUploads(QDateTime::currentMSecsSinceEpoch(),UPLOAD_IDLE_REAP_SEC);
}

PDU *MyTcpSocket::handlMsg(PDU *pdu)
{
    qCDebug(lcNet)<<"——————开始进行处理————————"<<endl;
    PDU* respdu=nullptr;
    m_MsgHandler->pdu=pdu;

    // ===== REQ-EXT-TRANSFER-001：按 PDU 落位读 transfer_id（请求侧传输族；D2 统一 [48..55]） =====
    quint64 transferId=TRANSFER_ID_LEGACY;
    switch(pdu->uiType){
    case ENUM_MSG_TYPE_UPLOADFILE_INIT_REQUEST:
    case ENUM_MSG_TYPE_UPLOADFILE_DATA_REQUEST:
    case ENUM_MSG_TYPE_DOWNLOADFILE_REQUEST:
    case ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_REQUEST:
    case ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_REQUEST:
    case ENUM_MSG_TYPE_UPLOADFILE_ABORT_REQUEST:
        memcpy(&transferId,pdu->caData+TRANSFER_ID_OFFSET,sizeof(transferId));
        break;
    default:
        break;
    }
    m_MsgHandler->m_currentTransferId=transferId;

    // ===== REQ-EXT-SEC-001 T2: 统一入口门 =====
    // 白名单（注册/登录/验证码/心跳）之外的一切业务请求，必须携带连接级认证上下文；
    // 拒绝原因与连接关联标识入日志（不含密码/令牌/文件内容），并回失败响应。
    if(!isPublicRequestType(pdu->uiType) && !m_actorAuthenticated){
        qCWarning(lcAuth)<<"[AUTHGATE] 拒绝：未认证连接发送业务请求 conn="<<m_connectionId
                         <<" type="<<pdu->uiType<<" session="<<m_actorSessionVersion;
        if(pdu->uiType==ENUM_MSG_TYPE_UPLOADFILE_DATA_REQUEST){
            sendUploadAck(transferId,false,0,false);
            return nullptr;
        }
        return authRejectPdu(pdu->uiType);
    }
    // ===== REQ-EXT-SESSION-001 D3/D4: 登录版本校验 =====
    // 已认证连接的一切请求（含心跳，杜绝靠心跳永久存活）处理前比对连接绑定版本与
    // 该用户最新登录版本；不一致=过期会话，直接切断。未认证连接维持上面的白名单语义。
    if(m_actorAuthenticated){
        const quint64 cur=MyTcpServer::getinstance().currentLoginVersion(m_strLoginName);
        if(m_loginVersion!=cur){
            qCWarning(lcAuth)<<"[KICK] 版本过期，拒绝并切断 conn="<<m_connectionId
                             <<" user="<<m_strLoginName<<" ver="<<m_loginVersion
                             <<" latest="<<cur<<" type="<<pdu->uiType;
            PDU* rej=authRejectPdu(pdu->uiType);  // 断线前快照响应（断后尽力发送，无害）
            abort();  // 同步触发 clientOffLine，由权威守卫保护不误清新会话
            return rej;
        }
    }
    // 处理器统一使用服务端派生的 actor（登录时绑定，注销/断线撤销）
    m_MsgHandler->m_actorName=this->m_strLoginName;
    m_MsgHandler->m_connId=this->m_connectionId;

    switch(pdu->uiType){
        case ENUM_MSG_TYPE_REGIST_REQUEST :{
             respdu=m_MsgHandler->rigist();
             break;
        }
         case ENUM_MSG_TYPE_LOGIN_REQUEST :{
            bool authOk=false;
            quint64 loginVersion=0;
            respdu=m_MsgHandler->login(this->m_strLoginName,authOk,loginVersion);
            if(authOk){
                // REQ-EXT-SEC-001 T1：登录成功，绑定连接级 actor_context
                m_actorAuthenticated=true;
                ++m_actorSessionVersion;
                // REQ-EXT-SESSION-001：绑定用户域登录版本——先登记再踢旧
                // （被踢方断开时登记表已指向新版，clientOffLine 守卫走 stale 分支）
                m_loginVersion=loginVersion;
                m_MsgHandler->m_actorName=this->m_strLoginName;
                MyTcpServer::getinstance().setLoginVersion(this->m_strLoginName, loginVersion);
                MyTcpServer::getinstance().closeStaleSockets(this->m_strLoginName);
                qCDebug(lcAuth)<<"[AUTH] 登录成功并绑定连接身份 conn="<<m_connectionId
                                <<" session="<<m_actorSessionVersion<<" loginver="<<m_loginVersion;
            }
            break;
         }
    case ENUM_MSG_TYPE_findUser_REQUEST:{
            respdu=m_MsgHandler->finduser();
        break;
    }
    case ENUM_MSG_TYPE_ONLINEUSER_REQUEST:{
        respdu=m_MsgHandler->onlineuser(this->m_strLoginName);
        break;
    }
    case ENUM_MSG_TYPE_ADDFRIEND_REQUEST:{
        respdu=m_MsgHandler->addfriend();
        break;
    }
    case ENUM_MSG_TYPE_ADDFRIEND_RESPOND:{
        respdu=m_MsgHandler->addfriendFin();
        break;
    }
    case ENUM_MSG_TYPE_FLUSHFRIEND_REQUEST:{
        respdu=m_MsgHandler->flushfriend(this->m_strLoginName);
        break;
    }
    case ENUM_MSG_TYPE_DELETEFRIEND_REQUEST:{
        respdu=m_MsgHandler->deletefriend();
        break;
    }
    case ENUM_MSG_TYPE_CHAT_REQUEST:{
        respdu=m_MsgHandler->chat();
        break;
    }
    case ENUM_MSG_TYPE_GETCHATMSG_REQUEST:{
        respdu=m_MsgHandler->chatmsg();
        break;
    }
    case ENUM_MSG_TYPE_MKDIR_REQUEST:{
        respdu=m_MsgHandler->MKDIR();
        break;
    }
    case ENUM_MSG_TYPE_CHANGEDIR_REQUEST:{
        respdu=m_MsgHandler->changedir();
        break;
    }
    case ENUM_MSG_TYPE_DELFILE_REQUEST:{
        respdu=m_MsgHandler->deldir(this->m_strLoginName);
        break;
    }
    case ENUM_MSG_TYPE_FLUSHFILE_REQUEST:{
        respdu=m_MsgHandler->flushfile();
        break;
    }
    case ENUM_MSG_TYPE_UPLOADFILE_DATA_REQUEST:{
        handleUploadData(pdu);
        return nullptr;
    }
    case ENUM_MSG_TYPE_UPLOADFILE_INIT_REQUEST:{
        respdu=m_MsgHandler->uploadfileinit();
        break;
    }
    case ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_REQUEST:{
        respdu=m_MsgHandler->uploadfilefastcheck();
        break;
    }
    case ENUM_MSG_TYPE_RENAMEFILE_REQUEST:{
        respdu=m_MsgHandler->renamefile(this->m_strLoginName);
        break;
    }
    case ENUM_MSG_TYPE_SHAREFILE_REQUEST:{
        respdu=m_MsgHandler->sharefile();
        break;
    }
    case ENUM_MSG_TYPE_SHAREFILE_AGREE_RESPOND:{
        respdu=m_MsgHandler->sharefilemove();
        break;
    }
    case ENUM_MSG_TYPE_MOVEFILE_REQUEST:{
        respdu=m_MsgHandler->movefile(this->m_strLoginName);
        break;
    }
    case ENUM_MSG_TYPE_LISTDIR_REQUEST:{
        respdu=m_MsgHandler->listdir(this->m_strLoginName);
        break;
    }
    case ENUM_MSG_TYPE_DOWNLOADFILE_REQUEST:{
        // D7：超下载上限时 41 回 ret=false（同 id 重发视为替换，不占额外名额）
        m_MsgHandler->m_downloadSlotAvailable =
                (m_downloads.contains(transferId) || m_downloads.size() < int(MAX_PARALLEL_DOWNLOADS));
        // REQ-EXT-TRANSFER-002 S2：读出 40 声明的续传起点（TRANSFER_ID_OFFSET 的同一读法；旧客户端为 0）
        qint64 resumeOffset=0;
        memcpy(&resumeOffset,pdu->caData+DOWNLOAD_RESUME_OFFSET,sizeof(resumeOffset));
        m_MsgHandler->m_downloadResumeOffset=resumeOffset;
        respdu=m_MsgHandler->downloadfile();
        if(respdu){
            bool valid=false;
            memcpy(&valid,respdu->caData,sizeof(valid));
            sendAndRelease(respdu);
            respdu=nullptr;
            if(valid && !m_MsgHandler->m_downloadCanonicalPath.isEmpty()){
                startDownloadStream(m_MsgHandler->m_downloadCanonicalPath,transferId,resumeOffset);
            }
        }
        return nullptr;
    }
    // REQ-EXT-TRANSFER-001：上传任务释放（暂停/取消共用；108 回带 id）
    // REQ-EXT-TRANSFER-002 S1：107 现对上传/下载两方向同义
    case ENUM_MSG_TYPE_UPLOADFILE_ABORT_REQUEST:{
        return handleTransferAbort(pdu);
    }
    case ENUM_MSG_TYPE_CAPTCHA_REQUEST:{
        respdu=m_MsgHandler->captcha();
        break;
    }
    case ENUM_MSG_TYPE_GETUSERINFORMATION_REQUEST:{
        respdu=m_MsgHandler->getuserinformation();
        break;
    }
    case ENUM_MSG_TYPE_LOGOFF_REQUEST:{
        bool revoked=false;
        respdu=m_MsgHandler->logoff(revoked);
        if(revoked){
            // REQ-EXT-SEC-001 T1：注销成功，撤销业务访问（连接保留，D2），后续请求被入口门拒绝
            // REQ-EXT-SESSION-001 修复（注销→同名重注册→登录被误杀）：注销删除了 user_info 行，
            // 重新注册会把 login_version 计数器归零。必须在这里同步释放该用户的登记表条目，
            // 否则注销前残留的高版本号仍在 m_loginVersionRegistry 里，重新注册后的首次登录
            // 分配到的低版本会被 setLoginVersion 的单调 CAS 丢弃，随后 closeStaleSockets 按
            // "m_loginVersion != cur" 把这条刚登录成功的连接自己 abort（客户端收不到
            // LOGIN_RESPOND，DB 留下 online=1 幽灵）。释放条目还顺带收紧语义：账号已删除后，
            // 同用户其它存活连接下次请求会因登记值为 0 ≠ 自身版本被踢，不再保留访问权。
            // 必须在 m_strLoginName.clear() 之前调用（释放按用户名定位）。
            MyTcpServer::getinstance().removeLoginVersion(m_strLoginName);
            m_actorAuthenticated=false;
            ++m_actorSessionVersion;
            m_strLoginName.clear();
            m_MsgHandler->m_actorName.clear();
            qCWarning(lcAuth)<<"[AUTH] 注销撤销业务访问（连接保留）conn="<<m_connectionId
                              <<" session="<<m_actorSessionVersion;
        }
        break;
    }
    case ENUM_MSG_TYPE_HEARTBEAT_REQUEST:{
        m_lastActiveMs = QDateTime::currentMSecsSinceEpoch();
        qCDebug(lcNet) << "[HEARTBEAT] recv PING from" << m_strLoginName << "-> send PONG";
        PDU* hRespdu = mkPDU();
        hRespdu->uiType = ENUM_MSG_TYPE_HEARTBEAT_RESPOND;
        return hRespdu;
    }
    case ENUM_MSG_TYPE_CREATE_PUBLINK_REQUEST:{
        respdu=m_MsgHandler->createPublishLink(this->m_strLoginName);
        break;
    }
    case ENUM_MSG_TYPE_RESOLVE_PUBLINK_REQUEST:{
        respdu=m_MsgHandler->resolvePublishLink(this->m_strLoginName);
        break;
    }
    case ENUM_MSG_TYPE_QUERY_FILE_PUBLINK_REQUEST:{
        respdu=m_MsgHandler->queryFilePublishLink();
        break;
    }
    case ENUM_MSG_TYPE_CREATE_GROUP_REQUEST:
    case ENUM_MSG_TYPE_INVITE_GROUP_MEMBER_REQUEST:
    case ENUM_MSG_TYPE_ACCEPT_GROUP_INVITATION_REQUEST:
    case ENUM_MSG_TYPE_REJECT_GROUP_INVITATION_REQUEST:
    case ENUM_MSG_TYPE_LEAVE_GROUP_REQUEST:
    case ENUM_MSG_TYPE_REMOVE_GROUP_MEMBER_REQUEST:
    case ENUM_MSG_TYPE_SET_GROUP_ROLE_REQUEST:
    case ENUM_MSG_TYPE_TRANSFER_GROUP_OWNER_REQUEST:
    case ENUM_MSG_TYPE_DISSOLVE_GROUP_REQUEST:
    case ENUM_MSG_TYPE_LIST_GROUPS_REQUEST:
    case ENUM_MSG_TYPE_LIST_GROUP_MEMBERS_REQUEST:
    case ENUM_MSG_TYPE_QUERY_GROUP_INVITATIONS_REQUEST:
    case ENUM_MSG_TYPE_QUERY_GROUP_SUMMARY_REQUEST:
    // REQ-EXT-GROUP-002：群消息请求（88/91/93/95，均经 actor 门）
    case ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_REQUEST:
    case ENUM_MSG_TYPE_GROUP_PULL_MESSAGES_REQUEST:
    case ENUM_MSG_TYPE_GROUP_READ_CONFIRM_REQUEST:
    case ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_REQUEST:
        respdu=m_MsgHandler->groupRequest(pdu->uiType);
        break;
    // REQ-EXT-GROUP-003：群文件请求（99/101/103，均经 actor 门）
    case ENUM_MSG_TYPE_GROUP_FILE_LIST_REQUEST:
    case ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_REQUEST:
    case ENUM_MSG_TYPE_GROUP_FILE_DELETE_REQUEST:
        respdu=m_MsgHandler->groupFileRequest(pdu->uiType);
        break;
    // 105 群文件下载：先回 106 元数据，再走既有 42/43 分块流（源路径由服务端解析）
    case ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_REQUEST:{
        // D7：超下载上限时 106 的 code = -2107（同 id 重发视为替换）
        m_MsgHandler->m_downloadSlotAvailable =
                (m_downloads.contains(transferId) || m_downloads.size() < int(MAX_PARALLEL_DOWNLOADS));
        // REQ-EXT-TRANSFER-002 S2：读出 105 声明的续传起点（与 40 同一常量同一偏移；caMsg 零改动）
        qint64 resumeOffset=0;
        memcpy(&resumeOffset,pdu->caData+DOWNLOAD_RESUME_OFFSET,sizeof(resumeOffset));
        m_MsgHandler->m_downloadResumeOffset=resumeOffset;
        respdu=m_MsgHandler->groupFileRequest(pdu->uiType);
        if(respdu){
            const QString src=m_MsgHandler->m_downloadCanonicalPath;
            sendAndRelease(respdu);
            respdu=nullptr;
            if(!src.isEmpty()){
                startDownloadStream(src,transferId,resumeOffset);
            }
        }
        return nullptr;
    }
    case ENUM_MSG_TYPE_GROUP_MEMBER_EVENT:
    case ENUM_MSG_TYPE_GROUP_INVITATION_EVENT:
    case ENUM_MSG_TYPE_GROUP_MESSAGE_EVENT:
        // 事件只能由服务端生成，客户端发入的同名类型一律丢弃并记审计日志。
        qCWarning(lcNet) << "[GROUP] 拒绝客户端伪造事件 type=" << pdu->uiType
                         << "conn=" << m_connectionId;
        break;
        default:
            break;
    }
   return respdu;
}

// 上传数据：按 transfer_id 取上下文（未知 id 拒绝且不回退，P7）
void MyTcpSocket::handleUploadData(PDU* pdu)
{
    quint64 transferId=TRANSFER_ID_LEGACY;
    memcpy(&transferId,pdu->caData+TRANSFER_ID_OFFSET,sizeof(transferId));
    MsgHandler::UploadCtx* cp=m_MsgHandler->findUpload(transferId);
    if(cp==nullptr){
        qCWarning(lcNet)<<"[transfer] 未知上传 id 拒绝 conn="<<m_connectionId<<" id="<<transferId;
        sendUploadAck(transferId,false,0,false);
        return;
    }

    uint resumeMagic=0;
    qint64 chunkOffset=0;
    memcpy(&resumeMagic,pdu->caData+UPLOAD_RESUME_MAGIC_OFFSET,sizeof(resumeMagic));
    memcpy(&chunkOffset,pdu->caData+UPLOAD_CHUNK_OFFSET,sizeof(chunkOffset));

    const qint64 expectedOffset=cp->session.receivedSize();
    // REQ-EXT-TRANSFER-003 D5/S7：超长块显式拒绝（32 ok=false，连接存活），不落到"非法 PDU → 计数断连"
    if(pdu->uiMsgLen > MAX_UPLOAD_DATA_CHUNK){
        qCWarning(lcNet) << "[transfer] 超长上传块拒绝 conn=" << m_connectionId
                         << " id=" << transferId << " len=" << pdu->uiMsgLen
                         << " cap=" << MAX_UPLOAD_DATA_CHUNK;
        sendUploadAck(transferId,false,expectedOffset,false);
        return;
    }
    if(resumeMagic != UPLOAD_RESUME_MAGIC || cp->writeInFlight
            || chunkOffset != expectedOffset || pdu->uiMsgLen == 0
            || pdu->uiMsgLen > cp->session.totalSize()-expectedOffset
            || cp->session.stagingFilePath().isEmpty()){
        sendUploadAck(transferId,false,expectedOffset,cp->session.isComplete());
        return;
    }

    cp->writeInFlight=true;
    const QByteArray data(pdu->caMsg,pdu->uiMsgLen);
    QThreadPool::globalInstance()->start(new UploadWriteTask(
            m_connectionId,transferId,cp->session.stagingFile(),chunkOffset,data));
}

void MyTcpSocket::handleUploadWriteFinished(quint64 transferId, qint64 offset, qint64 length, bool success)
{
    MsgHandler::UploadCtx* cp=m_MsgHandler->findUpload(transferId);
    if(cp==nullptr){
        return;   // 上下文已被 abort/回收；客户端已收 108 或不再等待
    }
    cp->writeInFlight=false;
    if(success){
        success=cp->session.acceptWrittenChunk(offset,length);
    }
    // AUD-4：107 曾因写盘在途而延迟——现在写已落定（成功则已计入 receivedSize），收尾释放。
    // abortUpload 会把暂存对齐到 receivedSize 并落 DB confirmed_offset；此处不再回 32（客户端已发 107）。
    if(cp->pendingAbort){
        m_MsgHandler->abortUpload(transferId);
        return;
    }
    const bool complete = success && cp->session.isComplete();
    const qint64 ackOffset = cp->session.receivedSize();
    if(complete){
        // REQ-EXT-FILE-001 T4：末块写完→服务端复算转正+建条目+引用计数（会释放上下文）
        m_MsgHandler->m_currentTransferId=transferId;
        m_MsgHandler->finalizeUploadOnComplete();
    }else if(success){
        // AUD-1：该槽已开始承载正文（旧客户端 97→31 直传路径），不再是 97 纯暂存占位 → 重新计入名额
        cp->fastcheckPlaceholder=false;
        cp->lastProgressMs=QDateTime::currentMSecsSinceEpoch();   // 推进空闲回收计时
        // REQ-EXT-TRANSFER-003 S9/D6：进度落库节流——≥1 MiB 或 ≥2 s 才写一次
        // （4 KB 旧客户端 ≈ 每 256 块一次；1 MiB 新客户端 ≈ 每块一次）。完成态由 S10 强制写，不依赖节流。
        const qint64 now=cp->lastProgressMs;
        const qint64 received=cp->session.receivedSize();
        if(cp->sessionId>=0
                && (received-cp->lastPersistOffset>=UPLOAD_PERSIST_MIN_BYTES
                    || now-cp->lastPersistMs>=UPLOAD_PERSIST_MIN_MS)){
            OperateDB::instance().updateUploadOffset(cp->sessionId,received);
            cp->lastPersistOffset=received;
            cp->lastPersistMs=now;
        }
    }
    sendUploadAck(transferId,success,ackOffset,complete);
}

// 107：释放传输槽位。上传（暂停/取消同处置：会话 expired，staging 保留）；
// REQ-EXT-TRANSFER-002 S1：未命中上传槽时再查下载流 → 摘环停发、名额回落。
// 未知 id → ret=false；id=0 一律不受理（维持既有断言 T-12a，旧单流路径不受 107 影响）。
PDU* MyTcpSocket::handleTransferAbort(PDU* pdu)
{
    quint64 transferId=TRANSFER_ID_LEGACY;
    memcpy(&transferId,pdu->caData+TRANSFER_ID_OFFSET,sizeof(transferId));
    bool ret=false;
    if(transferId!=TRANSFER_ID_LEGACY){
        MsgHandler::UploadCtx* cp=m_MsgHandler->findUpload(transferId);
        if(cp!=nullptr){
            // AUD-4：写盘在途时不得立即释放——否则在途写会落在 ctx 消失之后（回调落空），暂存物理大小
            // 越过 upload_session.confirmed_offset。标记延迟释放，由 handleUploadWriteFinished 收尾；
            // 108 仍即时回 ret=true（客户端不再向该 id 发正文，槽位语义不受影响）。
            if(cp->writeInFlight){
                cp->pendingAbort=true;
            }else{
                m_MsgHandler->abortUpload(transferId);
            }
            ret=true;
        }else if(m_downloads.contains(transferId)){
            removeDownloadStream(transferId);
            ret=true;
        }
    }
    PDU* resp=mkPDU();
    resp->uiType=ENUM_MSG_TYPE_UPLOADFILE_ABORT_RESPOND;
    memcpy(resp->caData,&ret,sizeof(ret));
    memcpy(resp->caData+TRANSFER_ID_OFFSET,&transferId,sizeof(transferId));
    return resp;
}

void MyTcpSocket::sendUploadAck(quint64 transferId, bool success, qint64 nextOffset, bool complete)
{
    PDU* response=mkPDU();
    const uint resumeMagic=UPLOAD_RESUME_MAGIC;
    response->uiType=ENUM_MSG_TYPE_UPLOADFILE_DATA_RESPOND;
    memcpy(response->caData,&success,sizeof(success));
    memcpy(response->caData+UPLOAD_ACK_NEXT_OFFSET,&nextOffset,sizeof(nextOffset));
    memcpy(response->caData+UPLOAD_ACK_COMPLETE_OFFSET,&complete,sizeof(complete));
    memcpy(response->caData+UPLOAD_RESUME_MAGIC_OFFSET,&resumeMagic,sizeof(resumeMagic));
    memcpy(response->caData+TRANSFER_ID_OFFSET,&transferId,sizeof(transferId));   // R3：回带 id
    seedMsg(response);
}

// 43 完成 PDU（携带 id）
void MyTcpSocket::seedDownloadFinish(quint64 transferId, bool success)
{
    PDU* finishPdu=mkPDU();
    finishPdu->uiType=ENUM_MSG_TYPE_DOWNLOADFILE_FINISH;
    memcpy(finishPdu->caData,&success,sizeof(success));
    memcpy(finishPdu->caData+TRANSFER_ID_OFFSET,&transferId,sizeof(transferId));
    seedMsg(finishPdu);
}

// 新流加入环尾（id=0 = 旧单流路径）。
// REQ-EXT-TRANSFER-002 S3：startOffset = 采纳的续传起点，seek 后只发后缀；起点==大小即 0 字节正文 + 43 success（D5）。
// 起点>大小在调用方已按 §2.3 拒绝，此处仅断言式防护：不建流并明确失败，绝不静默拼坏文件。
void MyTcpSocket::startDownloadStream(const QString& filePath, quint64 transferId, qint64 startOffset)
{
    DownloadStream& st=m_downloads[transferId];
    st.id=transferId;
    if(st.file){
        st.file->close();
        delete st.file;
        st.file=nullptr;
    }
    st.file=new QFile(filePath);
    if(!st.file->open(QIODevice::ReadOnly)){
        qCWarning(lcNet)<<"下载文件打开失败:"<<filePath;
        delete st.file;
        st.file=nullptr;
        m_downloads.remove(transferId);
        seedDownloadFinish(transferId,false);
        return;
    }
    st.totalSize=st.file->size();
    if(startOffset<0||startOffset>st.totalSize||!st.file->seek(startOffset)){
        qCWarning(lcNet)<<"续传起点非法，不建流:"<<filePath<<"offset"<<startOffset<<"size"<<st.totalSize<<"id"<<transferId;
        st.file->close();
        delete st.file;
        st.file=nullptr;
        m_downloads.remove(transferId);
        seedDownloadFinish(transferId,false);
        return;
    }
    st.sentSize=startOffset;
    if(!m_downloadRing.contains(transferId)){
        m_downloadRing.append(transferId);
    }
    qCDebug(lcNet)<<"开始流式下载:"<<filePath<<"大小:"<<st.totalSize<<"起点"<<startOffset<<"id"<<transferId;
    scheduleNextDownloadChunk();
}

// 摘环：完成/读失败/被 abort 时移除；被摘下标 < 游标时游标减一（§4.1）
void MyTcpSocket::removeDownloadStream(quint64 id)
{
    const int idx=m_downloadRing.indexOf(id);
    if(idx>=0){
        if(idx<m_rrCursor){
            --m_rrCursor;
        }
        m_downloadRing.removeAt(idx);
    }
    QHash<quint64,DownloadStream>::iterator it=m_downloads.find(id);
    if(it!=m_downloads.end()){
        if(it->file){
            it->file->close();
            delete it->file;
            it->file=nullptr;
        }
        m_downloads.erase(it);
    }
    if(m_downloadRing.isEmpty()){
        m_rrCursor=0;
    }else if(m_rrCursor>=m_downloadRing.size()){
        m_rrCursor=0;
    }
}

void MyTcpSocket::scheduleNextDownloadChunk()
{
    if(m_downloadRing.isEmpty() || m_downloadNextChunkScheduled){
        return;
    }
    m_downloadNextChunkScheduled=true;
    QTimer::singleShot(0,this,&MyTcpSocket::sendNextChunk);
}

void MyTcpSocket::onBytesWritten(qint64)
{
    if(!m_downloadRing.isEmpty() && bytesToWrite()<DOWNLOAD_WRITE_HIGH_WATER){
        scheduleNextDownloadChunk();
    }
    // REQ-EXT-GROUP-002 T3：出站队列随 socket 缓冲回落继续冲刷
    if(!m_outboundQueue.isEmpty() && bytesToWrite()<OUTBOUND_FLUSH_HIGH_WATER){
        pumpOutboundQueue();
    }
}

// REQ-EXT-GROUP-002 T3：入队一条待发送 PDU（广播路径调用，锁外、非阻塞）。
// 队列满（条数或字节先到者）返回 false：该连接实时投递被丢弃并计数，
// 由下次进入群聊/重连的游标拉取兜底；不影响其他成员。
bool MyTcpSocket::enqueueOutbound(const QByteArray &pdu)
{
    if(pdu.isEmpty()){
        return true;
    }
    if(m_outboundQueue.size()>=OUTBOUND_QUEUE_MAX_MSGS
            || m_outboundQueueBytes+pdu.size()>OUTBOUND_QUEUE_MAX_BYTES){
        ++m_outboundDropCount;
        qCWarning(lcNet)<<"[GROUP] 广播队列满，丢弃该连接实时投递 conn="<<m_connectionId
                        <<" user="<<m_strLoginName<<" drops="<<m_outboundDropCount;
        return false;
    }
    m_outboundQueue.append(pdu);
    m_outboundQueueBytes+=pdu.size();
    if(!m_outboundPumpScheduled && bytesToWrite()<OUTBOUND_FLUSH_HIGH_WATER){
        m_outboundPumpScheduled=true;
        QTimer::singleShot(0,this,&MyTcpSocket::pumpOutboundQueue);
    }
    return true;
}

// REQ-EXT-GROUP-002 T3：事件循环内冲刷队列——仅在 socket 缓冲低于高水位时写入，
// 使单连接总排队量有界；连接断开时由 clientOffLine 清空。
void MyTcpSocket::pumpOutboundQueue()
{
    m_outboundPumpScheduled=false;
    while(!m_outboundQueue.isEmpty() && bytesToWrite()<OUTBOUND_FLUSH_HIGH_WATER){
        const QByteArray next=m_outboundQueue.takeFirst();
        m_outboundQueueBytes-=next.size();
        write(next.constData(),next.size());
    }
}

// 下载调度 tick：每个 tick 只发 1 块（round-robin，§4.1）；背压时本轮不发，等 onBytesWritten
void MyTcpSocket::sendNextChunk()
{
    m_downloadNextChunkScheduled=false;
    if(m_downloadRing.isEmpty()){
        return;
    }
    if(bytesToWrite()+DOWNLOAD_CHUNK_SIZE+static_cast<qint64>(sizeof(PDU))
            > DOWNLOAD_WRITE_HIGH_WATER){
        return;
    }

    int attempts=m_downloadRing.size();
    while(attempts-->0 && !m_downloadRing.isEmpty()){
        if(m_rrCursor>=m_downloadRing.size()){
            m_rrCursor=0;
        }
        const quint64 id=m_downloadRing.at(m_rrCursor);
        QHash<quint64,DownloadStream>::iterator it=m_downloads.find(id);
        if(it==m_downloads.end() || it->file==nullptr || !it->file->isOpen()){
            seedDownloadFinish(id,false);
            removeDownloadStream(id);
            continue;
        }

        const qint64 remaining=it->remaining();
        if(remaining<=0){
            qCDebug(lcNet)<<"下载完成 id="<<id<<"总发送:"<<it->sentSize<<"字节";
            seedDownloadFinish(id,true);
            removeDownloadStream(id);   // 摘环时游标规则内置，不前进
            continue;
        }

        const int chunkSize=int(qMin((qint64)DOWNLOAD_CHUNK_SIZE,remaining));
        const QByteArray chunk=it->file->read(chunkSize);
        if(chunk.isEmpty()){
            qCWarning(lcNet)<<"读取文件块失败 id="<<id;
            seedDownloadFinish(id,false);
            removeDownloadStream(id);
            continue;
        }

        PDU* dataPdu=mkPDU(uint(chunk.size()));
        dataPdu->uiType=ENUM_MSG_TYPE_DOWNLOADFILE_DATA;
        memcpy(dataPdu->caData+TRANSFER_ID_OFFSET,&id,sizeof(id));   // R3：42 携带 id
        memcpy(dataPdu->caMsg,chunk.constData(),size_t(chunk.size()));
        it->sentSize+=chunk.size();
        m_rrCursor=(m_rrCursor+1)%m_downloadRing.size();   // 一次只发一块
        seedMsg(dataPdu);
        scheduleNextDownloadChunk();
        return;
    }
    if(!m_downloadRing.isEmpty()){
        scheduleNextDownloadChunk();
    }
}
//接收客户端消息
void MyTcpSocket::recvmsg(){
    m_lastActiveMs = QDateTime::currentMSecsSinceEpoch();
    QByteArray dpui=this->readAll();
    this->m_dpulist.append(dpui);
    
    // 安全校验：未处理缓存超过 256 KB，防止耗尽内存攻击
    if (m_dpulist.size() > MAX_PENDING_INBOUND_BYTES) {
        qCWarning(lcNet) << "Security: Inbound buffer exceeded 256KB from" 
                   << this->peerAddress() << "- size:" << m_dpulist.size();
        m_dpulist.clear();
        this->disconnectFromHost();
        return;
    }

    while(this->m_dpulist.size()>=(int)sizeof (PDU)){
            PDU* pdu=(PDU*)m_dpulist.data();
            uint pduTotalLen=pdu->uiTotalLen;
            
            // 安全校验：解析头部合法性
            // REQ-EXT-TRANSFER-003 D2：仅上传数据通道（31）按大分片上限校验，其余报文一律保持 16 KB
            const uint inboundCap = (pdu->uiType == ENUM_MSG_TYPE_UPLOADFILE_DATA_REQUEST)
                    ? MAX_UPLOAD_DATA_PDU_SIZE : MAX_INBOUND_PDU_SIZE;
            bool valid = false;
            if (pduTotalLen >= sizeof(PDU) && pduTotalLen <= inboundCap) {
                if (pdu->uiMsgLen == pduTotalLen - sizeof(PDU) &&
                    pdu->uiType > ENUM_MSG_TYPE_MIN && 
                    pdu->uiType < ENUM_MSG_TYPE_MAX) {
                    valid = true;
                }
            }
            
            if (!valid) {
                m_invalidPduCount++;
                qCWarning(lcNet) << "Security: Invalid PDU from" << this->peerAddress()
                           << "Count:" << m_invalidPduCount << "/" << MAX_INVALID_PDU_THRESHOLD
                           << "uiTotalLen:" << pduTotalLen
                           << "uiMsgLen:" << pdu->uiMsgLen
                           << "uiType:" << pdu->uiType;
                
                if (m_invalidPduCount >= MAX_INVALID_PDU_THRESHOLD) {
                    qCCritical(lcNet) << "Security: Dropped packets exceeded threshold. Kicking client:" << this->peerAddress();
                    m_dpulist.clear();
                    this->disconnectFromHost();
                    return;
                }
                
                // 仅丢弃当前非法 PDU 的长度，继续解析剩余数据（尝试同步）
                m_dpulist.remove(0, pduTotalLen >= sizeof(PDU) ? pduTotalLen : sizeof(PDU));
                continue;
            }

            // 校验通过，重置计数器（证明客户端是合法的或已恢复同步）
            m_invalidPduCount = 0;

            if(m_dpulist.size()>=(int)pduTotalLen){
                qCDebug(lcNet)<<"请求信息已接收成功:"<<" "
                        <<"pdu->uiTotalLen"<<pdu->uiTotalLen<<" "
                        <<"pdu->uiMsgLen"<<pdu->uiMsgLen<<" "
                        <<"pdu->uiType"<<pdu->uiType<<"  "
                        <<"pdu->caData"<<pdu->caData<<" "
                        <<"pdu->caData+32"<<pdu->caData+32<<" "
                        <<"pdu->caMsg"<<pdu->caMsg<<endl;
                PDU* respdu=handlMsg(pdu);
                seedMsg(respdu);
                m_dpulist.remove(0,pdu->uiTotalLen);
            }
            else break;
        }
}
//客户端下线->更改客户端的登录状态，并且移除sorket对应的
void MyTcpSocket::clientOffLine(){
    // REQ-EXT-TRANSFER-001 §3.5：断线清理全部下载流与上传上下文；不写 DB
    //（staging 与 upload_session 行留给既有 3600s TTL 收敛，与今日单流语义一致）
    for(QHash<quint64,DownloadStream>::iterator it=m_downloads.begin();it!=m_downloads.end();++it){
        if(it->file){
            it->file->close();
            delete it->file;
            it->file=nullptr;
        }
    }
    m_downloads.clear();
    m_downloadRing.clear();
    m_rrCursor=0;
    m_downloadNextChunkScheduled=false;
    if(m_MsgHandler!=nullptr){
        m_MsgHandler->m_uploads.clear();
    }
    // REQ-EXT-GROUP-002 T3：断线清空出站队列（实时投递丢弃由拉取兜底）
    m_outboundQueue.clear();
    m_outboundQueueBytes=0;
    m_outboundPumpScheduled=false;
    // REQ-EXT-SESSION-001 D6：权威判定——版本不匹配=被踢旧连接，不得清在线状态、不得动登记表，
    // 否则旧连接死亡会误清新会话的在线态（验收 §6.5 竞态断言）
    bool authoritative=true;
    if(m_actorAuthenticated){
        authoritative=(m_loginVersion==MyTcpServer::getinstance().currentLoginVersion(m_strLoginName));
    }
    if(authoritative){
        OperateDB::instance().handleOffLine(m_strLoginName.toStdString().c_str());
        if(m_actorAuthenticated){
            MyTcpServer::getinstance().removeLoginVersion(m_strLoginName);
        }
    }else{
        qCWarning(lcAuth)<<"[KICK] 过期连接断开，跳过下线写以保护新会话 conn="<<m_connectionId
                         <<" user="<<m_strLoginName;
    }
    // REQ-EXT-SEC-001 T1：断线即撤销连接级 actor_context
    m_actorAuthenticated=false;
    m_loginVersion=0;
    ++m_actorSessionVersion;
    m_MsgHandler->m_actorName.clear();
    m_strLoginName.clear();
    MyTcpServer::getinstance().removeSocket(this);
    qCDebug(lcNet)<<"用户已经退出登录，业务访问已撤销 conn="<<m_connectionId<<endl;
}

// REQ-EXT-SEC-001：白名单——注册/登录/验证码/心跳可在未认证状态使用
bool MyTcpSocket::isPublicRequestType(uint uiType)
{
    switch(uiType){
    case ENUM_MSG_TYPE_REGIST_REQUEST:
    case ENUM_MSG_TYPE_LOGIN_REQUEST:
    case ENUM_MSG_TYPE_CAPTCHA_REQUEST:
    case ENUM_MSG_TYPE_HEARTBEAT_REQUEST:
        return true;
    default:
        return false;
    }
}

// REQ-EXT-SEC-001：未认证拒绝时按请求类型回对应失败响应（caData 清零即 false/空列表/异常码）
PDU* MyTcpSocket::authRejectPdu(uint uiType)
{
    auto groupReject=[&](uint responseType){
        QByteArray body;
        groupAppendU16(body, GROUP_PROTOCOL_VERSION);
        groupAppendU16(body, GROUP_ENVELOPE_LENGTH);
        groupAppendU64(body, 0);
        groupAppendU64(body, 0);
        groupAppendU32(body, quint32(-1001));
        PDU* r=mkPDU(uint(body.size()));
        r->uiType=responseType;
        memcpy(r->caMsg,body.constData(),size_t(body.size()));
        return r;
    };
    auto reject=[&](uint respType){
        PDU* r=mkPDU();
        r->uiType=respType;
        memset(r->caData,0,sizeof(r->caData));
        return r;
    };
    switch(uiType){
    case ENUM_MSG_TYPE_findUser_REQUEST:{
        PDU* r=reject(ENUM_MSG_TYPE_findUser_RESPOND); int ret=-1; memcpy(r->caData,&ret,sizeof(ret)); return r;
    }
    case ENUM_MSG_TYPE_ONLINEUSER_REQUEST:
        return reject(ENUM_MSG_TYPE_ONLINEUSER_RESPOND);
    case ENUM_MSG_TYPE_ADDFRIEND_REQUEST:{
        PDU* r=reject(ENUM_MSG_TYPE_ADDFRIEND_RESPOND); int ret=-1; memcpy(r->caData,&ret,sizeof(ret)); return r;
    }
    case ENUM_MSG_TYPE_FLUSHFRIEND_REQUEST:
        return reject(ENUM_MSG_TYPE_FLUSHFRIEND_RESPOND);
    case ENUM_MSG_TYPE_DELETEFRIEND_REQUEST:
        return reject(ENUM_MSG_TYPE_DELETEFRIEND_RESPOND);
    case ENUM_MSG_TYPE_GETCHATMSG_REQUEST:
        return reject(ENUM_MSG_TYPE_GETCHATMSG_RESPOND);
    case ENUM_MSG_TYPE_MKDIR_REQUEST:
        return reject(ENUM_MSG_TYPE_MKDIR_RESPOND);
    case ENUM_MSG_TYPE_CHANGEDIR_REQUEST:
        return reject(ENUM_MSG_TYPE_CHANGEDIR_RESPOND);
    case ENUM_MSG_TYPE_DELFILE_REQUEST:
        return reject(ENUM_MSG_TYPE_DELFILE_RESPOND);
    case ENUM_MSG_TYPE_FLUSHFILE_REQUEST:
        return reject(ENUM_MSG_TYPE_FLUSHFILE_RESPOND);
    case ENUM_MSG_TYPE_UPLOADFILE_INIT_REQUEST:
        return reject(ENUM_MSG_TYPE_UPLOADFILE_INIT_RESPOND);
    case ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_REQUEST:
        return reject(ENUM_MSG_TYPE_UPLOADFILE_FASTCHECK_RESPOND);
    // REQ-EXT-TRANSFER-001：未认证 107 → 108{ret=false}（回带 id）
    case ENUM_MSG_TYPE_UPLOADFILE_ABORT_REQUEST:{
        PDU* r=reject(ENUM_MSG_TYPE_UPLOADFILE_ABORT_RESPOND);
        quint64 tid=TRANSFER_ID_LEGACY;
        if(m_MsgHandler!=nullptr && m_MsgHandler->pdu!=nullptr){
            memcpy(&tid,m_MsgHandler->pdu->caData+TRANSFER_ID_OFFSET,sizeof(tid));
        }
        memcpy(r->caData+TRANSFER_ID_OFFSET,&tid,sizeof(tid));
        return r;
    }
    case ENUM_MSG_TYPE_RENAMEFILE_REQUEST:
        return reject(ENUM_MSG_TYPE_RENAMEFILE_RESPOND);
    case ENUM_MSG_TYPE_SHAREFILE_REQUEST:
        return reject(ENUM_MSG_TYPE_SHAREFILE_RESPOND);
    case ENUM_MSG_TYPE_SHAREFILE_AGREE_RESPOND:
        return reject(ENUM_MSG_TYPE_SHAREFILE_FINISH_RESPOND);
    case ENUM_MSG_TYPE_MOVEFILE_REQUEST:
        return reject(ENUM_MSG_TYPE_MOVEFILE_RESPOND);
    case ENUM_MSG_TYPE_LISTDIR_REQUEST:
        return reject(ENUM_MSG_TYPE_LISTDIR_RESPOND);
    case ENUM_MSG_TYPE_DOWNLOADFILE_REQUEST:
        return reject(ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND);
    case ENUM_MSG_TYPE_GETUSERINFORMATION_REQUEST:{
        PDU* r=reject(ENUM_MSG_TYPE_GETUSERINFORMATION_RESPOND); int ret=-1; memcpy(r->caData,&ret,sizeof(ret)); return r;
    }
    case ENUM_MSG_TYPE_LOGOFF_REQUEST:
        return reject(ENUM_MSG_TYPE_LOGOFF_RESPOND);
    case ENUM_MSG_TYPE_CREATE_PUBLINK_REQUEST:
        return reject(ENUM_MSG_TYPE_CREATE_PUBLINK_RESPOND);
    case ENUM_MSG_TYPE_RESOLVE_PUBLINK_REQUEST:
        return reject(ENUM_MSG_TYPE_RESOLVE_PUBLINK_RESPOND);
    case ENUM_MSG_TYPE_QUERY_FILE_PUBLINK_REQUEST:
        return reject(ENUM_MSG_TYPE_QUERY_FILE_PUBLINK_RESPOND);
    case ENUM_MSG_TYPE_CREATE_GROUP_REQUEST:
        return groupReject(ENUM_MSG_TYPE_CREATE_GROUP_RESPOND);
    case ENUM_MSG_TYPE_INVITE_GROUP_MEMBER_REQUEST:
        return groupReject(ENUM_MSG_TYPE_INVITE_GROUP_MEMBER_RESPOND);
    case ENUM_MSG_TYPE_ACCEPT_GROUP_INVITATION_REQUEST:
        return groupReject(ENUM_MSG_TYPE_ACCEPT_GROUP_INVITATION_RESPOND);
    case ENUM_MSG_TYPE_REJECT_GROUP_INVITATION_REQUEST:
        return groupReject(ENUM_MSG_TYPE_REJECT_GROUP_INVITATION_RESPOND);
    case ENUM_MSG_TYPE_LEAVE_GROUP_REQUEST:
        return groupReject(ENUM_MSG_TYPE_LEAVE_GROUP_RESPOND);
    case ENUM_MSG_TYPE_REMOVE_GROUP_MEMBER_REQUEST:
        return groupReject(ENUM_MSG_TYPE_REMOVE_GROUP_MEMBER_RESPOND);
    case ENUM_MSG_TYPE_SET_GROUP_ROLE_REQUEST:
        return groupReject(ENUM_MSG_TYPE_SET_GROUP_ROLE_RESPOND);
    case ENUM_MSG_TYPE_TRANSFER_GROUP_OWNER_REQUEST:
        return groupReject(ENUM_MSG_TYPE_TRANSFER_GROUP_OWNER_RESPOND);
    case ENUM_MSG_TYPE_DISSOLVE_GROUP_REQUEST:
        return groupReject(ENUM_MSG_TYPE_DISSOLVE_GROUP_RESPOND);
    case ENUM_MSG_TYPE_LIST_GROUPS_REQUEST:
        return groupReject(ENUM_MSG_TYPE_LIST_GROUPS_RESPOND);
    case ENUM_MSG_TYPE_LIST_GROUP_MEMBERS_REQUEST:
        return groupReject(ENUM_MSG_TYPE_LIST_GROUP_MEMBERS_RESPOND);
    case ENUM_MSG_TYPE_QUERY_GROUP_INVITATIONS_REQUEST:
        return groupReject(ENUM_MSG_TYPE_QUERY_GROUP_INVITATIONS_RESPOND);
    case ENUM_MSG_TYPE_QUERY_GROUP_SUMMARY_REQUEST:
        return groupReject(ENUM_MSG_TYPE_QUERY_GROUP_SUMMARY_RESPOND);
    // REQ-EXT-GROUP-002：群消息请求的未认证拒绝
    case ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_REQUEST:
        return groupReject(ENUM_MSG_TYPE_GROUP_SEND_MESSAGE_RESPOND);
    case ENUM_MSG_TYPE_GROUP_PULL_MESSAGES_REQUEST:
        return groupReject(ENUM_MSG_TYPE_GROUP_PULL_MESSAGES_RESPOND);
    case ENUM_MSG_TYPE_GROUP_READ_CONFIRM_REQUEST:
        return groupReject(ENUM_MSG_TYPE_GROUP_READ_CONFIRM_RESPOND);
    case ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_REQUEST:
        return groupReject(ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_RESPOND);
    // REQ-EXT-GROUP-003：群文件请求的未认证拒绝
    case ENUM_MSG_TYPE_GROUP_FILE_LIST_REQUEST:
        return groupReject(ENUM_MSG_TYPE_GROUP_FILE_LIST_RESPOND);
    case ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_REQUEST:
        return groupReject(ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_RESPOND);
    case ENUM_MSG_TYPE_GROUP_FILE_DELETE_REQUEST:
        return groupReject(ENUM_MSG_TYPE_GROUP_FILE_DELETE_RESPOND);
    case ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_REQUEST:
        return groupReject(ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_RESPOND);
    case ENUM_MSG_TYPE_CHAT_REQUEST:
    case ENUM_MSG_TYPE_ADDFRIEND_RESPOND:
        return nullptr; // 无响应语义的投递型请求，拒绝仅落审计日志
    default:
        return nullptr;
    }
}
//发送 PDU 到客户端并释放其内存（所有权转移到此方法）
//pdu 为 nullptr 时安全空操作
void MyTcpSocket::sendAndRelease(PDU* pdu)
{
    if (pdu == nullptr) return;
    write((char*)pdu, pdu->uiTotalLen);
    memfree(pdu);
}
// 发送 PDU 给客户端。pdu 必须由 Server 端 mkPDU() (memalloc) 分配。
// 发送完成后调用 memfree(pdu) 释放。
// 调用方不得在 seedMsg 返回后再访问 pdu。
//给客户端发送respdu消息
void MyTcpSocket::seedMsg(PDU *respdu){
    if(respdu==nullptr){
        return;
    }
    this->write((char*)respdu,respdu->uiTotalLen);
    qCDebug(lcNet)<<"响应信息 已发送:"<<" "
            <<"respdu->uiTotalLen"<<respdu->uiTotalLen<<" "
            <<"respdu->uiMsgLen"<<respdu->uiMsgLen<<" "
            <<"respdu->uiType"<<respdu->uiType<<" "
              <<"respdu->caData"<<respdu->caData<<" "
            <<"respdu->caData+32"<<respdu->caData+32<<" "
            <<(respdu->uiType==ENUM_MSG_TYPE_DOWNLOADFILE_DATA ? QByteArray::number(respdu->uiMsgLen).constData() :
               respdu->uiType==ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND ? "metadata" : "respdu->caMsg")
            <<endl;
    memfree(respdu);
    respdu=nullptr;
}
