#include "protocol.h"
#include "uploader.h"

#include <QTimer>
#include <cstring>

Uploader::Uploader(quint64 transferId, const QString &strUploadPath, qint64 resumeOffset, QObject *parent)
    : QObject(parent),
      strUploadPath(strUploadPath),
      m_resumeOffset(resumeOffset),
      m_transferId(transferId),
      m_nextOffset(0),
      m_waitingAck(false),
      m_active(false),
      m_finished(false)
{
}

void Uploader::start()
{
    if (m_active) {
        return;
    }

    m_file.setFileName(strUploadPath);
    if (!m_file.open(QIODevice::ReadOnly)) {
        emit errnoMsg("Upload file open failed");
        finish(false);
        return;
    }

    m_nextOffset = qMax<qint64>(0, m_resumeOffset);
    // C2：chunk = clamp(ceil(size / 1024), 64 KiB, 1 MiB)；size==0 取 MIN（由 C3 兜底不发送）
    const qint64 totalSize = m_file.size();
    m_chunkSize = qBound<qint64>(UPLOAD_CHUNK_MIN,
                                 (totalSize + UPLOAD_CHUNK_TARGET_COUNT - 1) / UPLOAD_CHUNK_TARGET_COUNT,
                                 UPLOAD_CHUNK_MAX);
    if (m_nextOffset > m_file.size() || !m_file.seek(m_nextOffset)) {
        emit errnoMsg("Upload file seek failed");
        finish(false);
        return;
    }

    m_active = true;
    sendNextChunk();
}

void Uploader::acknowledge(bool success, qint64 nextOffset, bool complete)
{
    if (!m_active || !m_waitingAck) {
        return;
    }

    m_waitingAck = false;
    if (!success || nextOffset < m_nextOffset) {
        emit errnoMsg("Upload chunk rejected");
        finish(false);
        return;
    }

    m_nextOffset = nextOffset;
    if (complete) {
        finish(true);
        return;
    }

    QTimer::singleShot(0, this, &Uploader::sendNextChunk);
}

void Uploader::sendNextChunk()
{
    if (!m_active || m_waitingAck) {
        return;
    }

    const QByteArray chunk = m_file.read(m_chunkSize);
    if (chunk.isEmpty()) {
        emit errnoMsg("Upload file ended before server confirmation");
        finish(false);
        return;
    }

    PDU *pdu = mkPDU(chunk.size());
    pdu->uiType = ENUM_MSG_TYPE_UPLOADFILE_DATA_REQUEST;
    const uint resumeMagic = UPLOAD_RESUME_MAGIC;
    memcpy(pdu->caData + UPLOAD_CHUNK_OFFSET, &m_nextOffset, sizeof(m_nextOffset));
    memcpy(pdu->caData + UPLOAD_RESUME_MAGIC_OFFSET, &resumeMagic, sizeof(resumeMagic));
    memcpy(pdu->caData + TRANSFER_ID_OFFSET, &m_transferId, sizeof(m_transferId));   // REQ-EXT-TRANSFER-001
    memcpy(pdu->caMsg, chunk.constData(), chunk.size());
    m_waitingAck = true;
    emit seedMsg(pdu);
}

void Uploader::finish(bool success)
{
    if (m_finished) {
        return;
    }

    m_finished = true;
    m_active = false;
    m_waitingAck = false;
    if (m_file.isOpen()) {
        m_file.close();
    }
    emit finished(success);
}
