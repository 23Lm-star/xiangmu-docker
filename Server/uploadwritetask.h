#ifndef UPLOADWRITETASK_H
#define UPLOADWRITETASK_H

#include <QRunnable>
#include <QByteArray>
#include <QString>
#include <QSharedPointer>

class QFile;

// REQ-EXT-TRANSFER-003 S3：写任务持暂存句柄的共享所有权（不 open/不 close），
// 只 seek+write+flush；ctx 先销毁也不会让在途写落在已关闭的句柄上。
class UploadWriteTask : public QRunnable
{
public:
    UploadWriteTask(quint64 connectionId, quint64 transferId, const QSharedPointer<QFile> &file,
                    qint64 offset, const QByteArray &data);
    void run() override;

private:
    quint64 m_connectionId;
    quint64 m_transferId;
    QSharedPointer<QFile> m_file;
    qint64 m_offset;
    QByteArray m_data;
};

#endif // UPLOADWRITETASK_H
