#ifndef CHAT_H
#define CHAT_H

#include <QWidget>

namespace Ui {
class Chat;
}

class Chat : public QWidget
{
    Q_OBJECT

public:
    QString m_strChatName;//聊天对象
    void updateShow_TE(QString Msg);//更新聊天框 - 一条信息
    void updateShow_TEs(QStringList Msgs);//更新聊天框 - 多条信息
    explicit Chat(QWidget *parent = nullptr);
    ~Chat();

private slots:
    void on_output_PB_clicked();//点击聊天消息发送按钮->发送消息

private:
    Ui::Chat *ui;
};

#endif // CHAT_H
