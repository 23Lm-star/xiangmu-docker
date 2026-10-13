#ifndef GROUPCHAT_H
#define GROUPCHAT_H

#include "protocol.h"
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QWidget>

class QHideEvent;
class QTimer;
class GroupChatState;

// REQ-EXT-GROUP-002：单群聊天窗口（可靠文本消息）。
// 发送 88 → 89 已持久化确认；接收 90 在线事件（与拉取按 seq 去重）；
// 打开窗口经 95/96 定位未读游标后按 91/92 前向补齐，上翻历史走后向翻页；
// 已读经 93/94 单调推进。Group-001 的群事件（84/85）处理不变。
//
// REQ-EXT-GROUP-004：本窗口只是"会话级群读状态"的读写者，自己不再持有游标。
// 上报锚点恒为「已连续拿到的最长前缀的末尾」，绝不是已渲染的最大 seq——
// 实时投递会丢（服务端出站队列满即丢弃），报最大会让洞内消息被永久判为已读。
class GroupChat : public QWidget
{
    Q_OBJECT
public:
    GroupChat(quint64 groupId, const QString &groupName, QWidget *parent = nullptr);
    quint64 groupId() const { return m_groupId; }
    // 89/92/94 响应路由（按信封 group_id 匹配）
    void handleResponse(PDU *pdu);
    // 90 在线消息事件
    void handleMessageEvent(PDU *pdu);
    // 服务端摘要（96）落地后通知：按新锚点续补/上报，窗口未开或无新锚点时为空操作
    void onAnchorUpdated();

protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private slots:
    void sendCurrent();
    void loadOlder();
    void onItemDoubleClicked(QListWidgetItem *item);

private:
    void sendRequest(uint type, const QByteArray &body = QByteArray());
    QByteArray envelope(const QByteArray &body);
    void requestPull(quint8 mode, quint64 cursor, quint16 limit);
    GroupChatState &groupState();                  // Client 持有的会话级群读状态
    void onMessageArrived(quint64 seq);      // 唯一推进入口（实时/自己消息/拉取共用）
    void maybeCatchUp();                     // 需要且允许时发起前向补洞拉取
    void scheduleReadReport(bool immediate); // 上报（节流合并 / 立即）
    void flushReadReport();
    void updateStatus();
    QListWidgetItem *insertMessage(quint64 seq, const QString &sender, quint8 msgType,
                                   quint64 createdAtMs, const QString &content);
    void markRendered(quint64 seq);
    // REQ-UX-001 A3：自己的消息统一"身份/时间行 + 正文行"右对齐重绘
    void renderOwnMessage(QListWidgetItem *item, const QString &state, const QString &content);

    quint64 m_groupId;
    QString m_groupName;
    QListWidget *m_messageList;
    QLineEdit *m_input;
    QPushButton *m_sendButton;
    QPushButton *m_historyButton;
    QLabel *m_statusLabel;
    quint64 m_requestId = 0;
    bool m_summaryRequested = false;
    // 渲染去重：seq 全局唯一，90 与 92 同源
    QHash<quint64, bool> m_renderedSeqs;
    quint64 m_firstSeq = 0;        // 已渲染最小 seq（上翻游标）
    QTimer *m_readReportTimer;     // 已读上报节流（固定 400ms 速率的合并器）
    bool m_catchupInFlight = false; // 补洞拉取在途（防拉风暴）
    int m_catchupBatches = 0;      // 前向补齐批次计数（上限防长历史全量拉）
    static const int MAX_CATCHUP_BATCHES = 20;
    static const int READ_REPORT_THROTTLE_MS = 400;
    struct PendingSend { QString clientMsgId; QString content; QListWidgetItem *item; };
    QList<PendingSend> m_pending;
    // 列表项数据角色
    static const int ROLE_SEQ = Qt::UserRole;          // quint64 seq
    static const int ROLE_STATE = Qt::UserRole + 1;    // 0 发送中 / 1 已发送 / 2 发送失败 / 3 系统
    static const int ROLE_CLIENT_KEY = Qt::UserRole + 2;
    static const int ROLE_TIME = Qt::UserRole + 3;     // 本地乐观条目的时间戳显示
};

#endif // GROUPCHAT_H
