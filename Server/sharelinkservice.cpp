#include "sharelinkservice.h"
#include <QByteArray>

QString ShareLinkService::generateToken() {
    QByteArray randomData(16, 0);
    for (int i = 0; i < 16; ++i) {
        randomData[i] = static_cast<char>(QRandomGenerator::system()->generate() % 256);
    }
    // URL-safe Base64: + -> -, / -> _, remove =
    return randomData.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

QString ShareLinkService::hashToken(const QString &token) {
    return QCryptographicHash::hash(token.toUtf8(), QCryptographicHash::Sha256).toHex();
}

bool ShareLinkService::validatePath(const QString &ownerRoot, const QString &relativePath) {
    QDir rootDir(ownerRoot);
    QString absolutePath = rootDir.absoluteFilePath(relativePath);
    QFileInfo fileInfo(absolutePath);
    
    // 检查文件是否存在
    if (!fileInfo.exists()) return false;
    
    // 检查是否在根目录下（防止路径穿越）
    return absolutePath.startsWith(rootDir.canonicalPath());
}

QDateTime ShareLinkService::defaultExpireTime() {
    return QDateTime::currentDateTime().addSecs(24 * 3600);
}
