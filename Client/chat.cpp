#include "logging.h"
#include "chat.h"
#include "client.h"
#include "protocol.h"
#include "ui_chat.h"
//构造函数
Chat::Chat(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Chat){
    setAttribute(Qt::WA_StyledBackground, true);   // QSS 背景在 QWidget 子类上必须显式开启
    ui->setupUi(this);
}
Chat::~Chat(){
    delete ui;
}
//往消息框中添加消息—————————————————————————————————————————————————————————————
//向消息框里添加一条消息
void Chat::updateShow_TE(QString Msg){
    ui->show_TE->append(Msg);
}
//向消息框里添加一系列消息
void Chat::updateShow_TEs(QStringList Msgs){
    for(int i=0;i<Msgs.size();i+=2){
        ui->show_TE->append(m_strChatName+":"+Msgs[i]+"  发送时间:"+Msgs[i+1]);
    }
}
//发送聊天消息——————————————————————————————————————————————————————————————————
//发送聊天消息（一条）
void Chat::on_output_PB_clicked(){
    //获取发送消息，还有当前时间
    QString msg=ui->output_LE->text().toUtf8().toStdString().c_str();
    QString currtime=(QDateTime::currentDateTime()).toString();
    //判断消息是否为空，为空 不发送
    if(msg.isEmpty()){
        return;
    }
    //清空消息发送框
    ui->output_LE->clear();
    uint Msglen=sizeof(Msg)+msg.toUtf8().toStdString().size()+1;
    PDU* pdu=mkPDU(Msglen);//构建一个Msg大小还有一个消息的柔性数组大小
    pdu->uiType=ENUM_MSG_TYPE_CHAT_REQUEST;
    //现将发送人和接收人的用户名记录下来
    memcpy(pdu->caData,Client::getinstance().m_LoginName.toStdString().c_str(),32);
    memcpy(pdu->caData+32,m_strChatName.toStdString().c_str(),32);
    //构造一条消息（消息本体+发送时间）
    Msg* pdu_msg=(Msg*)pdu->caMsg;
    pdu_msg->totalLen=Msglen;//消息总长度（+消息本体长度+1）
    pdu_msg->msgLen=msg.toUtf8().toStdString().size()+1;
    qCDebug(lcCli)<<"pdu_msg->totalLen"<<pdu_msg->totalLen<<" pdu_msg->msgLen"<<pdu_msg->msgLen<<" ";
    memcpy(pdu_msg->Time,QDateTime::currentDateTime().toString().toStdString().c_str(),30);
    memcpy(pdu_msg->msg,msg.toUtf8().toStdString().c_str(),pdu_msg->msgLen);
    qCDebug(lcCli)<<"发送msg"<<pdu_msg->msg<<" time:"<<pdu_msg->Time<<" ";
    qCDebug(lcCli)<<"报文长度为:"<<pdu->uiTotalLen<<" msgLen:"<<pdu->uiMsgLen<<endl;
    Client::getinstance().seedMsg(pdu);
}
