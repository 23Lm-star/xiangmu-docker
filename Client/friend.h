#ifndef FRIEND_H
#define FRIEND_H

#include "onlineuser.h"

#include <QWidget>
#include <QShowEvent>
#include <QTimer>
#include <QStringList>
#include "chat.h"
namespace Ui {
class Friend;
}
//好友类
class Friend : public QWidget
{
    Q_OBJECT

public:
    //在线用户的子框架的指针
    OnlineUser* m_onlineUser;
    //聊天界面的指针
    Chat* m_chat;
    //刷新好友接口函数->引导刷新好友
    void flushfriend();
    QListWidget* getfile_LW();
    //刷新好友操作函数——仅接管数据，不直接写 QListWidget
    void flushFriend_LW(const QStringList& q);
    explicit Friend(QWidget *parent = nullptr);
    ~Friend();

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    //查询好友槽函数
    void on_findUser_PB_clicked();
    //获取所有用户（非好友）
    void on_onlineUser_PB_clicked();
    //刷新好友槽函数
    void on_flush_PB_clicked();
    //删除好友槽函数
    void on_del_PB_clicked();
    //聊天槽函数
    void on_chat_PB_clicked();

private:
    Ui::Friend *ui;
    // 缓存的好友列表数据
    QStringList m_friendList;
    // 存在待处理的 UI 刷新数据
    bool m_pendingUiRefresh = false;
    // 已有一个 UI 刷新排队中
    bool m_uiRefreshQueued = false;
    // 请求一次合并排队 UI 刷新
    void scheduleUiRefresh();
    // 唯一可写 QListWidget 的私有方法
    void rebuildFriendList();
};

#endif // FRIEND_H
