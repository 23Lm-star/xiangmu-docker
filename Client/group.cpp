#include "group.h"

#include "client.h"

#include <QInputDialog>
#include <QDateTime>
#include <QLabel>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QShowEvent>
#include <QVariant>
#include <QVBoxLayout>
#include <cstring>

Group::Group(QWidget *parent)
    : QWidget(parent),
      m_createButton(new QPushButton(QStringLiteral("建群"), this)),
      m_refreshButton(new QPushButton(QStringLiteral("刷新群聊"), this)),
      m_fileButton(new QPushButton(QStringLiteral("群文件"), this)),
      m_invitationButton(new QPushButton(QStringLiteral("待处理邀请"), this)),
      m_groupList(new QListWidget(this)),
      m_invitationList(new QListWidget(this)),
      m_targetEdit(new QLineEdit(this)),
      m_roleBox(new QComboBox(this)),
      m_statusLabel(new QLabel(this)),
      m_requestId(0),
      m_loaded(false)
{
    setAttribute(Qt::WA_StyledBackground, true);   // QSS 背景在 QWidget 子类上必须显式开启
    m_invitationDialog = nullptr;
    auto *layout = new QVBoxLayout(this);
    auto *toolbar = new QHBoxLayout;
    toolbar->addWidget(m_createButton);
    toolbar->addWidget(m_refreshButton);
    toolbar->addWidget(m_fileButton);
    toolbar->addWidget(m_invitationButton);
    layout->addLayout(toolbar);
    auto *actionBar = new QHBoxLayout;
    // 操作均作用于「选中的群」+「目标用户输入框」；tooltip 说明各按钮用途（主人反馈看不懂移除/设角色）
    const QList<QPair<QString, void (Group::*)()>> actions = {
        {QStringLiteral("邀请"), &Group::inviteMember},
        {QStringLiteral("成员列表"), &Group::listMembers},
        {QStringLiteral("退出"), &Group::leaveGroup},
        {QStringLiteral("移除成员"), &Group::removeMember},
        {QStringLiteral("设角色"), &Group::setRole},
        {QStringLiteral("转让群主"), &Group::transferOwner},
        {QStringLiteral("解散"), &Group::dissolveGroup}
    };
    const QList<QString> tips = {
        QStringLiteral("邀请「目标用户」加入选中的群（需群主/管理员）"),
        QStringLiteral("列出选中群的全部成员及角色"),
        QStringLiteral("退出选中的群（群主不能退出）"),
        QStringLiteral("把「目标用户」移出选中的群（需群主/管理员，仅普通成员可移除）"),
        QStringLiteral("群主把「目标用户」设为/撤销管理员（角色在右侧下拉选）"),
        QStringLiteral("把群主转给「目标用户」，自己变为管理员"),
        QStringLiteral("解散选中的群（仅群主，不可恢复）")
    };
    for (int i = 0; i < actions.size(); ++i) {
        auto *button = new QPushButton(actions.at(i).first, this);
        button->setToolTip(tips.at(i));
        connect(button, &QPushButton::clicked, this, actions.at(i).second);
        actionBar->addWidget(button);
    }
    m_targetEdit->setPlaceholderText(QStringLiteral("目标用户名"));
    m_targetEdit->setToolTip(QStringLiteral("邀请/移除/设角色/转让的作用对象（输入对方用户名）"));
    m_roleBox->addItem(QStringLiteral("member"));
    m_roleBox->addItem(QStringLiteral("admin"));
    m_roleBox->setToolTip(QStringLiteral("「设角色」使用的目标角色"));
    actionBar->addWidget(m_targetEdit);
    actionBar->addWidget(m_roleBox);
    layout->addLayout(actionBar);
    layout->addWidget(m_statusLabel);
    layout->addWidget(m_groupList);
    // 邀请列表已移入「待处理邀请」弹窗（主人反馈：接受/拒绝应作用于弹窗内选中条目）
    connect(m_createButton, &QPushButton::clicked, this, &Group::createGroup);
    connect(m_refreshButton, &QPushButton::clicked, this, &Group::refreshGroups);
    connect(m_invitationButton, &QPushButton::clicked, this, &Group::showInvitationDialog);
    // REQ-EXT-GROUP-003：打开选中群的群文件目录窗口
    connect(m_fileButton, &QPushButton::clicked, this, &Group::openSelectedFiles);
    // REQ-EXT-GROUP-002：双击群条目打开群聊窗口
    connect(m_groupList, &QListWidget::itemDoubleClicked, this, &Group::openSelectedChat);
    m_statusLabel->setText(QStringLiteral("群聊按需加载；双击群条目打开群聊窗口"));
}

quint64 Group::selectedGroupId() const
{
    const QListWidgetItem *item = m_groupList->currentItem();
    return item == nullptr ? 0 : item->data(Qt::UserRole).toULongLong();
}

quint64 Group::selectedInvitationId() const
{
    const QListWidgetItem *item = m_invitationList->currentItem();
    return item == nullptr ? 0 : item->data(Qt::UserRole).toULongLong();
}

void Group::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    if (!m_loaded) {
        m_loaded = true;
        refreshGroups();
        refreshInvitations();
    }
}

QByteArray Group::envelope(quint64 groupId, const QByteArray &body)
{
    QByteArray payload;
    groupAppendU16(payload, GROUP_PROTOCOL_VERSION);
    groupAppendU16(payload, GROUP_ENVELOPE_LENGTH);
    groupAppendU64(payload, ++m_requestId);
    groupAppendU64(payload, groupId);
    payload.append(body);
    return payload;
}

void Group::sendRequest(uint type, quint64 groupId, const QByteArray &body)
{
    const QByteArray payload = envelope(groupId, body);
    PDU *pdu = mkPDU(uint(payload.size()));
    pdu->uiType = type;
    memcpy(pdu->caMsg, payload.constData(), size_t(payload.size()));
    Client::getinstance().seedMsg(pdu);
}

void Group::createGroup()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, QStringLiteral("建群"), QStringLiteral("群名称"),
                                               QLineEdit::Normal, QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;
    QByteArray body;
    groupAppendString(body, name.trimmed().toUtf8());
    sendRequest(ENUM_MSG_TYPE_CREATE_GROUP_REQUEST, 0, body);
}

void Group::refreshGroups()
{
    sendRequest(ENUM_MSG_TYPE_LIST_GROUPS_REQUEST);
    requestUnreadSummary();
}

// REQ-EXT-GROUP-002：95 无业务字段，按 actor 汇总全部 active 群；96 回带未读徽标
void Group::requestUnreadSummary()
{
    sendRequest(ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_REQUEST);
}

void Group::initializeSession()
{
    requestUnreadSummary();
}

void Group::refreshInvitations()
{
    sendRequest(ENUM_MSG_TYPE_QUERY_GROUP_INVITATIONS_REQUEST);
}

void Group::inviteMember()
{
    const quint64 groupId = selectedGroupId();
    if (groupId == 0) {
        m_statusLabel->setText(QStringLiteral("请先在列表中选择一个群，再点邀请"));
        return;
    }
    const QString target = m_targetEdit->text().trimmed();
    if (target.isEmpty()) {
        m_statusLabel->setText(QStringLiteral("请在「目标用户名」框输入对方用户名，再点邀请"));
        return;
    }
    QByteArray body;
    groupAppendString(body, target.toUtf8());
    groupAppendString(body, QStringLiteral("client-%1").arg(QDateTime::currentMSecsSinceEpoch()).toUtf8());
    sendRequest(ENUM_MSG_TYPE_INVITE_GROUP_MEMBER_REQUEST, groupId, body);
}

// 邀请弹窗：列表中选中条目 → 接受/拒绝按钮生效（主人要求的交互）
void Group::showInvitationDialog()
{
    ensureInvitationDialog();
    m_invitationDialog->show();
    m_invitationDialog->raise();
    m_invitationDialog->activateWindow();
    refreshInvitations();
}

void Group::ensureInvitationDialog()
{
    if (m_invitationDialog != nullptr) return;
    m_invitationDialog = new QDialog(this);
    m_invitationDialog->setWindowTitle(QStringLiteral("待处理邀请"));
    m_invitationDialog->resize(420, 320);
    auto *dialogLayout = new QVBoxLayout(m_invitationDialog);
    auto *hint = new QLabel(QStringLiteral("选中一条邀请后点「接受」或「拒绝」"), m_invitationDialog);
    dialogLayout->addWidget(hint);
    dialogLayout->addWidget(m_invitationList);
    auto *buttonRow = new QHBoxLayout;
    auto *acceptButton = new QPushButton(QStringLiteral("接受"), m_invitationDialog);
    auto *rejectButton = new QPushButton(QStringLiteral("拒绝"), m_invitationDialog);
    auto *reloadButton = new QPushButton(QStringLiteral("刷新"), m_invitationDialog);
    acceptButton->setToolTip(QStringLiteral("接受选中的邀请并加入该群"));
    rejectButton->setToolTip(QStringLiteral("拒绝选中的邀请"));
    connect(acceptButton, &QPushButton::clicked, this, &Group::acceptInvitation);
    connect(rejectButton, &QPushButton::clicked, this, &Group::rejectInvitation);
    connect(reloadButton, &QPushButton::clicked, this, &Group::refreshInvitations);
    buttonRow->addWidget(acceptButton);
    buttonRow->addWidget(rejectButton);
    buttonRow->addWidget(reloadButton);
    dialogLayout->addLayout(buttonRow);
}

void Group::acceptInvitation()
{
    const quint64 invitationId = selectedInvitationId();
    if (invitationId == 0) {
        m_statusLabel->setText(QStringLiteral("请先在邀请列表中选中一条邀请"));
        return;
    }
    QByteArray body;
    groupAppendU64(body, invitationId);
    sendRequest(ENUM_MSG_TYPE_ACCEPT_GROUP_INVITATION_REQUEST, 0, body);
}

void Group::rejectInvitation()
{
    const quint64 invitationId = selectedInvitationId();
    if (invitationId == 0) {
        m_statusLabel->setText(QStringLiteral("请先在邀请列表中选中一条邀请"));
        return;
    }
    QByteArray body;
    groupAppendU64(body, invitationId);
    sendRequest(ENUM_MSG_TYPE_REJECT_GROUP_INVITATION_REQUEST, 0, body);
}

void Group::listMembers()
{
    const quint64 groupId = selectedGroupId();
    if (groupId == 0) {
        m_statusLabel->setText(QStringLiteral("请先在列表中选择一个群，再查看成员"));
        return;
    }
    sendRequest(ENUM_MSG_TYPE_LIST_GROUP_MEMBERS_REQUEST, groupId);
}

void Group::leaveGroup()
{
    const quint64 groupId = selectedGroupId();
    if (groupId == 0) {
        m_statusLabel->setText(QStringLiteral("请先在列表中选择一个群"));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("退出群聊"),
                              QStringLiteral("确定退出该群吗？")) != QMessageBox::Yes) return;
    sendRequest(ENUM_MSG_TYPE_LEAVE_GROUP_REQUEST, groupId);
}

void Group::removeMember()
{
    const quint64 groupId = selectedGroupId();
    if (groupId == 0) {
        m_statusLabel->setText(QStringLiteral("请先在列表中选择一个群"));
        return;
    }
    const QString target = m_targetEdit->text().trimmed();
    if (target.isEmpty()) {
        m_statusLabel->setText(QStringLiteral("请在「目标用户名」框输入要移除的成员名"));
        return;
    }
    // 移除仅对普通成员生效（群主/管理员需先被撤销角色），提前说明避免误操作
    if (QMessageBox::question(this, QStringLiteral("移除成员"),
                              QStringLiteral("确定把 %1 移出该群吗？\n（只能移除普通成员；管理员需群主先将其设回 member）").arg(target))
            != QMessageBox::Yes) return;
    QByteArray body;
    groupAppendString(body, target.toUtf8());
    sendRequest(ENUM_MSG_TYPE_REMOVE_GROUP_MEMBER_REQUEST, groupId, body);
}

void Group::setRole()
{
    const quint64 groupId = selectedGroupId();
    if (groupId == 0) {
        m_statusLabel->setText(QStringLiteral("请先在列表中选择一个群"));
        return;
    }
    const QString target = m_targetEdit->text().trimmed();
    if (target.isEmpty()) {
        m_statusLabel->setText(QStringLiteral("请在「目标用户名」框输入要设置角色的成员名"));
        return;
    }
    QByteArray body;
    groupAppendString(body, target.toUtf8());
    groupAppendString(body, m_roleBox->currentText().toUtf8());
    sendRequest(ENUM_MSG_TYPE_SET_GROUP_ROLE_REQUEST, groupId, body);
}

void Group::transferOwner()
{
    const quint64 groupId = selectedGroupId();
    if (groupId == 0) {
        m_statusLabel->setText(QStringLiteral("请先在列表中选择一个群"));
        return;
    }
    const QString target = m_targetEdit->text().trimmed();
    if (target.isEmpty()) {
        m_statusLabel->setText(QStringLiteral("请在「目标用户名」框输入新群主的用户名"));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("转让群主"),
                              QStringLiteral("确定把群主转给 %1 吗？你将变为管理员。").arg(target))
            != QMessageBox::Yes) return;
    QByteArray body;
    groupAppendString(body, target.toUtf8());
    sendRequest(ENUM_MSG_TYPE_TRANSFER_GROUP_OWNER_REQUEST, groupId, body);
}

void Group::dissolveGroup()
{
    const quint64 groupId = selectedGroupId();
    if (groupId == 0) {
        m_statusLabel->setText(QStringLiteral("请先在列表中选择一个群"));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("解散群"),
                              QStringLiteral("确定解散该群吗？此操作不可恢复！")) != QMessageBox::Yes) return;
    sendRequest(ENUM_MSG_TYPE_DISSOLVE_GROUP_REQUEST, groupId);
}

// 服务端错误码 → 可读中文（码段见 operatedb.cpp handleGroupOperation）
QString Group::groupErrorText(quint32 code) const
{
    switch (qint32(code)) {
    case -1001: return QStringLiteral("未登录或会话失效");
    case -1002: return QStringLiteral("数据库不可达，请联系服务端");
    case -1009: return QStringLiteral("请求缺少群号");
    case -1010: return QStringLiteral("邀请不存在");
    case -1011: case -1014: case -1017: return QStringLiteral("群不存在");
    case -1012: return QStringLiteral("你不是该群成员，无法查看");
    case -1018: return QStringLiteral("邀请不存在或已被处理");
    case -1019: return QStringLiteral("该邀请不属于你，或已被处理");
    case -1020: return QStringLiteral("邀请已过期或群已解散");
    case -1027: return QStringLiteral("群已解散，或你已不在群中");
    case -1028: return QStringLiteral("只有群主或管理员才能邀请成员");
    case -1029: return QStringLiteral("参数非法（用户名过长等）");
    case -1030: return QStringLiteral("目标用户不存在，请核对用户名");
    case -1031: case -1032: return QStringLiteral("对方已经是群成员");
    case -1034: return QStringLiteral("对方已有一条待处理邀请，等待其处理");
    case -1036: return QStringLiteral("群主不能退出群，请先转让群主或解散");
    case -1037: return QStringLiteral("你已不在该群中");
    case -1038: return QStringLiteral("只有群主或管理员才能移除成员");
    case -1039: return QStringLiteral("目标不是普通成员（管理员需群主先设回 member）");
    case -1041: return QStringLiteral("只有群主能设置角色");
    case -1042: return QStringLiteral("目标不是活跃成员或已是该角色");
    case -1043: return QStringLiteral("只有群主能转让群主");
    case -1044: return QStringLiteral("新群主必须是群里的普通成员");
    case -1046: return QStringLiteral("只有群主能解散群");
    case -1047: return QStringLiteral("解散失败（服务端错误）");
    case -2001: case -2002: return QStringLiteral("请求格式被服务端拒绝");
    default: return QStringLiteral("错误码 %1").arg(qint32(code));
    }
}

void Group::handleResponse(PDU *pdu)
{
    if (pdu == nullptr || pdu->uiMsgLen < GROUP_ENVELOPE_LENGTH + 4) return;
    const QByteArray payload(pdu->caMsg, int(pdu->uiMsgLen));
    int offset = 0;
    quint16 version = 0, length = 0;
    quint64 requestId = 0, groupId = 0;
    quint32 code = 0;
    // 84/85 是服务端主动事件：code 位置放的是群 version（低 32 位），绝不能进
    // 通用失败判定——此前 version=2 被误读为错误码，操作成功反而弹「群操作失败」。
    if (pdu->uiType == ENUM_MSG_TYPE_GROUP_MEMBER_EVENT
            || pdu->uiType == ENUM_MSG_TYPE_GROUP_INVITATION_EVENT) {
        if (!groupReadU16(payload, offset, version) || !groupReadU16(payload, offset, length)
                || !groupReadU64(payload, offset, requestId) || !groupReadU64(payload, offset, groupId)) return;
        if (pdu->uiType == ENUM_MSG_TYPE_GROUP_INVITATION_EVENT) {
            refreshInvitations();   // 有人邀请你 / 邀请被处理
            m_statusLabel->setText(QStringLiteral("收到新的群邀请，已刷新"));
        } else {
            refreshGroups();        // 成员变动（加入/退出/移除/转让/解散）
        }
        return;
    }
    if (!groupReadU16(payload, offset, version) || !groupReadU16(payload, offset, length)
            || !groupReadU64(payload, offset, requestId) || !groupReadU64(payload, offset, groupId)
            || !groupReadU32(payload, offset, code) || version != GROUP_PROTOCOL_VERSION
            || length != GROUP_ENVELOPE_LENGTH) return;
    Q_UNUSED(requestId);
    if (code != 0) {
        m_statusLabel->setText(QStringLiteral("群操作失败：%1").arg(groupErrorText(code)));
        return;
    }
    if (pdu->uiType == ENUM_MSG_TYPE_LIST_GROUPS_RESPOND) {
        quint32 count = 0;
        if (!groupReadU32(payload, offset, count)) return;
        m_groupList->clear();
        for (quint32 i = 0; i < count; ++i) {
            quint64 id = 0, groupVersion = 0;
            QByteArray name, status, role;
            if (!groupReadU64(payload, offset, id) || !groupReadString(payload, offset, name)
                    || !groupReadString(payload, offset, status) || !groupReadString(payload, offset, role)
                    || !groupReadU64(payload, offset, groupVersion)) return;
            auto *item = new QListWidgetItem(QStringLiteral("%1  [%2] %3 (%4)")
                                             .arg(QString::fromUtf8(name)).arg(QString::fromUtf8(role))
                                             .arg(QString::fromUtf8(status)).arg(id), m_groupList);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(id));
            item->setData(Qt::UserRole + 1, QString::fromUtf8(name));   // 群名 → 聊天窗口标题
            item->setData(Qt::UserRole + 2, item->text());              // 基础文案（徽标在其上追加）
            refreshUnreadBadge(id, m_unreadByGroup.value(id, 0));
        }
        m_statusLabel->setText(QStringLiteral("我的群聊：%1 个").arg(count));
    } else if (pdu->uiType == ENUM_MSG_TYPE_QUERY_GROUP_INVITATIONS_RESPOND) {
        quint32 count = 0;
        if (!groupReadU32(payload, offset, count)) return;
        m_invitationList->clear();
        for (quint32 i = 0; i < count; ++i) {
            quint64 invitationId = 0, invitationGroupId = 0, expiresAt = 0;
            QByteArray groupName, inviter, status;
            if (!groupReadU64(payload, offset, invitationId) || !groupReadU64(payload, offset, invitationGroupId)
                    || !groupReadString(payload, offset, groupName) || !groupReadString(payload, offset, inviter)
                    || !groupReadString(payload, offset, status) || !groupReadU64(payload, offset, expiresAt)) return;
            Q_UNUSED(invitationGroupId);
            Q_UNUSED(expiresAt);
            auto *item = new QListWidgetItem(QStringLiteral("%1 邀请人：%2 (%3)")
                                             .arg(QString::fromUtf8(groupName)).arg(QString::fromUtf8(inviter))
                                             .arg(QString::fromUtf8(status)), m_invitationList);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(invitationId));
        }
        m_statusLabel->setText(QStringLiteral("待处理邀请：%1 个").arg(count));
    } else if (pdu->uiType == ENUM_MSG_TYPE_LIST_GROUP_MEMBERS_RESPOND) {
        quint32 count = 0;
        if (!groupReadU32(payload, offset, count)) return;
        QStringList members;
        for (quint32 i = 0; i < count; ++i) {
            QByteArray user, role, status;
            if (!groupReadString(payload, offset, user) || !groupReadString(payload, offset, role)
                    || !groupReadString(payload, offset, status)) return;
            members << QStringLiteral("%1 [%2] %3").arg(QString::fromUtf8(user))
                       .arg(QString::fromUtf8(role)).arg(QString::fromUtf8(status));
        }
        QMessageBox::information(this, QStringLiteral("群成员"), members.join(QStringLiteral("\n")));
    } else if (pdu->uiType == ENUM_MSG_TYPE_CREATE_GROUP_RESPOND) {
        m_statusLabel->setText(QStringLiteral("建群成功"));
        refreshGroups();
    } else if (pdu->uiType == ENUM_MSG_TYPE_GROUP_UNREAD_SUMMARY_RESPOND) {
        // REQ-EXT-GROUP-002：96 = count + N×{group_id,last_seq,last_read_seq,unread}
        quint32 count = 0;
        if (!groupReadU32(payload, offset, count)) return;
        quint64 totalUnread = 0;
        for (quint32 i = 0; i < count; ++i) {
            quint64 summaryGroupId = 0, lastSeq = 0, lastReadSeq = 0, unread = 0;
            if (!groupReadU64(payload, offset, summaryGroupId) || !groupReadU64(payload, offset, lastSeq)
                    || !groupReadU64(payload, offset, lastReadSeq) || !groupReadU64(payload, offset, unread)) return;
            totalUnread += unread;
            m_unreadByGroup.insert(summaryGroupId, unread);
            refreshUnreadBadge(summaryGroupId, unread);
            // REQ-EXT-GROUP-004：锚点落进会话级状态（水位与游标都只增不减）。
            // 徽标仍用服务端回带的未读数（权威），窗口内的显示由状态推导，两者等价（见需求 §4.5）。
            Client::getinstance().groupState(summaryGroupId).applyServerAnchor(lastSeq, lastReadSeq);
            // 已打开窗口：让其按新锚点续补/上报
            if (m_chatWindows.contains(summaryGroupId)) {
                m_chatWindows.value(summaryGroupId)->onAnchorUpdated();
            }
        }
        m_statusLabel->setText(QStringLiteral("我的群聊：%1 个 · 未读合计 %2")
                               .arg(m_groupList->count()).arg(totalUnread));
    } else if (pdu->uiType == ENUM_MSG_TYPE_INVITE_GROUP_MEMBER_RESPOND) {
        // 此前成功时零反馈，主人以为邀请坏了；响应体带 invitation_id
        m_statusLabel->setText(QStringLiteral("邀请已发送，等待对方处理"));
    } else if (pdu->uiType == ENUM_MSG_TYPE_ACCEPT_GROUP_INVITATION_RESPOND) {
        m_statusLabel->setText(QStringLiteral("已接受邀请并加入群聊"));
        refreshGroups();
        refreshInvitations();
    } else if (pdu->uiType == ENUM_MSG_TYPE_REJECT_GROUP_INVITATION_RESPOND) {
        m_statusLabel->setText(QStringLiteral("已拒绝该邀请"));
        refreshInvitations();
    } else if (pdu->uiType == ENUM_MSG_TYPE_LEAVE_GROUP_RESPOND) {
        m_statusLabel->setText(QStringLiteral("已退出群聊"));
        refreshGroups();
    } else if (pdu->uiType == ENUM_MSG_TYPE_REMOVE_GROUP_MEMBER_RESPOND) {
        m_statusLabel->setText(QStringLiteral("已移除该成员"));
        refreshGroups();
    } else if (pdu->uiType == ENUM_MSG_TYPE_SET_GROUP_ROLE_RESPOND) {
        m_statusLabel->setText(QStringLiteral("角色已更新"));
        refreshGroups();
    } else if (pdu->uiType == ENUM_MSG_TYPE_TRANSFER_GROUP_OWNER_RESPOND) {
        // 此前实际已成功，但界面零反馈 + 84 事件误报失败，看起来像「转让失败」
        m_statusLabel->setText(QStringLiteral("群主已转让，你现在是管理员"));
        refreshGroups();
    } else if (pdu->uiType == ENUM_MSG_TYPE_DISSOLVE_GROUP_RESPOND) {
        // 同上：第一次解散实际成功但无反馈；再点一次才报「群已解散」错误
        m_statusLabel->setText(QStringLiteral("群已解散"));
        refreshGroups();
    }
}

// REQ-EXT-GROUP-002：群条目文案追加未读徽标（0 不显示）
void Group::refreshUnreadBadge(quint64 groupId, quint64 unread)
{
    for (int i = 0; i < m_groupList->count(); ++i) {
        QListWidgetItem *item = m_groupList->item(i);
        if (item->data(Qt::UserRole).toULongLong() != groupId) continue;
        QString text = item->data(Qt::UserRole + 2).toString();
        if (text.isEmpty()) text = item->text();
        if (unread > 0) text += QStringLiteral(" · 未读 %1").arg(unread);
        item->setText(text);
        return;
    }
}

void Group::openSelectedChat()
{
    const QListWidgetItem *item = m_groupList->currentItem();
    if (item == nullptr) return;
    const quint64 groupId = item->data(Qt::UserRole).toULongLong();
    if (groupId == 0) return;
    const QString groupName = item->data(Qt::UserRole + 1).toString();
    GroupChat *chat = m_chatWindows.value(groupId, nullptr);
    if (chat == nullptr) {
        chat = new GroupChat(groupId, groupName, nullptr);
        m_chatWindows.insert(groupId, chat);
    }
    chat->show();
    chat->raise();
    chat->activateWindow();
}

// 信封首 20B：版本(2)+长度(2)+request_id(8)+group_id(8)
quint64 Group::envelopeGroupId(PDU *pdu)
{
    if (pdu == nullptr || pdu->uiMsgLen < GROUP_ENVELOPE_LENGTH) return 0;
    const QByteArray payload(pdu->caMsg, GROUP_ENVELOPE_LENGTH);
    int offset = 0;
    quint16 version = 0, length = 0;
    quint64 requestId = 0, groupId = 0;
    if (!groupReadU16(payload, offset, version) || !groupReadU16(payload, offset, length)
            || !groupReadU64(payload, offset, requestId) || !groupReadU64(payload, offset, groupId)) return 0;
    Q_UNUSED(version);
    Q_UNUSED(length);
    Q_UNUSED(requestId);
    return groupId;
}

void Group::handleMessageResponse(PDU *pdu)
{
    const quint64 groupId = envelopeGroupId(pdu);
    if (groupId == 0) return;
    if (m_chatWindows.contains(groupId)) {
        m_chatWindows.value(groupId)->handleResponse(pdu);
        return;
    }
    m_statusLabel->setText(QStringLiteral("群 %1 的消息响应到达（窗口未打开）").arg(groupId));
}

void Group::handleMessageEvent(PDU *pdu)
{
    const quint64 groupId = envelopeGroupId(pdu);
    if (groupId == 0) return;
    GroupChat *chat = m_chatWindows.value(groupId, nullptr);
    if (chat != nullptr) {
        chat->handleMessageEvent(pdu);
        return;
    }
    // 未打开窗口：不渲染，只刷新未读徽标并提示（离线补偿拉取兜底）
    requestUnreadSummary();
    m_statusLabel->setText(QStringLiteral("群 %1 有新消息").arg(groupId));
}

// REQ-EXT-GROUP-003：打开选中群的群文件目录窗口
void Group::openSelectedFiles()
{
    const QListWidgetItem *item = m_groupList->currentItem();
    if (item == nullptr) {
        m_statusLabel->setText(QStringLiteral("请先选择一个群"));
        return;
    }
    const quint64 groupId = item->data(Qt::UserRole).toULongLong();
    if (groupId == 0) return;
    const QString groupName = item->data(Qt::UserRole + 1).toString();
    GroupFile *win = m_fileWindows.value(groupId, nullptr);
    if (win == nullptr) {
        win = new GroupFile(groupId, groupName, nullptr);
        m_fileWindows.insert(groupId, win);
    }
    win->show();
    win->raise();
    win->activateWindow();
}

// REQ-EXT-GROUP-003：100/102/104 按信封 group_id 路由到群文件窗口
void Group::handleFileResponse(PDU *pdu)
{
    const quint64 groupId = envelopeGroupId(pdu);
    if (groupId == 0) return;
    GroupFile *win = m_fileWindows.value(groupId, nullptr);
    if (win != nullptr) {
        win->handleResponse(pdu);
        return;
    }
    m_statusLabel->setText(QStringLiteral("群 %1 的文件响应到达（窗口未打开）").arg(groupId));
}
