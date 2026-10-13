#include "logging.h"
#include "onlineuser.h"
#include "ui_onlineuser.h"

#include <QMessageBox>
#include <cstring>

namespace {
void copyFixedField(char *dst, int dstSize, const QByteArray &src)
{
    memset(dst, 0, dstSize);
    memcpy(dst, src.constData(), qMin(src.size(), dstSize - 1));
}
}

OnlineUser::OnlineUser(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::OnlineUser)
{
    setAttribute(Qt::WA_StyledBackground, true);   // QSS 背景在 QWidget 子类上必须显式开启
    ui->setupUi(this);
    ui->hintLabel->setText(QStringLiteral("单击选中，双击添加好友"));
}

OnlineUser::~OnlineUser()
{
    delete ui;
}

void OnlineUser::setHint(const QString &text)
{
    ui->hintLabel->setText(text);
}

void OnlineUser::updateOnlineUser( const QStringList &userNames){
    //清理列表
    ui->listWidget->clear();
    //重新添加所有的在线用户
    ui->listWidget->addItems(userNames);
    ui->hintLabel->setText(userNames.isEmpty()
            ? QStringLiteral("当前没有其他在线用户")
            : QStringLiteral("单击选中，双击添加好友"));
}

// REQ-UX-001 A1：单击必须有可见反馈（旧实现只有双击槽，单击完全无响应）
void OnlineUser::on_listWidget_itemClicked(QListWidgetItem *item)
{
    if(item == nullptr){
        return;
    }
    ui->hintLabel->setText(QStringLiteral("已选中：%1　·　双击添加好友").arg(item->text()));
}

//在线用户 双击添加好友（先确认再发送，并给出可见结果反馈）
void OnlineUser::on_listWidget_itemDoubleClicked(QListWidgetItem *item)
{
    if(item == nullptr){
        return;
    }
    // REQ-UX-001 A1：列表项文本即用户名（旧实现为 "<序号>.<用户名>" 并按首个 '.' 解析，
    // 用户名含 '.' 时解析出错；列表生成侧已改为纯用户名）
    const QString target = item->text().trimmed();
    if(target.isEmpty()){
        return;
    }
    const QString myName=Client::getinstance().m_LoginName;
    if(target == myName){
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("不能添加自己为好友"));
        return;
    }
    if(QMessageBox::question(this, QStringLiteral("添加好友"),
                             QStringLiteral("是否添加 %1 为好友？").arg(target),
                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes){
        return;
    }
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_ADDFRIEND_REQUEST;
    copyFixedField(pdu->caData,32,myName.toUtf8());
    copyFixedField(pdu->caData+32,32,target.toUtf8());
    Client::getinstance().seedMsg(pdu);
    // REQ-UX-001 A1：旧实现该路径无任何客户端提示（且目标在线时服务端不回响应），
    // 用户完全看不到"点了有反应"。此处立即反馈，最终结果由响应处理更新提示条。
    ui->hintLabel->setText(QStringLiteral("已向 %1 发送好友请求，等待对方同意…").arg(target));
    QMessageBox::information(this, QStringLiteral("提示"),
                             QStringLiteral("已向 %1 发送好友请求，等待对方同意").arg(target));
}
