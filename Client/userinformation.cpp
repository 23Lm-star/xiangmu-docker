#include "userinformation.h"
#include "ui_userinformation.h"
#include "client.h"
//单例模式
UserInformation &UserInformation::getinstance()
{
    static UserInformation instance;
    return instance;
}
//设置信息
void UserInformation::setMsg()
{
    QString LoginName=Client::getinstance().m_LoginName;
    PDU* pdu=mkPDU();
    pdu->uiType=ENUM_MSG_TYPE_GETUSERINFORMATION_REQUEST;
    memcpy(pdu->caData,LoginName.toStdString().c_str(),32);
    Client::getinstance().seedMsg(pdu);
}
//刷新信息
void UserInformation::fluMsg(QStringList msg){
    userName=msg[0];
    TiName=msg[1];
    memcpy(&age,msg[2].toStdString().c_str(),sizeof (age));
    int sexx;
    memcpy(&sexx,msg[3].toStdString().c_str(),sizeof (sexx));
    if(sexx){sex="男";}
    else { sex="女";}
    address.append(QString(msg[4]));
    address.append(QString(msg[5]));
    address.append(QString(msg[6]));
    career=msg[7];
    school.append(QString(msg[8]));
    school.append(QString(msg[9]));
    school.append(QString(msg[10]));
    school.append(QString(msg[11]));
    title=msg[12];
    fluMsgHandl();
}
//刷新信息操作
void UserInformation::fluMsgHandl()
{
    ui->TiName__LB->setText(TiName);
    ui->age_LB->setText(QString(age));
    ui->title_LB->setText(title);
    ui->userName_LB->setText(userName);
    ui->career_LB->setText(career);
    ui->sex_LB->setText(sex);
    ui->province_LB->setText(address[0]);
    ui->county_LB->setText(address[1]);
    ui->city_LB->setText(address[2]);
    ui->school1_LB->setText(school[0]);
    ui->school2_LB->setText(school[1]);
    ui->school3_LB->setText(school[2]);
    ui->school4_LB->setText(school[3]);
}

UserInformation::UserInformation(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::UserInformation)
{
    setAttribute(Qt::WA_StyledBackground, true);   // QSS 背景在 QWidget 子类上必须显式开启
    ui->setupUi(this);
}

UserInformation::~UserInformation()
{
    delete ui;
}
