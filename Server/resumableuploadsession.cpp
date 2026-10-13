#include "resumableuploadsession.h"
#include "server.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

ResumableUploadSession::ResumableUploadSession()
    : m_receivedSize(0),
      m_totalSize(0)
{
}

ResumableUploadSession::~ResumableUploadSession()
{
    reset();
}

ResumableUploadSession::BeginResult ResumableUploadSession::begin(const QString &dirPath,
                                                                  const QString &fileName,
                                                                  qint64 totalSize,
                                                                  bool resumeEnabled)
{
    reset();

    BeginResult result = {false, 0, false};
    if (dirPath.isEmpty() || fileName.isEmpty() || fileName.contains('/')
            || fileName.contains('\\') || totalSize < 0) {
        return result;
    }

    QDir dir(dirPath);
    if (!dir.exists()) {
        return result;
    }

    m_filePath = dir.filePath(fileName);
    QFileInfo fileInfo(m_filePath);
    qint64 offset = 0;
    if (resumeEnabled && fileInfo.exists() && fileInfo.isFile()) {
        const qint64 existingSize = fileInfo.size();
        if (existingSize > 0 && existingSize < totalSize) {
            offset = existingSize;
        } else if (existingSize == totalSize) {
            m_receivedSize = existingSize;
            m_totalSize = totalSize;
            return {true, existingSize, true};
        }
    }

    QFile file(m_filePath);
    QIODevice::OpenMode mode = QIODevice::WriteOnly;
    mode |= ((resumeEnabled && offset > 0) ? QIODevice::Append : QIODevice::Truncate);
    if (!file.open(mode)) {
        reset();
        return result;
    }
    file.close();

    m_receivedSize = offset;
    m_totalSize = totalSize;
    result.ok = true;
    result.offset = offset;
    result.complete = isComplete();
    return result;
}

// REQ-EXT-FILE-001 T4：暂存路径 = filesys/.staging/<sha1(owner|parent|display|size)>.part
// 同参数重连命中同一 staging（断点续传）；完成前不进最终目录（§4）。
ResumableUploadSession::BeginResult ResumableUploadSession::beginStaged(
        const QString &owner, const QString &parentPath, const QString &displayName,
        qint64 totalSize, bool resumeEnabled)
{
    reset();

    BeginResult result = {false, 0, false};
    if (owner.isEmpty() || parentPath.isEmpty() || displayName.isEmpty()
            || displayName.contains('/') || displayName.contains('\\')
            || totalSize < 0) {
        return result;
    }

    const QByteArray key = (owner + "|" + parentPath + "|" + displayName + "|" + QString::number(totalSize))
                               .toUtf8();
    const QString token = QString::fromLatin1(
        QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex());

    QDir stageRoot(QDir(Server::getinstance().filesPath).absoluteFilePath(".staging"));
    if (!stageRoot.exists() && !stageRoot.mkpath(".")) {
        return result;
    }
    m_filePath = stageRoot.filePath(token + ".part");
    m_owner = owner;
    m_parent = parentPath;
    m_display = displayName;

    // REQ-EXT-TRANSFER-003 S1（方案 B）：暂存句柄打开后**不移交、不关闭**，挂在会话上共享所有权。
    // 写任务只 seek+write+flush（S3）；无需 Append——每块显式 seek 到 chunkOffset。

    QFileInfo fileInfo(m_filePath);
    qint64 offset = 0;
    if (resumeEnabled && fileInfo.exists() && fileInfo.isFile()) {
        const qint64 existingSize = fileInfo.size();
        if (existingSize > 0 && existingSize < totalSize) {
            offset = existingSize;
        } else if (existingSize == totalSize) {
            m_receivedSize = existingSize;
            m_totalSize = totalSize;
            return {true, existingSize, true};
        }
    }

    QSharedPointer<QFile> file(new QFile(m_filePath));
    QIODevice::OpenMode mode = QIODevice::ReadWrite;
    if (offset == 0) {
        mode |= QIODevice::Truncate;   // 重头（含 resumeEnabled=false）：清掉既有残留
    }
    if (!file->open(mode)) {
        reset();
        return result;
    }
    m_stagingFile = file;

    m_receivedSize = offset;
    m_totalSize = totalSize;
    result.ok = true;
    result.offset = offset;
    result.complete = isComplete();
    return result;
}

bool ResumableUploadSession::acceptWrittenChunk(qint64 offset, qint64 len)
{
    if (offset != m_receivedSize || len <= 0 || len > m_totalSize - m_receivedSize) {
        return false;
    }
    m_receivedSize += len;
    return true;
}

bool ResumableUploadSession::isComplete() const
{
    return m_totalSize >= 0 && m_receivedSize >= m_totalSize;
}

qint64 ResumableUploadSession::receivedSize() const
{
    return m_receivedSize;
}

qint64 ResumableUploadSession::totalSize() const
{
    return m_totalSize;
}

QString ResumableUploadSession::stagingFilePath() const
{
    return m_filePath;
}

void ResumableUploadSession::reset()
{
    // REQ-EXT-TRANSFER-003 S2：会话重置即释放共享句柄（最后一个引用释放才会真正关闭）
    m_stagingFile.clear();
    m_filePath.clear();
    m_receivedSize = 0;
    m_totalSize = 0;
    m_owner.clear();
    m_parent.clear();
    m_display.clear();
}
