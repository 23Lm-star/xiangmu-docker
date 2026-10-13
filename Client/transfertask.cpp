#include "transfertask.h"

#include "client.h"
#include "file.h"
#include "index.h"
#include "logging.h"
#include "uploader.h"

#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QCryptographicHash>
#include <QFutureWatcher>
#include <QPushButton>
#include <QStandardPaths>
#include <QTableWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <cstring>

// ===================== TransferManager =====================

TransferManager& TransferManager::instance()
{
    static TransferManager mgr;
    return mgr;
}

TransferManager::TransferManager()
{
}

quint64 TransferManager::allocId()
{
    return m_nextId++;
}

// ===================== 会话间持久化（transfers_<user>.json） =====================
// 设计：快照只在状态跃迁点写入（入队/完成/暂停/恢复/取消/重试），进度字节不落盘——
// 续传点由协议决定（个人上传 97/30 回带 offset；下载续传点 = 本地分片物理大小，
// REQ-EXT-TRANSFER-002 D9），落盘进度只会误导。

QString TransferManager::snapshotPath(const QString& user) const
{
    QString safe;
    for (const QChar &ch : user) {
        const ushort u = ch.unicode();
        safe += ((u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9')
                 || u >= 0x4E00) ? ch : QChar('_');   // 中文/字母数字保留，其余转 _（防路径注入）
    }
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(base).filePath(QStringLiteral("transfers_%1.json").arg(safe));
}

void TransferManager::saveSnapshot()
{
    // D-a：没有当前用户也不再静默丢写——以"最近一次有效登录名"兜底；两者皆无则明确告警。
    const QString user = m_currentUser.isEmpty() ? m_lastKnownUser : m_currentUser;
    if (user.isEmpty()) {
        qCWarning(lcCli) << "[transfer] 快照跳过：既无当前用户也无历史登录名，本轮任务不落盘";
        return;
    }
    if (m_currentUser.isEmpty()) {
        qCWarning(lcCli) << "[transfer] 当前用户名为空，快照按最近一次登录名兜底写入 user=" << user;
    }
    QJsonArray arr;
    for (auto it = m_tasks.constBegin(); it != m_tasks.constEnd(); ++it) {
        const Task &t = it.value();
        if (t.state == Done || t.state == Cancelled) continue;   // 终态不入快照
        QJsonObject o;
        o[QStringLiteral("kind")] = int(t.kind);
        o[QStringLiteral("localPath")] = t.localPath;
        o[QStringLiteral("displayName")] = t.displayName;
        o[QStringLiteral("dirPath")] = t.dirPath;
        o[QStringLiteral("serverPath")] = t.serverPath;
        o[QStringLiteral("groupId")] = double(t.groupId);
        o[QStringLiteral("groupFileId")] = double(t.groupFileId);
        o[QStringLiteral("totalBytes")] = double(t.totalBytes);
        if (!t.isUpload()) {
            // REQ-EXT-TRANSFER-002 C9：下载分片路径入快照；进度不落盘（恢复时由分片大小推导，D9）
            o[QStringLiteral("savePath")] = t.savePath;
        }
        arr.append(o);
    }
    const QString path = snapshotPath(user);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qCWarning(lcCli) << "[transfer] 快照写入失败（无法打开）path=" << path;
        return;
    }
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

// D-b：任务身份（快照合并用；不含 key/id/进度/状态）——同身份视为同一条任务，内存为准
static bool sameTaskIdentity(const TransferManager::Task& a, const TransferManager::Task& b)
{
    if (a.kind != b.kind) return false;
    if (a.kind == TransferManager::PersonalUpload) {
        return a.localPath == b.localPath && a.displayName == b.displayName
                && a.dirPath == b.dirPath;
    }
    if (a.kind == TransferManager::GroupUpload) {
        return a.localPath == b.localPath && a.displayName == b.displayName
                && a.groupId == b.groupId;
    }
    if (a.kind == TransferManager::PersonalDownload) return a.serverPath == b.serverPath;
    return a.groupId == b.groupId && a.groupFileId == b.groupFileId;
}

// 内存里是否已有同身份任务（有则快照同名条目不再补入）
static bool containsIdentity(const QHash<quint64, TransferManager::Task>& tasks,
                             const TransferManager::Task& probe)
{
    for (auto it = tasks.constBegin(); it != tasks.constEnd(); ++it) {
        if (sameTaskIdentity(it.value(), probe)) return true;
    }
    return false;
}

void TransferManager::restoreSavedTasks()
{
    if (m_currentUser.isEmpty()) return;
    if (m_restoredUser == m_currentUser) return;   // 断线重连不重复恢复（内存为准）
    m_restoredUser = m_currentUser;
    m_restoredCount = 0;
    QFile f(snapshotPath(m_currentUser));
    if (!f.open(QIODevice::ReadOnly)) return;
    const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).array();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        const Kind kind = Kind(o[QStringLiteral("kind")].toInt());
        Task t;
        t.kind = kind;
        t.localPath = o[QStringLiteral("localPath")].toString();
        t.displayName = o[QStringLiteral("displayName")].toString();
        t.dirPath = o[QStringLiteral("dirPath")].toString();
        t.serverPath = o[QStringLiteral("serverPath")].toString();
        t.groupId = quint64(o[QStringLiteral("groupId")].toDouble());
        t.groupFileId = quint64(o[QStringLiteral("groupFileId")].toDouble());
        // D-b：按键合并——内存已有同身份任务时不补入（内存为准、快照补缺），
        // 既不清内存也不重复建行。
        if (containsIdentity(m_tasks, t)) continue;
        t.key = m_nextKey++;
        t.id = allocId();   // Paused 任务无 wire 活动；resume 时会重新分配
        if (t.isUpload()) {
            t.totalBytes = QFileInfo(t.localPath).size();
            if (t.localPath.isEmpty() || !QFileInfo::exists(t.localPath)) {
                // 本地文件已删：恢复为 Failed 让用户看见，而不是无声丢弃
                t.state = Failed;
                t.failReason = QStringLiteral("本地文件不存在，无法续传");
            } else {
                t.state = Paused;
                t.wasRestored = true;
                ++m_restoredCount;
            }
        } else {
            t.totalBytes = qint64(o[QStringLiteral("totalBytes")].toDouble());
            // REQ-EXT-TRANSFER-002 C9：分片路径入快照；起点按分片物理大小回填（D9）。
            // 旧快照无 savePath → 空 → 起点 0 →「继续」即今日整文件重传（I3 无回归）。
            t.savePath = o[QStringLiteral("savePath")].toString();
            const QFileInfo shardInfo(t.savePath);
            t.doneBytes = (t.savePath.isEmpty() || !shardInfo.isFile()) ? 0 : shardInfo.size();
            t.state = Paused;
            ++m_restoredCount;
        }
        m_tasks.insert(t.key, t);
    }
    if (m_restoredCount > 0) emit tasksChanged();
}

// D-b：不做"清表重建"。空名/同名（含断线重连、空名→真名）直接保持内存任务不动，
// 恢复走按键合并；只有确实是另一个账号（m_currentUser 非空且名字不同）才丢弃内存任务。
void TransferManager::setCurrentUser(const QString& name)
{
    if (name.isEmpty()) return;
    m_lastKnownUser = name;                      // D-a：兜底键随每次有效登录名更新
    if (name == m_currentUser) return;           // 同一账号重连：内存为准，不清不重建
    if (!m_currentUser.isEmpty()) {
        // 确实是另一个账号：上一个账号的内存任务先落盘，再丢弃内存侧
        saveSnapshot();
        m_tasks.clear();
        m_idToKey.clear();
        m_uploadQueue.clear();
        m_downloadQueue.clear();
        m_fastcheckQueue.clear();
        m_fastcheckInFlight = 0;
        m_fastcheckInFlightToken.clear();
        m_activeUploads = 0;
        m_activeDownloads = 0;
    }
    m_currentUser = name;
    restoreSavedTasks();                         // 恢复 = 合并（内存为准、快照补缺）
}

void TransferManager::saveSnapshotNow()
{
    saveSnapshot();
}

QString TransferManager::stateText(State s)
{
    switch (s) {
    case Queued:   return QStringLiteral("排队中");
    case Hashing:  return QStringLiteral("计算摘要…");
    case Running:  return QStringLiteral("传输中");
    case Paused:   return QStringLiteral("已暂停");
    case Done:     return QStringLiteral("已完成");
    case Failed:   return QStringLiteral("失败");
    case Cancelled:return QStringLiteral("已取消");
    }
    return QString();
}

// 41 的 id 落在 [16..23]，其余传输族 [48..55]
quint64 TransferManager::transferIdOf(const PDU* pdu)
{
    quint64 id = 0;
    if (pdu == nullptr) return 0;
    const int off = (pdu->uiType == ENUM_MSG_TYPE_DOWNLOADFILE_RESPOND)
            ? DOWNLOAD_RESP_TRANSFER_ID_OFFSET : TRANSFER_ID_OFFSET;
    memcpy(&id, pdu->caData + off, sizeof(id));
    return id;
}

quint64 TransferManager::stableKeyOf(quint64 transferId)
{
    return m_idToKey.value(transferId, 0);
}

QList<quint64> TransferManager::taskKeys() const
{
    QList<quint64> keys = m_tasks.keys();
    std::sort(keys.begin(), keys.end());
    return keys;
}

quint64 TransferManager::enqueuePersonalUpload(const QString& localPath, const QString& dirPath, const QString& displayName)
{
    Task t;
    t.key = m_nextKey++;
    t.id = allocId();
    t.kind = PersonalUpload;
    t.localPath = localPath;
    t.dirPath = dirPath;
    t.displayName = displayName;
    t.totalBytes = QFileInfo(localPath).size();
    t.state = Queued;
    m_tasks.insert(t.key, t);
    m_idToKey.insert(t.id, t.key);
    m_uploadQueue.append(t.key);
    saveSnapshot();
    emit tasksChanged();
    pump();
    return t.key;
}

quint64 TransferManager::enqueueGroupUpload(quint64 groupId, const QString& localPath,
                                            const QString& displayName, const QByteArray& shaHex)
{
    Task t;
    t.key = m_nextKey++;
    t.id = allocId();
    t.kind = GroupUpload;
    t.localPath = localPath;
    t.groupId = groupId;
    t.displayName = displayName;
    t.shaHex = shaHex;
    t.totalBytes = QFileInfo(localPath).size();
    t.state = Queued;
    m_tasks.insert(t.key, t);
    m_idToKey.insert(t.id, t.key);
    m_uploadQueue.append(t.key);
    saveSnapshot();
    emit tasksChanged();
    pump();
    return t.key;
}

quint64 TransferManager::enqueuePersonalDownload(const QString& serverPath, const QString& displayName)
{
    Task t;
    t.key = m_nextKey++;
    t.id = allocId();
    t.kind = PersonalDownload;
    t.serverPath = serverPath;
    t.displayName = displayName;
    t.state = Queued;
    m_tasks.insert(t.key, t);
    m_idToKey.insert(t.id, t.key);
    m_downloadQueue.append(t.key);
    saveSnapshot();
    emit tasksChanged();
    pump();
    return t.key;
}

quint64 TransferManager::enqueueGroupDownload(quint64 groupId, quint64 groupFileId, const QString& displayName)
{
    Task t;
    t.key = m_nextKey++;
    t.id = allocId();
    t.kind = GroupDownload;
    t.groupId = groupId;
    t.groupFileId = groupFileId;
    t.displayName = displayName;
    t.state = Queued;
    m_tasks.insert(t.key, t);
    m_idToKey.insert(t.id, t.key);
    m_downloadQueue.append(t.key);
    saveSnapshot();
    emit tasksChanged();
    pump();
    return t.key;
}

// 两方向预算独立（D6）：上传 3 / 下载 3，各自覆盖个人与群路径
void TransferManager::pump()
{
    if (m_connectionDown) return;   // G7/C7：与服务端断开期间不启动新任务（重连登录后由 onConnectionRestored 恢复）
    while (m_activeUploads < int(MAX_PARALLEL_UPLOADS) && !m_uploadQueue.isEmpty()) {
        const quint64 key = m_uploadQueue.takeFirst();
        Task* t = taskByKey(key);
        if (t == nullptr || t->state != Queued) continue;
        startTask(*t);
    }
    while (m_activeDownloads < int(MAX_PARALLEL_DOWNLOADS) && !m_downloadQueue.isEmpty()) {
        const quint64 key = m_downloadQueue.takeFirst();
        Task* t = taskByKey(key);
        if (t == nullptr || t->state != Queued) continue;
        startTask(*t);
    }
}

// M5：SHA-256 计算移出 UI 线程（1MiB 分块，算法与分段不变）；完成后回主线程发 101
void TransferManager::startGroupHash(Task& t)
{
    t.state = Hashing;
    ++m_activeUploads;
    const quint64 key = t.key;
    auto* watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [this, key, watcher]() {
        const QByteArray sha = watcher->result();
        watcher->deleteLater();
        Task* tt = taskByKey(key);
        if (tt == nullptr) return;
        if (tt->state != Hashing) return;   // G7/C6：断线已把任务收敛为失败 → 迟到的 SHA 回调不得复活它
        if (sha.isEmpty()) {
            finishTask(*tt, Failed, QStringLiteral("读取文件失败"));
            return;
        }
        tt->shaHex = sha;
        tt->state = Running;   // 名额已在 Hashing 时计入，不重复
        sendGroupUploadInit(*tt);
        emit tasksChanged();
    });
    const QString path = t.localPath;
    watcher->setFuture(QtConcurrent::run([path]() -> QByteArray {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        QCryptographicHash hasher(QCryptographicHash::Sha256);
        while (!f.atEnd()) {
            const QByteArray chunk = f.read(1024 * 1024);
            if (chunk.isEmpty() && f.error() != QFile::NoError) return QByteArray();
            hasher.addData(chunk);
        }
        return hasher.result().toHex();
    }));
    emit tasksChanged();
}

void TransferManager::sendGroupUploadInit(Task& t)
{
    QByteArray payload;
    groupAppendU16(payload, GROUP_PROTOCOL_VERSION);
    groupAppendU16(payload, GROUP_ENVELOPE_LENGTH);
    groupAppendU64(payload, t.id);            // request_id
    groupAppendU64(payload, t.groupId);
    groupAppendU64(payload, quint64(t.totalBytes));
    groupAppendString(payload, t.shaHex);
    groupAppendString(payload, t.displayName.toUtf8());
    PDU* pdu = mkPDU(uint(payload.size()));
    pdu->uiType = ENUM_MSG_TYPE_GROUP_FILE_UPLOAD_INIT_REQUEST;
    memcpy(pdu->caData + TRANSFER_ID_OFFSET, &t.id, sizeof(t.id));
    memcpy(pdu->caMsg, payload.constData(), size_t(payload.size()));
    Client::getinstance().seedMsg(pdu);
    emit tasksChanged();
}

// 单飞：同一时刻只允许一个 97 在途（服务端预检查落在旧单流槽）
void TransferManager::pumpFastcheck()
{
    if (m_fastcheckInFlight != 0) return;
    while (!m_fastcheckQueue.isEmpty()) {
        const quint64 key = m_fastcheckQueue.takeFirst();
        Task* t = taskByKey(key);
        if (t == nullptr || t->state != Hashing) continue;
        m_fastcheckInFlight = key;
        // AUD-3：每次 97 携带唯一请求标识，由服务端 98 回带；客户端据此按标识归属，
        // 98 与在途标识不符（迟到响应）即丢弃，避免误算到当前任务头上。
        m_fastcheckInFlightToken = QString::fromLatin1(
                    QUuid::createUuid().toRfc4122().toHex());
        t->fastcheckToken = m_fastcheckInFlightToken;
        emit fastcheckDue(key);
        return;
    }
}

void TransferManager::submitFastcheck(quint64 key, const QByteArray& shaHex)
{
    Task* t = taskByKey(key);
    if (t == nullptr) return;
    if (t->state != Hashing) return;   // G7/C6：断线失败后到达的 97 提交不得复活任务
    t->shaHex = shaHex;
    m_fastcheckQueue.append(key);
    emit tasksChanged();
    pumpFastcheck();
}

// ===== REQ-EXT-TRANSFER-003 G7：断线任务态收敛 =====
// 与服务端断开（socket disconnected / 90 s 心跳丢失）：在途（Running/Hashing）任务即刻判失败并归还名额，
// 任务行不再永久"传输中"、队列可继续前进；Queued/Paused/Done/Failed 一律不动。
// 边界：不删任何本地分片、不动服务端暂存与会话行（留给"重试即续传"）；不做自动重发。
void TransferManager::onConnectionLost(const QString& reason)
{
    m_connectionDown = true;
    m_fastcheckQueue.clear();      // 在途 97 的归属一并作废（迟到 98 由"无在途即丢弃"吞掉）
    m_fastcheckInFlight = 0;
    m_fastcheckInFlightToken.clear();
    const QList<quint64> keys = m_tasks.keys();
    for (quint64 key : keys) {
        Task* t = taskByKey(key);
        if (t == nullptr) continue;
        if (t->state != Running && t->state != Hashing) continue;
        // §4.3 同款防御：摘旧 id 映射，防止迟到 32/42 喂进下一条任务
        m_idToKey.remove(t->id);
        finishTask(*t, Failed, reason);
    }
    emit tasksChanged();
}

// 重连并重新登录后恢复调度（由 Client 的 LOGIN_RESPOND 分支调用，不依赖 setCurrentUser 的提前返回）
void TransferManager::onConnectionRestored()
{
    if (!m_connectionDown) return;
    m_connectionDown = false;
    qCDebug(lcCli) << "[transfer] 连接恢复，重新调度排队任务";
    pump();
}

void TransferManager::failTask(quint64 key, const QString& reason)
{
    Task* t = taskByKey(key);
    if (t == nullptr) return;
    m_fastcheckQueue.removeOne(key);
    if (m_fastcheckInFlight == key) {
        m_fastcheckInFlight = 0;
        m_fastcheckInFlightToken.clear();
    }
    finishTask(*t, Failed, reason);
}

// 97 → 98 → 29（服务端采纳 97 建立的暂存会话）→ 30 → 31/32
void TransferManager::onFastcheckRespond(PDU* pdu)
{
    // AUD-3：无在途 97 → 迟到/孤儿 98，丢弃（不推进任何任务状态，也不释放单飞槽）。
    if (m_fastcheckInFlight == 0) {
        return;
    }
    // 服务端回带的请求标识（98 caMsg = 名字 || 标识，nameLen 只算名字）；无回带则不校验（兼容拒绝路径）。
    QString echoed;
    if (pdu->caMsg != nullptr && pdu->uiMsgLen > 0) {
        uint nameLen = 0;
        memcpy(&nameLen, pdu->caData + FASTCHECK_RESP_NAME_LEN_OFFSET, sizeof(nameLen));
        if (nameLen < uint(pdu->uiMsgLen)) {
            echoed = QString::fromLatin1(pdu->caMsg + nameLen, int(pdu->uiMsgLen) - int(nameLen));
        }
    }
    // 有回带但不等于在途标识 → 这是更早一次 97 的迟到响应：丢弃，保留在途槽等待真正响应。
    if (!echoed.isEmpty() && echoed != m_fastcheckInFlightToken) {
        return;
    }
    const quint64 key = m_fastcheckInFlight;
    m_fastcheckInFlight = 0;
    m_fastcheckInFlightToken.clear();
    Task* t = taskByKey(key);
    if (t == nullptr) {
        pumpFastcheck();
        return;
    }
    bool ret = false, hit = false;
    uint version = 0;
    memcpy(&ret, pdu->caData + FASTCHECK_RESP_RET_OFFSET, sizeof(ret));
    memcpy(&hit, pdu->caData + FASTCHECK_RESP_HIT_OFFSET, sizeof(hit));
    memcpy(&version, pdu->caData + FASTCHECK_RESP_VERSION_OFFSET, sizeof(version));
    if (version != FASTCHECK_VERSION) {
        finishTask(*t, Failed, QStringLiteral("服务端协议版本不匹配，请升级客户端"));
        pumpFastcheck();
        return;
    }
    if (!ret) {
        finishTask(*t, Failed, QStringLiteral("上传失败：服务端拒绝秒传预检查"));
        pumpFastcheck();
        return;
    }
    if (hit) {
        finishTask(*t, Done);   // 秒传命中：零正文，服务端已建条目
        pumpFastcheck();
        return;
    }
    sendPersonalUploadInit(*t);
    pumpFastcheck();
}

void TransferManager::sendPersonalUploadInit(Task& t)
{
    const qint64 size = QFileInfo(t.localPath).size();
    const QByteArray nameBytes = t.displayName.toUtf8();
    const QByteArray dirBytes = t.dirPath.toUtf8();
    PDU* pdu = mkPDU(uint(dirBytes.size() + 1));
    pdu->uiType = ENUM_MSG_TYPE_UPLOADFILE_INIT_REQUEST;
    memcpy(pdu->caData, nameBytes.constData(), qMin(nameBytes.size(), 31));
    memcpy(pdu->caData + UPLOAD_FILE_SIZE_OFFSET, &size, sizeof(size));
    const uint magic = UPLOAD_RESUME_MAGIC;
    memcpy(pdu->caData + UPLOAD_RESUME_MAGIC_OFFSET, &magic, sizeof(magic));
    memcpy(pdu->caData + TRANSFER_ID_OFFSET, &t.id, sizeof(t.id));
    memcpy(pdu->caMsg, dirBytes.constData(), size_t(dirBytes.size()));
    t.totalBytes = size;
    t.state = Running;
    Client::getinstance().seedMsg(pdu);
    emit tasksChanged();
}

void TransferManager::startTask(Task& t)
{
    if (t.kind == GroupUpload && t.shaHex.isEmpty()) {
        startGroupHash(t);
        return;
    }
    if (t.kind == PersonalUpload) {
        ++m_activeUploads;
        if (t.needFastcheck) {
            t.state = Hashing;              // SHA 由 File 在后台计算（M5），随后单飞发 97
            emit hashDue(t.key, t.localPath);
        } else {
            sendPersonalUploadInit(t);      // 续传：直接 29（服务端由确定性 staging 回带续传起点）
        }
        emit tasksChanged();
        return;
    }
    Client& cli = Client::getinstance();
    if (t.kind == GroupUpload) {
        t.state = Running;
        ++m_activeUploads;
        sendGroupUploadInit(t);
        return;
    } else if (t.kind == PersonalDownload) {
        const QByteArray nameBytes = t.displayName.toUtf8();
        const QByteArray pathBytes = t.serverPath.toUtf8();
        PDU* pdu = mkPDU(uint(pathBytes.size() + 1));
        pdu->uiType = ENUM_MSG_TYPE_DOWNLOADFILE_REQUEST;
        memcpy(pdu->caData, nameBytes.constData(), qMin(nameBytes.size(), 31));
        // REQ-EXT-TRANSFER-002 C3：声明续传起点（新任务 doneBytes=0 = 从头，旧语义由 mkPDU 清零保证）
        const qint64 startOffset = t.doneBytes;
        memcpy(pdu->caData + DOWNLOAD_RESUME_OFFSET, &startOffset, sizeof(startOffset));
        memcpy(pdu->caData + TRANSFER_ID_OFFSET, &t.id, sizeof(t.id));
        memcpy(pdu->caMsg, pathBytes.constData(), size_t(pathBytes.size()));
        t.state = Running;
        ++m_activeDownloads;
        cli.seedMsg(pdu);
    } else { // GroupDownload
        QByteArray payload;
        groupAppendU16(payload, GROUP_PROTOCOL_VERSION);
        groupAppendU16(payload, GROUP_ENVELOPE_LENGTH);
        groupAppendU64(payload, t.id);            // request_id
        groupAppendU64(payload, t.groupId);
        groupAppendU64(payload, t.groupFileId);
        PDU* pdu = mkPDU(uint(payload.size()));
        pdu->uiType = ENUM_MSG_TYPE_GROUP_FILE_DOWNLOAD_REQUEST;
        // REQ-EXT-TRANSFER-002 C3：与 40 同一常量同一偏移；caMsg 群信封零改动（I7）
        const qint64 startOffset = t.doneBytes;
        memcpy(pdu->caData + DOWNLOAD_RESUME_OFFSET, &startOffset, sizeof(startOffset));
        memcpy(pdu->caData + TRANSFER_ID_OFFSET, &t.id, sizeof(t.id));
        memcpy(pdu->caMsg, payload.constData(), size_t(payload.size()));
        t.state = Running;
        ++m_activeDownloads;
        cli.seedMsg(pdu);
    }
    emit tasksChanged();
}

void TransferManager::finishTask(Task& t, State s, const QString& reason)
{
    if (t.isUpload()) {
        if (t.state == Running || t.state == Hashing) {
            if (m_activeUploads > 0) --m_activeUploads;
        }
        if (t.uploader) {
            t.uploader->deleteLater();
            t.uploader = nullptr;
        }
    } else {
        if (t.state == Running && m_activeDownloads > 0) --m_activeDownloads;
        // REQ-EXT-TRANSFER-002 §4.3（FINDING-AUDIT-1）：收尾即摘 id 映射——否则已 Failed/Done 的
        // 下载任务仍被在途 42/43 经 taskByTransferId 命中，43 会把失败态翻转成 Done（静默损坏件）。
        m_idToKey.remove(t.id);
        closeRecv(t);
    }
    t.state = s;
    if (!reason.isEmpty()) t.failReason = reason;
    saveSnapshot();
    emit tasksChanged();
    pump();
}

void TransferManager::closeRecv(Task& t)
{
    if (t.recvFile) {
        t.recvFile->close();
        delete t.recvFile;
        t.recvFile = nullptr;
    }
}

// 客户目录/yyyy-MM/ 下按 (N) 递推去重（沿用既有落盘语义，§5.4 验收 3）
bool TransferManager::beginReceiving(Task& t, const QString& fileName)
{
    const QString safeName = fileName.isEmpty() ? QStringLiteral("download.bin") : fileName;
    File* f = Index::getinstance().getFile();
    if (f == nullptr) return false;
    const QString monthDirPath = QString("%1/%2").arg(f->m_strUserPath)
            .arg(QDate::currentDate().toString("yyyy-MM"));
    QDir dir;
    if (!dir.mkpath(monthDirPath)) {
        t.failReason = QStringLiteral("创建下载目录失败");
        return false;
    }
    QString savePath = QDir(monthDirPath).filePath(safeName);
    const QFileInfo saveInfo(savePath);
    const QString baseName = saveInfo.completeBaseName();
    const QString suffix = saveInfo.suffix();
    int index = 1;
    while (QFileInfo::exists(savePath)) {
        const QString candidate = suffix.isEmpty()
                ? QString("%1(%2)").arg(baseName).arg(index)
                : QString("%1(%2).%3").arg(baseName).arg(index).arg(suffix);
        savePath = QDir(monthDirPath).filePath(candidate);
        ++index;
    }
    closeRecv(t);
    t.recvFile = new QFile(savePath);
    if (!t.recvFile->open(QIODevice::WriteOnly)) {
        delete t.recvFile;
        t.recvFile = nullptr;
        t.failReason = QStringLiteral("无法创建下载文件");
        return false;
    }
    t.savePath = savePath;
    t.displayName = safeName;
    // 立刻落盘。分片路径原来只在内存里，直到下一次状态变化（暂停/取消/收尾）才写进快照；
    // 而下载中直接关闭客户端（没有先暂停）不会触发任何一次，快照仍停留在入队时那一版 ——
    // 那时 savePath 还是空的。于是重启恢复出空 savePath → 起点 0 → beginReceiving → 另起
    // 一个 (N) 新文件。实测：暂停后关闭能续传，下载中直接断开则不能。路径一旦确定就写，
    // 这个窗口即消失。（进度不必落盘：恢复时按分片物理大小推导，见 D9。）
    saveSnapshot();
    return true;
}

// REQ-EXT-TRANSFER-002 C5：续传 = 重开同一分片（ReadWrite）并 seek 到声明起点；(N) 递推仅属于新任务
bool TransferManager::resumeReceiving(Task& t, qint64 offset)
{
    closeRecv(t);
    if (t.savePath.isEmpty()) {
        t.failReason = QStringLiteral("无法打开下载分片");
        return false;
    }
    t.recvFile = new QFile(t.savePath);
    if (!t.recvFile->open(QIODevice::ReadWrite) || !t.recvFile->seek(offset)) {
        delete t.recvFile;
        t.recvFile = nullptr;
        t.failReason = QStringLiteral("无法打开下载分片");
        return false;
    }
    t.doneBytes = offset;
    return true;
}

// REQ-EXT-TRANSFER-002 C4 ①-⑤：续传护栏。requested>0 时任何不匹配都必须**明确失败**，
// 绝不把后缀写进错误偏移造成静默损坏（D6：旧服务端采纳起点恒 0 → accepted != requested）。
// 返回值：0 = 正常受理；1 = 失败（未接受续传起点）；2 = 服务端文件已变化（需删分片，D8）。
static int checkResumeRespond(qint64 requested, qint64 accepted, qint64 size, qint64 knownTotal)
{
    if (requested > 0 && (accepted != requested)) return 1;
    if (requested > 0 && size != knownTotal) return 2;
    return 0;
}

TransferManager::Task* TransferManager::taskByTransferId(quint64 id)
{
    const quint64 key = stableKeyOf(id);
    if (key == 0) return nullptr;
    return taskByKey(key);
}

void TransferManager::onUploadInitRespond(PDU* pdu)
{
    const quint64 id = transferIdOf(pdu);
    Task* t = taskByTransferId(id);
    if (t == nullptr) return;
    bool ret = false;
    qint64 resumeOffset = 0;
    uint magic = 0;
    memcpy(&ret, pdu->caData, sizeof(ret));
    memcpy(&magic, pdu->caData + UPLOAD_RESUME_MAGIC_OFFSET, sizeof(magic));
    if (magic == UPLOAD_RESUME_MAGIC) {
        memcpy(&resumeOffset, pdu->caData + UPLOAD_RESUME_OFFSET, sizeof(resumeOffset));
    }
    if (!ret) {
        // AUD-2 修复（design §3.3 / 验收 5）：INIT 被拒 = 合法的"继续排队"信号，不是失败。
        if (t->state != Running) return;   // 迟到响应（任务已被暂停/取消）：忽略，名额此前已归还
        if (m_activeUploads > 0) --m_activeUploads;   // 归还本任务在 startTask 计入的名额
        if (m_activeUploads > 0) {
            // 仍有在途上传：其结束事件（finishTask→pump）驱动本任务重试；此处不立即重发，避免自旋。
            // 摘要已由首次 97 判为未命中，重试直接走 29（与 resume() 同语义）。
            t->state = Queued;
            t->needFastcheck = false;
            t->failReason.clear();
            m_uploadQueue.append(t->key);
            emit tasksChanged();
            return;
        }
        // 无在途上传可释放名额：该拒绝不会自愈，收口为失败（避免无限静默排队）
        t->state = Queued;   // 名额已归还，避免 finishTask 二次扣减
        finishTask(*t, Failed, QStringLiteral("服务端拒绝上传初始化"));
        return;
    }
    // REQ-EXT-TRANSFER-003 C3/D9：空文件或断点已在尾部（resumeOffset >= 本地大小，含 0>=0）
    // → 服务端在 result.complete 时已转正，客户端直接判完成，不建 Uploader（否则空文件必失败）
    if (resumeOffset >= t->totalBytes) {
        finishTask(*t, Done);
        return;
    }
    t->uploader = new Uploader(id, t->localPath, resumeOffset, this);
    connect(t->uploader, &Uploader::seedMsg, &Client::getinstance(), &Client::seedMsg);
    const quint64 uploadKey = t->key;
    connect(t->uploader, &Uploader::errnoMsg, this, [this, uploadKey](const QString& msg) {
        Task* tt = taskByKey(uploadKey);
        if (tt) finishTask(*tt, Failed, msg);
    });
    connect(t->uploader, &Uploader::finished, this, [this, uploadKey](bool ok) {
        Task* tt = taskByKey(uploadKey);
        if (!tt) return;
        if (ok) finishTask(*tt, Done);
        else finishTask(*tt, Failed, QStringLiteral("上传失败"));
    });
    t->uploader->start();
    emit tasksChanged();
}

void TransferManager::onUploadDataRespond(PDU* pdu)
{
    const quint64 id = transferIdOf(pdu);
    Task* t = taskByTransferId(id);
    if (t == nullptr || t->uploader == nullptr) return;
    bool ret = true, complete = false;
    qint64 nextOffset = 0;
    uint magic = 0;
    memcpy(&magic, pdu->caData + UPLOAD_RESUME_MAGIC_OFFSET, sizeof(magic));
    if (magic == UPLOAD_RESUME_MAGIC) {
        memcpy(&ret, pdu->caData, sizeof(ret));
        memcpy(&nextOffset, pdu->caData + UPLOAD_ACK_NEXT_OFFSET, sizeof(nextOffset));
        memcpy(&complete, pdu->caData + UPLOAD_ACK_COMPLETE_OFFSET, sizeof(complete));
    }
    t->doneBytes = nextOffset;
    t->uploader->acknowledge(ret, nextOffset, complete);
    emit tasksChanged();
}

void TransferManager::onGroupUploadInitRespond(PDU* pdu)
{
    const quint64 id = transferIdOf(pdu);
    Task* t = taskByTransferId(id);
    if (t == nullptr) return;
    const QByteArray payload(pdu->caMsg, int(pdu->uiMsgLen));
    int offset = 0;
    quint16 version = 0, length = 0;
    quint64 requestId = 0, groupId = 0;
    quint32 code = 0;
    if (!groupReadU16(payload, offset, version) || !groupReadU16(payload, offset, length)
            || !groupReadU64(payload, offset, requestId) || !groupReadU64(payload, offset, groupId)
            || !groupReadU32(payload, offset, code)
            || version != GROUP_PROTOCOL_VERSION || length != GROUP_ENVELOPE_LENGTH) {
        finishTask(*t, Failed, QStringLiteral("群上传响应非法"));
        return;
    }
    if (code != 0) {
        finishTask(*t, Failed, QStringLiteral("群上传拒绝（%1）").arg(qint32(code)));
        return;
    }
    if (payload.size() < GROUP_FILE_INIT_RESP_NAME_OFFSET) {
        finishTask(*t, Failed, QStringLiteral("群上传响应截断"));
        return;
    }
    auto readU64At = [&](int at) -> quint64 {
        quint64 v = 0;
        for (int i = 0; i < 8; ++i) v |= quint64(quint8(payload.at(at + i))) << (8 * i);
        return v;
    };
    const bool hit = (quint8(payload.at(GROUP_FILE_INIT_RESP_HIT_OFFSET)) != 0);
    const qint64 nextOffset = qint64(readU64At(GROUP_FILE_INIT_RESP_NEXT_OFFSET));
    if (hit) {
        finishTask(*t, Done);
        return;
    }
    // REQ-EXT-TRANSFER-003 C3/D9：断点已在尾部（含 0 字节）→ 服务端已落定，直接完成
    if (nextOffset >= t->totalBytes) {
        finishTask(*t, Done);
        return;
    }
    t->uploader = new Uploader(id, t->localPath, nextOffset, this);
    connect(t->uploader, &Uploader::seedMsg, &Client::getinstance(), &Client::seedMsg);
    const quint64 groupUploadKey = t->key;
    connect(t->uploader, &Uploader::errnoMsg, this, [this, groupUploadKey](const QString& msg) {
        Task* tt = taskByKey(groupUploadKey);
        if (tt) finishTask(*tt, Failed, msg);
    });
    connect(t->uploader, &Uploader::finished, this, [this, groupUploadKey](bool ok) {
        Task* tt = taskByKey(groupUploadKey);
        if (!tt) return;
        if (ok) finishTask(*tt, Done);
        else finishTask(*tt, Failed, QStringLiteral("群上传失败"));
    });
    t->uploader->start();
    emit tasksChanged();
}

void TransferManager::onDownloadRespond(PDU* pdu)
{
    const quint64 id = transferIdOf(pdu);
    Task* t = taskByTransferId(id);
    if (t == nullptr) return;
    bool ret = false;
    qint64 fileSize = 0, accepted = 0;
    memcpy(&ret, pdu->caData, sizeof(ret));
    memcpy(&fileSize, pdu->caData + sizeof(bool), sizeof(fileSize));
    memcpy(&accepted, pdu->caData + DOWNLOAD_ACCEPTED_OFFSET, sizeof(accepted));
    QString fileName = QString::fromUtf8(pdu->caData + 32);
    if (fileName.isEmpty()) fileName = QStringLiteral("download.bin");
    const qint64 requested = t->doneBytes;   // 本次 40 声明的起点（0 = 新任务/从头）
    if (!ret) {
        finishTask(*t, Failed, requested > 0
                ? QStringLiteral("服务端未接受续传起点")
                : QStringLiteral("服务端拒绝下载请求"));
        return;
    }
    const int resumeProblem = checkResumeRespond(requested, accepted, fileSize, t->totalBytes);
    if (resumeProblem == 1) {   // D6 护栏：含新客户端×旧服务端（采纳区间恒 0）
        finishTask(*t, Failed, QStringLiteral("服务端未接受续传起点"));
        return;
    }
    if (resumeProblem == 2) {   // D8：前缀不可信 → 删分片 + 起点归零，绝不拼损坏件
        QFile::remove(t->savePath);
        t->savePath.clear();
        t->doneBytes = 0;
        finishTask(*t, Failed, QStringLiteral("服务端文件已变化，无法续传"));
        return;
    }
    t->totalBytes = fileSize;   // C4⑤：统一回写权威大小
    if (requested > 0) {
        if (!resumeReceiving(*t, accepted)) {
            finishTask(*t, Failed, t->failReason);
            return;
        }
    } else {
        t->doneBytes = 0;
        if (!beginReceiving(*t, fileName)) {
            finishTask(*t, Failed, t->failReason);
            return;
        }
    }
    emit tasksChanged();
}

void TransferManager::onGroupDownloadRespond(PDU* pdu)
{
    const quint64 id = transferIdOf(pdu);
    Task* t = taskByTransferId(id);
    if (t == nullptr) return;
    const qint64 requested = t->doneBytes;   // 本次 105 声明的起点（0 = 新任务/从头）
    quint64 accepted = 0;
    memcpy(&accepted, pdu->caData + DOWNLOAD_ACCEPTED_OFFSET, sizeof(accepted));
    if (pdu->uiMsgLen < 36) {
        finishTask(*t, Failed, QStringLiteral("群下载响应截断"));
        return;
    }
    const QByteArray payload(pdu->caMsg, int(pdu->uiMsgLen));
    int offset = 0;
    quint16 version = 0, length = 0;
    quint64 requestId = 0, groupId = 0;
    quint32 code = 0;
    if (!groupReadU16(payload, offset, version) || !groupReadU16(payload, offset, length)
            || !groupReadU64(payload, offset, requestId) || !groupReadU64(payload, offset, groupId)
            || !groupReadU32(payload, offset, code)
            || version != GROUP_PROTOCOL_VERSION || length != GROUP_ENVELOPE_LENGTH) {
        finishTask(*t, Failed, QStringLiteral("群下载响应非法"));
        return;
    }
    if (code != 0) {
        // D1：-2108=续传起点非法；D7：-2107=并发上限。续传请求未获受理时统一呈现（design C4①）
        finishTask(*t, Failed, requested > 0
                ? QStringLiteral("服务端未接受续传起点")
                : QStringLiteral("群文件下载失败（%1）").arg(qint32(code)));
        return;
    }
    quint64 sizeBytes = 0;
    quint32 nameLen = 0;
    QByteArray nameBytes;
    if (!groupReadU64(payload, offset, sizeBytes) || !groupReadU32(payload, offset, nameLen)
            || !groupReadString(payload, offset, nameBytes)) {
        finishTask(*t, Failed, QStringLiteral("群下载元数据截断"));
        return;
    }
    const qint64 fileSize = qint64(sizeBytes);
    const int resumeProblem = checkResumeRespond(requested, qint64(accepted), fileSize, t->totalBytes);
    if (resumeProblem == 1) {
        // D6 护栏（同 41）：采纳起点 != 声明起点 → 明确失败，含新客户端×旧服务端
        finishTask(*t, Failed, QStringLiteral("服务端未接受续传起点"));
        return;
    }
    if (resumeProblem == 2) {
        // D8：前缀不可信 → 删分片 + 起点归零，绝不拼损坏件
        QFile::remove(t->savePath);
        t->savePath.clear();
        t->doneBytes = 0;
        finishTask(*t, Failed, QStringLiteral("服务端文件已变化，无法续传"));
        return;
    }
    t->totalBytes = fileSize;   // C4⑤：统一回写权威大小
    if (requested > 0) {
        if (!resumeReceiving(*t, qint64(accepted))) {
            finishTask(*t, Failed, t->failReason);
            return;
        }
    } else {
        t->doneBytes = 0;
        if (!beginReceiving(*t, QString::fromUtf8(nameBytes))) {
            finishTask(*t, Failed, t->failReason);
            return;
        }
    }
    emit tasksChanged();
}

void TransferManager::onDownloadData(PDU* pdu)
{
    const quint64 id = transferIdOf(pdu);
    Task* t = taskByTransferId(id);
    if (t == nullptr || t->recvFile == nullptr || !t->recvFile->isOpen()) return;
    const qint64 written = t->recvFile->write(pdu->caMsg, pdu->uiMsgLen);
    if (written != qint64(pdu->uiMsgLen)) {
        qCWarning(lcCli) << "写入下载数据块失败 id=" << id;
        return;
    }
    t->doneBytes += written;
    emit tasksChanged();
}

void TransferManager::onDownloadFinish(PDU* pdu)
{
    const quint64 id = transferIdOf(pdu);
    Task* t = taskByTransferId(id);
    if (t == nullptr) return;
    // REQ-EXT-TRANSFER-002（FINDING-AUDIT-1 双保险）：43 仅对 Running 任务有意义；
    // 护栏/IO 失败后映射虽已摘除，仍拒绝在途 43 改写非 Running 的终态。
    if (t->state != Running) return;
    bool success = false;
    memcpy(&success, pdu->caData, sizeof(success));
    // REQ-EXT-TRANSFER-002 C6（D7）：失败**不再删除**分片——保留是续传前提，
    // 清理仅由「取消」承担；失败任务留同名分片直到取消或重试成功（如实记录，无后台清理）。
    finishTask(*t, success ? Done : Failed, success ? QString() : QStringLiteral("下载失败"));
}

void TransferManager::sendPause(quint64 id)
{
    PDU* pdu = mkPDU();
    pdu->uiType = ENUM_MSG_TYPE_UPLOADFILE_ABORT_REQUEST;
    memcpy(pdu->caData + TRANSFER_ID_OFFSET, &id, sizeof(id));
    Client::getinstance().seedMsg(pdu);
}

// 暂停/取消共用 107（服务端处置相同，区别纯在客户端）
// REQ-EXT-TRANSFER-002 C8：107 现对两方向同义，判定由「上传 && Running」放宽为 Running
void TransferManager::sendAbortIfActive(Task& t)
{
    if (t.state == Running) {
        sendPause(t.id);
    }
}

void TransferManager::onAbortRespond(PDU* pdu)
{
    const quint64 id = transferIdOf(pdu);
    bool ret = false;
    memcpy(&ret, pdu->caData, sizeof(ret));
    Task* t = taskByTransferId(id);
    if (t == nullptr) return;
    // 暂停/取消已在本地落定，108 仅确认服务端已释放槽位（幂等，无需二次动作）
    qCDebug(lcCli) << "[transfer] 108 释放确认 id=" << id << " ret=" << ret;
}

void TransferManager::pause(quint64 key)
{
    Task* t = taskByKey(key);
    if (t == nullptr) return;
    if (!t->isUpload()) {
        // REQ-EXT-TRANSFER-002 C1：下载暂停。Running → 107 停发+释放名额，**保留分片**；Queued → 移出队列
        if (t->state == Queued) {
            m_downloadQueue.removeOne(key);
            t->state = Paused;
            saveSnapshot();
            emit tasksChanged();
            return;
        }
        if (t->state != Running) return;
        sendAbortIfActive(*t);           // 107：服务端摘环停发、名额即时可复用（不等 108，D3）
        closeRecv(*t);                   // 已收字节留在磁盘（续传前提，D7）
        if (m_activeDownloads > 0) --m_activeDownloads;
        m_idToKey.remove(t->id);         // §4.3 必做防御：摘旧 id 映射，仍在途的旧 id 42 一律落空，不静默串写
        t->state = Paused;
        saveSnapshot();
        emit tasksChanged();
        pump();
        return;
    }
    if (t->state == Queued || t->state == Hashing) {
        m_uploadQueue.removeOne(key);
        m_fastcheckQueue.removeOne(key);
        if (m_fastcheckInFlight == key) {
            m_fastcheckInFlight = 0;   // 98 到达时视为无归属，丢弃
            m_fastcheckInFlightToken.clear();
            pumpFastcheck();
        }
        if (t->state == Hashing && m_activeUploads > 0) --m_activeUploads;
        m_idToKey.remove(t->id);   // S-E/C4：摘旧 id 映射（防旧 id 的迟到 32 喂进后续新 uploader）
        t->state = Paused;
        saveSnapshot();
        emit tasksChanged();
        return;
    }
    if (t->state != Running) return;
    sendAbortIfActive(*t);
    // 停发 + 释放本地名额（服务端槽位由 107 释放；TCP 顺序保证 107 先于后续 INIT 到达）
    m_idToKey.remove(t->id);   // S-E/C4：摘旧 id 映射（108 仅日志，迟到 32 不再有归属）
    if (t->uploader) {
        t->uploader->deleteLater();
        t->uploader = nullptr;
    }
    if (m_activeUploads > 0) --m_activeUploads;
    t->state = Paused;
    emit tasksChanged();
    pump();
}

// REQ-EXT-TRANSFER-002 C2：下载「继续/重试」= 换新 id + 声明起点（= 分片物理大小，D9 唯一权威来源）。
// §4.3 必做防御：先摘旧 id 映射再入新映射——暂停/失败瞬间仍在途的旧 id 42 数据块一律落空，不静默串写。
void TransferManager::resumeDownload(Task& t)
{
    m_idToKey.remove(t.id);
    t.id = allocId();
    m_idToKey.insert(t.id, t.key);
    qint64 shard = 0;
    QString shardNotice;
    if (!t.savePath.isEmpty()) {
        const QFileInfo fi(t.savePath);
        if (fi.isFile()) shard = fi.size();
        else {
            // 分片缺失。这里过去是静默 clear()，于是「用户自己删了分片」（正常）与「分片路径解析
            // 失效 / 跨月目录变化」（异常）表现完全一样：进度悄悄归零、另起一个新文件，界面上没有
            // 任何提示——排查时只能看到「又开始从头下载」。现在把结论写成可见的原因（面板第 4 列
            // 「失败原因」原样显示 failReason），并保留原路径，用来区分是这两类中的哪一类。
            shardNotice = QStringLiteral("未找到本地分片，改为整文件重下（原分片路径：%1）")
                              .arg(t.savePath);
            t.savePath.clear();
        }
    }
    t.doneBytes = shard;
    t.failReason = shardNotice;   // 无分片问题时为空串，等于沿用既有「继续即清空旧原因」语义
    t.state = Queued;
    m_downloadQueue.append(t.key);
    saveSnapshot();
    emit tasksChanged();
    pump();
}

// 恢复任务：上传走 29 INIT 续传（服务端按 staging 续），下载重发 40/105（起点 = 分片物理大小）
void TransferManager::resume(quint64 key)
{
    Task* t = taskByKey(key);
    if (t == nullptr) return;
    if (!t->isUpload()) {
        if (t->state != Paused && t->state != Failed) return;
        resumeDownload(*t);
        return;
    }
    if (t->state != Paused && t->state != Failed) return;
    // S3：以新 id 重走 INIT，服务端由 (owner|parent|display|size) 派生同一 staging → 续传
    m_idToKey.remove(t->id);   // S-E/C4：换 id 前摘旧映射，防旧 id 的迟到 32 喂进新 uploader
    t->id = allocId();
    m_idToKey.insert(t->id, key);
    t->failReason.clear();
    t->state = Queued;
    if (t->kind == PersonalUpload) {
        if (t->wasRestored) {
            // 快照恢复的任务：首次继续仍走 97 预检查（服务端 staging 可能已清空，
            // 直接 29 会被拒并陷入 AUD-2 重试循环；97 能拿到真实状态），走完即归位 S3 快路径
            t->wasRestored = false;
        } else {
            t->needFastcheck = false;   // S3：续传不再做秒传预检查（staging 已在服务端）
        }
    }
    m_uploadQueue.append(key);
    saveSnapshot();
    emit tasksChanged();
    pump();
}

void TransferManager::cancel(quint64 key)
{
    Task* t = taskByKey(key);
    if (t == nullptr) return;
    if (!t->isUpload()) {
        // REQ-EXT-TRANSFER-002 C7：下载取消 = Running 先 107 停发；**删除分片**（仅取消删除）；摘 id 映射
        if (t->state == Running) {
            sendAbortIfActive(*t);
            if (m_activeDownloads > 0) --m_activeDownloads;
            closeRecv(*t);
        } else if (t->state == Queued) {
            m_downloadQueue.removeOne(key);
        }
        if (!t->savePath.isEmpty()) {
            QFile::remove(t->savePath);
            t->savePath.clear();
        }
        m_idToKey.remove(t->id);
        m_tasks.remove(key);
        saveSnapshot();
        emit tasksChanged();
        pump();
        return;
    }
    if (t->state == Running) {
        sendAbortIfActive(*t);
        if (t->uploader) {
            t->uploader->deleteLater();
            t->uploader = nullptr;
        }
        if (m_activeUploads > 0) --m_activeUploads;
    } else if (t->state == Queued) {
        m_uploadQueue.removeOne(key);
    } else if (t->state == Hashing) {
        if (m_activeUploads > 0) --m_activeUploads;
    }
    m_uploadQueue.removeOne(key);
    m_fastcheckQueue.removeOne(key);
    if (m_fastcheckInFlight == key) {
        m_fastcheckInFlight = 0;   // 98 到达时视为无归属，丢弃
        m_fastcheckInFlightToken.clear();
    }
    m_idToKey.remove(t->id);
    m_tasks.remove(key);
    saveSnapshot();
    emit tasksChanged();
    pumpFastcheck();
    pump();
}

void TransferManager::retry(quint64 key)
{
    Task* t = taskByKey(key);
    if (t == nullptr) return;
    if (t->isUpload()) {
        resume(key);
        return;
    }
    if (t->state != Failed) return;
    resumeDownload(*t);   // C2：与"继续"同一路径——摘旧映射、起点 = 分片大小（失败重试不再从头）
}

void TransferManager::showPanel()
{
    if (m_panel == nullptr) {
        m_panel = new TransferTaskPanel();
        connect(this, &TransferManager::tasksChanged,
                static_cast<TransferTaskPanel*>(m_panel), &TransferTaskPanel::refresh);
    }
    m_panel->show();
    m_panel->raise();
    m_panel->activateWindow();
    static_cast<TransferTaskPanel*>(m_panel)->refresh();
}

// ===================== TransferTaskPanel =====================

TransferTaskPanel::TransferTaskPanel(QWidget* parent)
    : QWidget(parent),
      m_table(new QTableWidget(this)),
      m_pauseBtn(new QPushButton(QStringLiteral("暂停"), this)),
      m_resumeBtn(new QPushButton(QStringLiteral("继续"), this)),
      m_cancelBtn(new QPushButton(QStringLiteral("取消"), this)),
      m_retryBtn(new QPushButton(QStringLiteral("重试"), this)),
      m_hint(new QLabel(QStringLiteral("双击传输行不可用；暂停/继续/取消对上传与下载同样生效（下载从本地分片断点续传）。"), this))
{
    setWindowTitle(QStringLiteral("传输任务"));
    resize(680, 360);

    m_table->setColumnCount(5);
    m_table->setHorizontalHeaderLabels(
                {QStringLiteral("文件"), QStringLiteral("方向"), QStringLiteral("进度"),
                 QStringLiteral("状态"), QStringLiteral("失败原因")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    auto* toolbar = new QHBoxLayout;
    toolbar->addWidget(m_pauseBtn);
    toolbar->addWidget(m_resumeBtn);
    toolbar->addWidget(m_cancelBtn);
    toolbar->addWidget(m_retryBtn);
    toolbar->addStretch();

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(m_hint);
    layout->addLayout(toolbar);
    layout->addWidget(m_table);

    connect(m_pauseBtn, &QPushButton::clicked, this, &TransferTaskPanel::onPause);
    connect(m_resumeBtn, &QPushButton::clicked, this, &TransferTaskPanel::onResume);
    connect(m_cancelBtn, &QPushButton::clicked, this, &TransferTaskPanel::onCancel);
    connect(m_retryBtn, &QPushButton::clicked, this, &TransferTaskPanel::onRetry);
    refresh();
}

quint64 TransferTaskPanel::selectedKey() const
{
    const int row = m_table->currentRow();
    if (row < 0) return 0;
    QTableWidgetItem* item = m_table->item(row, 0);
    if (item == nullptr) return 0;
    return item->data(Qt::UserRole).toULongLong();
}

void TransferTaskPanel::refresh()
{
    TransferManager& mgr = TransferManager::instance();
    const QList<quint64> keys = mgr.taskKeys();
    m_table->setRowCount(int(keys.size()));
    if (mgr.restoredCount() > 0) {
        m_hint->setText(QStringLiteral("已恢复 %1 个未完成传输（暂停状态，选中后点\"继续\"）；下载从本地分片断点续传，分片已删则从头重下。")
                        .arg(mgr.restoredCount()));
    }
    int row = 0;
    for (quint64 key : keys) {
        TransferManager::Task* t = mgr.taskByKey(key);
        if (t == nullptr) continue;
        QString dir;
        switch (t->kind) {
        case TransferManager::PersonalUpload:   dir = QStringLiteral("个人上传"); break;
        case TransferManager::GroupUpload:      dir = QStringLiteral("群上传"); break;
        case TransferManager::PersonalDownload: dir = QStringLiteral("个人下载"); break;
        case TransferManager::GroupDownload:    dir = QStringLiteral("群下载"); break;
        }
        QString progress = QStringLiteral("-");
        if (t->totalBytes > 0) {
            progress = QStringLiteral("%1%").arg(int(double(t->doneBytes) * 100.0 / double(t->totalBytes)));
        } else if (!t->isUpload() && t->state == TransferManager::Done) {
            progress = QStringLiteral("100%");
        }
        const QString stateText = TransferManager::stateText(t->state);
        auto* nameItem = new QTableWidgetItem(t->displayName);
        nameItem->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(key));
        m_table->setItem(row, 0, nameItem);
        m_table->setItem(row, 1, new QTableWidgetItem(dir));
        m_table->setItem(row, 2, new QTableWidgetItem(progress));
        m_table->setItem(row, 3, new QTableWidgetItem(stateText));
        m_table->setItem(row, 4, new QTableWidgetItem(t->failReason));
        ++row;
    }
    m_table->setRowCount(row);
}

void TransferTaskPanel::onPause()
{
    const quint64 key = selectedKey();
    if (key != 0) TransferManager::instance().pause(key);
}

void TransferTaskPanel::onResume()
{
    const quint64 key = selectedKey();
    if (key != 0) TransferManager::instance().resume(key);
}

void TransferTaskPanel::onCancel()
{
    const quint64 key = selectedKey();
    if (key != 0) TransferManager::instance().cancel(key);
}

void TransferTaskPanel::onRetry()
{
    const quint64 key = selectedKey();
    if (key != 0) TransferManager::instance().retry(key);
}
