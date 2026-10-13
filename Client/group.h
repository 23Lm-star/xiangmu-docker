#ifndef GROUP_H
#define GROUP_H

#include "protocol.h"
#include "groupchat.h"
#include "groupfile.h"
#include <QComboBox>
#include <QDialog>
#include <QHash>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QWidget>

class QLabel;

// 群聊模块：进入页面后按需查询群列表和邀请；REQ-EXT-GROUP-002 起承载群消息
// 窗口打开入口与未读徽标（消息正文在 GroupChat 窗口内处理）。
class Group : public QWidget
{
    Q_OBJECT
public:
    explicit Group(QWidget *parent = nullptr);
    void handleResponse(PDU *pdu);
    // REQ-EXT-GROUP-002：89/92/94 响应路由到对应群窗口
    void handleMessageResponse(PDU *pdu);
    // REQ-EXT-GROUP-002：90 在线消息事件按 group_id 路由
    void handleMessageEvent(PDU *pdu);
    // REQ-EXT-GROUP-003：100/102/104 群文件响应按 group_id 路由到群文件窗口
    void handleFileResponse(PDU *pdu);
    // REQ-EXT-GROUP-004：会话建立后取回全部群的未读锚点（群水位 + 我的已读游标），
    // 一次性建立会话级群读状态，使状态就绪与窗口是否打开解耦。
    void initializeSession();

protected:
    void showEvent(QShowEvent *event) override;

private slots:
    void createGroup();
    void refreshGroups();
    void refreshInvitations();
    void showInvitationDialog();    // 待处理邀请弹窗（列表选中条目 → 接受/拒绝）
    void inviteMember();
    void acceptInvitation();
    void rejectInvitation();
    void listMembers();
    void leaveGroup();
    void removeMember();
    void setRole();
    void transferOwner();
    void dissolveGroup();
    void openSelectedChat();
    void openSelectedFiles();

private:
    void sendRequest(uint type, quint64 groupId = 0, const QByteArray &body = QByteArray());
    QByteArray envelope(quint64 groupId, const QByteArray &body);
    quint64 selectedGroupId() const;
    quint64 selectedInvitationId() const;
    void requestUnreadSummary();
    void refreshUnreadBadge(quint64 groupId, quint64 unread);
    void ensureInvitationDialog();          // 惰性创建邀请弹窗
    QString groupErrorText(quint32 code) const;   // 服务端错误码 → 可读中文
    // 信封首 20B 提取 group_id（不满足最小长度返回 0）
    static quint64 envelopeGroupId(PDU *pdu);
    QPushButton *m_createButton;
    QPushButton *m_refreshButton;
    QPushButton *m_fileButton;      // REQ-EXT-GROUP-003：打开选中群的群文件窗口
    QPushButton *m_invitationButton;
    QListWidget *m_groupList;
    QListWidget *m_invitationList;  // 挂在邀请弹窗内（不再占主布局）
    QLineEdit *m_targetEdit;
    QComboBox *m_roleBox;
    QLabel *m_statusLabel;
    QDialog *m_invitationDialog;    // 待处理邀请弹窗（惰性创建）
    quint64 m_requestId;
    bool m_loaded;
    QHash<quint64, GroupChat*> m_chatWindows;   // group_id → 窗口（应用生命周期内保留）
    QHash<quint64, GroupFile*> m_fileWindows;   // group_id → 群文件窗口
    QHash<quint64, quint64> m_unreadByGroup;    // group_id → 未读数（96 回带）
};

#endif // GROUP_H
