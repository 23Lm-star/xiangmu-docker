#ifndef RESUMABLEUPLOADSESSION_H
#define RESUMABLEUPLOADSESSION_H

#include <QString>
#include <QSharedPointer>

class QFile;

class ResumableUploadSession
{
public:
    struct BeginResult
    {
        bool ok;
        qint64 offset;
        bool complete;
    };

    ResumableUploadSession();
    ~ResumableUploadSession();

    BeginResult begin(const QString &dirPath,
                      const QString &fileName,
                      qint64 totalSize,
                      bool resumeEnabled);

    // REQ-EXT-FILE-001 T4：内容寻址上传暂存。staging 按 (owner|parent|display|size)
    // 派生确定性路径，重连/续传命中同一 staging；完成前绝不落最终目录（§4）。
    BeginResult beginStaged(const QString &owner,
                            const QString &parentPath,
                            const QString &displayName,
                            qint64 totalSize,
                            bool resumeEnabled);
    bool acceptWrittenChunk(qint64 offset, qint64 len);
    bool isComplete() const;
    qint64 receivedSize() const;
    qint64 totalSize() const;
    QString stagingFilePath() const;
    // REQ-EXT-TRANSFER-003 S1/S2：暂存句柄共享所有权——beginStaged 打开后不关闭，
    // 写任务持引用做 seek+write+flush；最后一个引用释放即关闭。
    QSharedPointer<QFile> stagingFile() const { return m_stagingFile; }
    void releaseStagingFile() { m_stagingFile.clear(); }
    void reset();

    // 逻辑条目定位（commitUpload 用）
    QString owner() const { return m_owner; }
    QString parentPath() const { return m_parent; }
    QString displayName() const { return m_display; }

private:
    QString m_filePath;
    qint64 m_receivedSize;
    qint64 m_totalSize;
    QSharedPointer<QFile> m_stagingFile;   // REQ-EXT-TRANSFER-003：暂存打开句柄（仅 beginStaged 持有）
    // T4 暂存归属（仅 beginStaged 写入）
    QString m_owner;
    QString m_parent;
    QString m_display;
};

#endif // RESUMABLEUPLOADSESSION_H
