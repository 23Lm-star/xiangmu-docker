#include "groupfile.h"

#include "client.h"
#include "transfertask.h"

#include <QDateTime>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QVBoxLayout>
#include <cstring>

namespace {
QString humanSize(quint64 bytes)
{
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = double(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return u == 0 ? QStringLiteral("%1 B").arg(bytes)
                  : QStringLiteral("%1 %2").arg(v, 0, 'f', 1).arg(QLatin1String(units[u]));
}

QString humanTime(quint64 ms)
{
    if (ms == 0) return QStringLiteral("-");
    return QDateTime::fromMSecsSinceEpoch(qint64(ms)).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}
}

GroupFile::GroupFile(quint64 groupId, const QString &groupName, QWidget *parent)
    : QWidget(parent),
      m_groupId(groupId),
      m_groupName(groupName),
      m_fileList(new QListWidget(this)),
      m_refreshButton(new QPushButton(QStringLiteral("刷新"), this)),
      m_uploadButton(new QPushButton(QStringLiteral("上传"), this)),
      m_downloadButton(new QPushButton(QStringLiteral("下载"), this)),
      m_deleteButton(new QPushButton(QStringLiteral("删除"), this)),
      m_tasksButton(new QPushButton(QStringLiteral("传输列表"), this)),
      m_statusLabel(new QLabel(this))
{
    setAttribute(Qt::WA_StyledBackground, true);   // QSS 背景在 QWidget 子类上必须显式开启
    setWindowTitle(QStringLiteral("群文件 - %1").arg(groupName));
    if (parent == nullptr) resize(1240, 840);   // REQ-UX-001 A5：面积 4 倍

    auto *layout = new QVBoxLayout(this);
    auto *toolbar = new QHBoxLayout;
    toolbar->addWidget(m_refreshButton);
    toolbar->addWidget(m_uploadButton);
    toolbar->addWidget(m_downloadButton);
    toolbar->addWidget(m_deleteButton);
    toolbar->addWidget(m_tasksButton);
    // 购物车上传入口：分多次添加文件进待传清单，「上传」统一入队（主人要求的批量模式）
    m_addQueueButton = new QPushButton(QStringLiteral("添加待传"), this);
    m_addQueueButton->setToolTip(QStringLiteral("把文件加入待传清单（可分多次添加），再点「上传」一起进入上传队列"));
    toolbar->addWidget(m_addQueueButton);
    layout->addLayout(toolbar);
    layout->addWidget(m_statusLabel);
    layout->addWidget(m_fileList);
    m_fileList->setSelectionMode(QAbstractItemView::ExtendedSelection);   // 下载支持多选

    connect(m_refreshButton, &QPushButton::clicked, this, &GroupFile::refresh);
    connect(m_uploadButton, &QPushButton::clicked, this, &GroupFile::uploadFile);
    connect(m_addQueueButton, &QPushButton::clicked, this, &GroupFile::addPendingUploads);
    connect(m_downloadButton, &QPushButton::clicked, this, &GroupFile::downloadSelected);
    connect(m_deleteButton, &QPushButton::clicked, this, &GroupFile::deleteSelected);
    connect(m_tasksButton, &QPushButton::clicked, this, [this]() {
        TransferManager::instance().showPanel();   // 传输队列全局共享（个人+群文件同一队列）
    });
    connect(m_fileList, &QListWidget::itemDoubleClicked, this, &GroupFile::onItemDoubleClicked);
    setStatus(QStringLiteral("双击文件可直接下载；下载支持 Ctrl/Shift 多选"));
}

QByteArray GroupFile::envelope(const QByteArray &body)
{
    QByteArray payload;
    groupAppendU16(payload, GROUP_PROTOCOL_VERSION);
    groupAppendU16(payload, GROUP_ENVELOPE_LENGTH);
    groupAppendU64(payload, ++m_requestId);
    groupAppendU64(payload, m_groupId);
    payload.append(body);
    return payload;
}

void GroupFile::sendRequest(uint type, const QByteArray &body)
{
    const QByteArray payload = envelope(body);
    PDU *pdu = mkPDU(uint(payload.size()));
    pdu->uiType = type;
    memcpy(pdu->caMsg, payload.constData(), size_t(payload.size()));
    Client::getinstance().seedMsg(pdu);
}

void GroupFile::setStatus(const QString &text)
{
    m_statusLabel->setText(text);
}

quint64 GroupFile::selectedGroupFileId() const
{
    const QListWidgetItem *item = m_fileList->currentItem();
    return item == nullptr ? 0 : item->data(Qt::UserRole).toULongLong();
}

void GroupFile::refresh()
{
    QByteArray body;
    groupAppendU32(body, 0);                            // offset
    groupAppendU16(body, quint16(GROUP_FILE_MAX_LIST_LIMIT));  // limit
    sendRequest(ENUM_MSG_TYPE_GROUP_FILE_LIST_REQUEST, body);
}

// REQ-EXT-TRANSFER-001 §5.5/§5.6：多选上传；SHA-256 移出 UI 线程（M5），完成后交任务列表排队
// 购物车模式：待传清单非空时「上传」把清单统一入队；为空时走原多选对话框（一次可勾多个）。
void GroupFile::uploadFile()
{
    if (!m_pendingUploads.isEmpty()) {
        int enqueued = 0;
        for (const QString &path : m_pendingUploads) {
            const QString name = QFileInfo(path).fileName();
            if (name.isEmpty() || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
                    || name.toUtf8().size() > 255) {
                QMessageBox::information(this, QStringLiteral("提示"),
                                         QStringLiteral("文件名不合法：%1").arg(name));
                continue;
            }
            const quint64 rowKey = TransferManager::instance().enqueueGroupUpload(
                        m_groupId, path, name, QByteArray());   // sha 由任务在后台计算
            TransferManager::instance().showPanel();
            Q_UNUSED(rowKey);
            ++enqueued;
        }
        m_pendingUploads.clear();
        if (m_addQueueButton != nullptr) {
            m_addQueueButton->setText(QStringLiteral("添加待传"));
        }
        setStatus(QStringLiteral("已加入传输任务 %1 个").arg(enqueued));
        return;
    }
    const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("选择要上传的文件"));
    if (paths.isEmpty()) return;
    int enqueued = 0;
    for (const QString &path : paths) {
        const QString name = QFileInfo(path).fileName();
        if (name.isEmpty() || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\'))
                || name.toUtf8().size() > 255) {
            QMessageBox::information(this, QStringLiteral("提示"),
                                     QStringLiteral("文件名不合法：%1").arg(name));
            continue;
        }
        const quint64 rowKey = TransferManager::instance().enqueueGroupUpload(
                    m_groupId, path, name, QByteArray());   // sha 由任务在后台计算
        TransferManager::instance().showPanel();
        Q_UNUSED(rowKey);
        ++enqueued;
    }
    if (enqueued > 0) {
        setStatus(QStringLiteral("已加入传输任务 %1 个").arg(enqueued));
    }
}

// 把选中的文件加入待传清单（不开始传输）；可连续多次操作，攒够后点「上传」统一入队
void GroupFile::addPendingUploads()
{
    const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("选择要加入待传清单的文件（可多选）"));
    if (paths.isEmpty()) return;
    int added = 0;
    for (const QString &path : paths) {
        if (!m_pendingUploads.contains(path)) {
            m_pendingUploads.append(path);
            ++added;
        }
    }
    if (m_addQueueButton != nullptr) {
        m_addQueueButton->setText(QStringLiteral("添加待传(%1)").arg(m_pendingUploads.size()));
    }
    QMessageBox::information(this, QStringLiteral("待传清单"),
        QStringLiteral("本次新增 %1 个，清单共 %2 个。\n点「上传」统一进入上传队列。").arg(added).arg(m_pendingUploads.size()));
}

void GroupFile::downloadSelected()
{
    // 多选下载：遍历全部选中条目（Ctrl/Shift 多选），逐个入队（此前仅支持单选）
    const QList<QListWidgetItem*> selected = m_fileList->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请选择要下载的文件"));
        return;
    }
    int enqueued = 0;
    for (QListWidgetItem *item : selected) {
        if (item == nullptr) continue;
        const quint64 groupFileId = item->data(Qt::UserRole).toULongLong();
        if (groupFileId == 0) continue;
        const QString display = item->data(Qt::UserRole + 1).toString();
        TransferManager::instance().enqueueGroupDownload(m_groupId, groupFileId,
                    display.isEmpty() ? QStringLiteral("download.bin") : display);
        ++enqueued;
    }
    if (enqueued == 0) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("未选中可下载的文件"));
        return;
    }
    setStatus(QStringLiteral("已加入下载任务 %1 个").arg(enqueued));
    TransferManager::instance().showPanel();
}

void GroupFile::deleteSelected()
{
    const quint64 groupFileId = selectedGroupFileId();
    const QListWidgetItem *item = m_fileList->currentItem();
    if (groupFileId == 0 || item == nullptr) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请选择要删除的文件"));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("删除群文件"),
                              QStringLiteral("确定从本群目录删除「%1」？").arg(item->text()))
            != QMessageBox::Yes) {
        return;
    }
    QByteArray body;
    groupAppendU64(body, groupFileId);
    sendRequest(ENUM_MSG_TYPE_GROUP_FILE_DELETE_REQUEST, body);
}

void GroupFile::onItemDoubleClicked(QListWidgetItem *)
{
    downloadSelected();
}

void GroupFile::handleResponse(PDU *pdu)
{
    if (pdu == nullptr || pdu->uiMsgLen < GROUP_ENVELOPE_LENGTH + 4) return;
    const QByteArray payload(pdu->caMsg, int(pdu->uiMsgLen));
    int offset = 0;
    quint16 version = 0, length = 0;
    quint64 requestId = 0, groupId = 0;
    quint32 code = 0;
    if (!groupReadU16(payload, offset, version) || !groupReadU16(payload, offset, length)
            || !groupReadU64(payload, offset, requestId) || !groupReadU64(payload, offset, groupId)
            || !groupReadU32(payload, offset, code)
            || version != GROUP_PROTOCOL_VERSION || length != GROUP_ENVELOPE_LENGTH) return;
    Q_UNUSED(requestId);
    if (groupId != m_groupId) return;

    if (code != 0) {
        setStatus(QStringLiteral("群文件操作失败：%1").arg(qint32(code)));
        return;
    }

    if (pdu->uiType == ENUM_MSG_TYPE_GROUP_FILE_LIST_RESPOND) {
        quint32 count = 0, total = 0;
        if (!groupReadU32(payload, offset, count) || !groupReadU32(payload, offset, total)) return;
        m_fileList->clear();
        for (quint32 i = 0; i < count; ++i) {
            quint64 groupFileId = 0, sizeBytes = 0, createdAtMs = 0;
            QByteArray uploader, name;
            if (!groupReadU64(payload, offset, groupFileId)
                    || !groupReadU64(payload, offset, sizeBytes)
                    || !groupReadU64(payload, offset, createdAtMs)
                    || !groupReadString(payload, offset, uploader)
                    || !groupReadString(payload, offset, name)) return;
            const QString display = QString::fromUtf8(name);
            auto *item = new QListWidgetItem(QStringLiteral("%1    %2    %3    %4")
                                             .arg(display)
                                             .arg(humanSize(sizeBytes))
                                             .arg(QString::fromUtf8(uploader))
                                             .arg(humanTime(createdAtMs)), m_fileList);
            item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(groupFileId));
            item->setData(Qt::UserRole + 1, display);
        }
        setStatus(QStringLiteral("群文件：共 %1 项（本页 %2）").arg(total).arg(count));
    } else if (pdu->uiType == ENUM_MSG_TYPE_GROUP_FILE_DELETE_RESPOND) {
        setStatus(QStringLiteral("已删除，正在刷新"));
        refresh();
    }
    // REQ-EXT-TRANSFER-001：102 上传初始化响应改由 TransferManager 按 id 路由（本窗口不再处理），
    // 106 下载元数据同理；31/32 与 42/43 的分块收发均由 TransferManager 承载。
}
