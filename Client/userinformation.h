#ifndef USERINFORMATION_H
#define USERINFORMATION_H

#include "protocol.h"

#include <QWidget>

namespace Ui {
class UserInformation;
}

class UserInformation : public QWidget
{
    Q_OBJECT
public:
    static UserInformation& getinstance();
    QString userName;//用户名
    QString TiName;//昵称
    int age;//年龄
    QString sex;//性别
    QString title;//签名
    QStringList address;//所在地(省/市/县)
    QString career;//职业
    QStringList school;//学校（小学/初中/高中/大学）
    void setMsg();
    void fluMsg(QStringList msg);
    void fluMsgHandl();
private:
    explicit UserInformation(QWidget *parent = nullptr);
    UserInformation(const UserInformation&)=delete;
    UserInformation& operator=(const UserInformation&)=delete;
public:

    ~UserInformation();

private:
    Ui::UserInformation *ui;
};

#endif // USERINFORMATION_H
