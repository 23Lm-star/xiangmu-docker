#ifndef ONLINEUSER_H
#define ONLINEUSER_H

#include <QWidget>
#include <qlistwidget.h>
#include "client.h"
namespace Ui {
class OnlineUser;
}

class OnlineUser : public QWidget
{
    Q_OBJECT

public:
    explicit OnlineUser(QWidget *parent = nullptr);
    ~OnlineUser();
    //更新列表中的在线用户
    void updateOnlineUser(const QStringList& userNames);
    // REQ-UX-001 A1：外部（响应处理）更新顶部提示条，保证操作结果可见
    void setHint(const QString &text);
private slots:
    // 单击：选中并给出可见反馈
    void on_listWidget_itemClicked(QListWidgetItem *item);
    void on_listWidget_itemDoubleClicked(QListWidgetItem *item);

private:
    Ui::OnlineUser *ui;
};

#endif // ONLINEUSER_H
