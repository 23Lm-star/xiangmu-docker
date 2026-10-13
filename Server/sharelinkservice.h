#ifndef SHARELINKSERVICE_H
#define SHARELINKSERVICE_H

#include <QString>
#include <QDateTime>
#include <QRandomGenerator>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>

class ShareLinkService {
public:
    static QString generateToken();        // 16 随机字节 -> URL-safe Base64
    static QString hashToken(const QString &token); // SHA-256 -> hex
    static bool validatePath(const QString &ownerRoot, const QString &relativePath); // canonical 校验
    static QDateTime defaultExpireTime();  // 当前时间 + 24h
};

#endif // SHARELINKSERVICE_H
