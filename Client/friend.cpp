#include "logging.h"
#include "friend.h"
#include "protocol.h"
#include "ui_friend.h"
#include "client.h"
#include <QInputDialog>
#include <qdebug.h>
Friend::Friend(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Friend)
{
    setAttribute(Qt::WA_StyledBackground, true);   // QSS 背景在 QWidget 子类上必须显式开启
    ui->setupUi(this);
    m_onlineUser=new OnlineUser;
    m_chat=new Chat;
}

Friend::~Friend()
{
    delete ui;
    delete m_onlineUser;
    delete m_chat;
}

void Friend::flushfriend()
{
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_FLUSHFRIEND_REQUEST;
    Client::getinstance().seedMsg(pdu);
}

QListWidget *Friend::getfile_LW()
{
    return  ui->listWidget;
}

// flushFriend_LW：仅接管好友列表数据，不直接写 QListWidget
void Friend::flushFriend_LW(const QStringList& q)
{
    qCDebug(lcCli) << "[Friend] flushFriend_LW: 缓存" << q.size() << "个好友条目";
    m_friendList = q;
    m_pendingUiRefresh = true;
    scheduleUiRefresh();
}

// scheduleUiRefresh：合并排队——最多排入一个 UI 刷新回调
void Friend::scheduleUiRefresh()
{
    // 无待处理数据则跳过
    if (!m_pendingUiRefresh) {
        return;
    }
    // 不可见时不排队，等 showEvent 触发
    if (!isVisible()) {
        qCDebug(lcCli) << "[Friend] scheduleUiRefresh: 页面不可见，延迟到 showEvent";
        return;
    }
    // 已排队则跳过（合并去重）
    if (m_uiRefreshQueued) {
        return;
    }
    m_uiRefreshQueued = true;
    qCDebug(lcCli) << "[Friend] scheduleUiRefresh: 排入一次 UI 刷新";
    QTimer::singleShot(0, this, [this]() {
        m_uiRefreshQueued = false;
        // 回调执行时重新检查可见性与待刷新标志
        if (isVisible() && m_pendingUiRefresh) {
            qCDebug(lcCli) << "[Friend] UI 刷新：开始重建";
            rebuildFriendList();
            qCDebug(lcCli) << "[Friend] UI 刷新：完成";
        }
    });
}

// rebuildFriendList：唯一可写 QListWidget 的位置
void Friend::rebuildFriendList()
{
    ui->listWidget->clear();
    ui->listWidget->addItems(m_friendList);
    m_pendingUiRefresh = false;
}

void Friend::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    qCDebug(lcCli) << "[Friend] showEvent: 页面变为可见，pending=" << m_pendingUiRefresh;
    // 仅在页面可见后请求合并排队重建
    scheduleUiRefresh();
}

void Friend::on_findUser_PB_clicked()
{
    QString strname=QInputDialog::getText(this,"查找用户","用户名");
    qCDebug(lcCli)<<strname<<endl;
    if(strname==nullptr){
        return ;
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_findUser_REQUEST;
    memcpy(pdu->caData,strname.toStdString().c_str(),32);
    Client::getinstance().seedMsg(pdu);
}

void Friend::on_onlineUser_PB_clicked()
{
    if(m_onlineUser->isHidden()){
        m_onlineUser->show();
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_ONLINEUSER_REQUEST;
    Client::getinstance().seedMsg(pdu);
}

void Friend::on_flush_PB_clicked()
{
   flushfriend();
}

void Friend::on_del_PB_clicked()
{
    QListWidgetItem* tar =ui->listWidget->currentItem();
    if(!tar){
        return ;
    }
    QString tarname=tar->text();
    int cnt=0;
    for(int i=0;i<tarname.size();i++){
        if(tarname[i]=='.'){
            cnt=i;break;
        }
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_DELETEFRIEND_REQUEST;
    memcpy(pdu->caData,tarname.toStdString().c_str()+cnt+1,32);
    memcpy(pdu->caData+32,Client::getinstance().m_LoginName.toStdString().c_str(),32);
    Client::getinstance().seedMsg(pdu);
}

void Friend::on_chat_PB_clicked()
{
    QListWidgetItem* pitem=ui->listWidget->currentItem();//获取当前聊天的对象
    if(pitem==nullptr){//如果聊天对象为nullptr，那么就不能进行聊天
        return;
    }
    //取出聊天对象的名字（去除前面的序号）
    char pName[32]={'\0'};
    int cnt=0;
    for(int i=0;i<pitem->text().size();i++){
        if(pitem->text()[i]=='.'){
            cnt=i;
            break;
        }
    }
    memcpy(pName,pitem->text().toStdString().c_str()+cnt+1,32);
    m_chat->m_strChatName=pName;//记录聊天对象
    if(m_chat->isHidden()){
        m_chat->show();
        m_chat->setWindowTitle(m_chat->m_strChatName);
        PDU* pdu= mkPDU();
        pdu->uiType=ENUM_MSG_TYPE_GETCHATMSG_REQUEST;
        memcpy(pdu->caData,Client::getinstance().m_LoginName.toStdString().c_str(),32);
        memcpy(pdu->caData+32,m_chat->m_strChatName.toStdString().c_str(),32);
        Client::getinstance().seedMsg(pdu);
    }
}
