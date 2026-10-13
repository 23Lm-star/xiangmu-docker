#ifndef GROUPFILE_H
#define GROUPFILE_H

#include "protocol.h"
#include <QHash>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QWidget>

// REQ-EXT-GROUP-003：单群文件目录窗口。
// 目录经 99/100 分页列出（名称/大小/上传者/时间，无物理路径）；
// 上传先发 101 做秒传预检查：命中零正文，未命中回 session_id/nextOffset 走既有 31 分块；
// 下载发 105 取 106 元数据后走既有 42/43 分块；删除发 103。
// 权限一律由服务端判定（本窗口只做展示与置灰，不缓存授权）。
class GroupFile : public QWidget
{
    Q_OBJECT
public:
    GroupFile(quint64 groupId, const QString &groupName, QWidget *parent = nullptr);
    quint64 groupId() const { return m_groupId; }
    QString groupName() const { return m_groupName; }

    // 100/104 响应路由（按信封 group_id 匹配）
    // REQ-EXT-TRANSFER-001：102/106 已改由 TransferManager 按 transfer_id 路由
    void handleResponse(PDU *pdu);

private slots:
    void refresh();
    void uploadFile();
    void addPendingUploads();       // 购物车上传：分多次加入待传清单
    void downloadSelected();
    void deleteSelected();
    void onItemDoubleClicked(QListWidgetItem *item);

private:
    void sendRequest(uint type, const QByteArray &body = QByteArray());
    QByteArray envelope(const QByteArray &body);
    void setStatus(const QString &text);
    quint64 selectedGroupFileId() const;

    quint64 m_groupId;
    QString m_groupName;
    quint64 m_requestId = 0;
    QListWidget *m_fileList;
    QPushButton *m_refreshButton;
    QPushButton *m_uploadButton;
    QPushButton *m_downloadButton;
    QPushButton *m_deleteButton;
    QPushButton *m_addQueueButton = nullptr;    // 添加待传（购物车上传入口）
    QPushButton *m_tasksButton = nullptr;       // 传输列表入口（打开全局传输队列面板）
    QStringList m_pendingUploads;               // 待传清单（本地路径）
    QLabel *m_statusLabel;
};

#endif // GROUPFILE_H
