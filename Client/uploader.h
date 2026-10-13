#ifndef UPLOADER_H
#define UPLOADER_H

#include "protocol.h"

#include <QObject>
#include <QFile>

// REQ-EXT-TRANSFER-003 C1：上传分片策略——按固定块数（1024）生成，夹在 [64 KiB, 1 MiB]。
// 上限**必须 ≤ 服务端 MAX_UPLOAD_DATA_CHUNK**（Server/protocol.h；跨端一致性由 verify.py S-02 静态校验）。
static constexpr qint64 UPLOAD_CHUNK_TARGET_COUNT = 1024;
static constexpr qint64 UPLOAD_CHUNK_MIN = 64 * 1024;
static constexpr qint64 UPLOAD_CHUNK_MAX = 1024 * 1024;

class Uploader : public QObject
{
    Q_OBJECT
public:
    QString strUploadPath;
    qint64 m_resumeOffset;

    Uploader(quint64 transferId, const QString& strUploadPath, qint64 resumeOffset=0, QObject* parent=nullptr);
    void start();
    quint64 transferId() const { return m_transferId; }
public slots:
    void acknowledge(bool success, qint64 nextOffset, bool complete);
signals:
    void seedMsg(PDU* pdu);
    void errnoMsg(const QString& msg);
    void finished(bool success);

private:
    void sendNextChunk();
    void finish(bool success);

    quint64 m_transferId;
    QFile m_file;
    qint64 m_chunkSize = UPLOAD_CHUNK_MAX;   // C1：start() 按文件大小算定
    qint64 m_nextOffset;
    bool m_waitingAck;
    bool m_active;
    bool m_finished;
};

#endif // UPLOADER_H
