#include "logging.h"
#include "operatedb.h"
#include "databaseconnectionpool.h"
#include "fileobjectstore.h"
#include "server.h"

#include <QDebug>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QRegExp>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QVector>

namespace {
// 生成 32 位 hex 随机盐（熵源为 OS 级密码学安全随机数）
QString generatePasswordSalt()
{
    quint64 entropy[2];
    entropy[0] = QRandomGenerator::system()->generate64();
    entropy[1] = QRandomGenerator::system()->generate64();
    return QString::fromLatin1(
        QCryptographicHash::hash(QByteArray(reinterpret_cast<const char *>(entropy), sizeof(entropy)),
                                 QCryptographicHash::Sha256).toHex().left(32));
}

// salt 为空表示迁移前的老数据：保持裸哈希原值比对（登录成功后由调用方补盐升级）
QString applyPasswordSalt(const QString &clientHash, const QString &salt)
{
    if (salt.isEmpty()) {
        return clientHash;
    }
    return QString::fromLatin1(
        QCryptographicHash::hash((clientHash + salt).toUtf8(), QCryptographicHash::Md5).toHex());
}
}

OperateDB::~OperateDB()
{
}

OperateDB &OperateDB::instance()
{
    static OperateDB s;
    return s;
}

OperateDB::OperateDB(QObject *parent) : QObject(parent)
{
}

void OperateDB::connect()
{
    Server& server = Server::getinstance();
    DatabaseConnectionPool::instance().init(
        server.m_dbHost,
        server.m_dbPort,
        server.m_dbUser,
        server.m_dbPassword,
        server.m_dbName
    );
    bool ok = DatabaseConnectionPool::instance().testConnection();
    if (ok) {
        qCDebug(lcDb) << "OperateDB::connect - 数据库连接池初始化成功";
    } else {
        qCCritical(lcDb) << "OperateDB::connect - 数据库连接池初始化失败: testConnection 返回 false";
    }
}

// REQ-EXT-FILE-001 T1：启动 schema 版本检查（D1 fail-fast：库不可达/表缺失/版本低一律拒绝启动，
// 不静默建表、不自动执行迁移——迁移只由 migrate_ext_file_001.sql 人工受控执行）。
bool OperateDB::checkSchemaVersion()
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) {
        qCCritical(lcDb) << "[checkSchemaVersion] 数据库不可达：请先检查 connect.config 与 MySQL 服务";
        return false;
    }
    QSqlQuery q(connection.database());
    if (!q.exec("SELECT MAX(version) FROM schema_version")) {
        qCCritical(lcDb) << "[checkSchemaVersion] 读取 schema_version 失败（表不存在？）:"
                         << q.lastError().text()
                         << "— 请先执行 Server/migrate_ext_file_001.sql、migrate_ext_group_001.sql、"
                            "migrate_ext_group_002.sql、migrate_ext_file_002.sql、migrate_ext_group_003.sql "
                            "与 migrate_ext_reclaim_001.sql";
        return false;
    }
    int version = 0;
    if (q.next() && !q.value(0).isNull()) {
        version = q.value(0).toInt();
    }
    const int required = qMax(kFileAssetSchemaVersion,
                              qMax(kGroupSchemaVersion,
                                   qMax(kGroupMessageSchemaVersion,
                                        qMax(kFastUploadSchemaVersion,
                                             qMax(kGroupFileSchemaVersion, kReclaimSchemaVersion)))));
    if (version < required) {
        qCCritical(lcDb) << "[checkSchemaVersion] schema 版本" << version << "低于要求的" << required
                         << "— 请先执行 Server/migrate_ext_file_001.sql、migrate_ext_group_001.sql、"
                            "migrate_ext_group_002.sql、migrate_ext_file_002.sql、migrate_ext_group_003.sql "
                            "与 migrate_ext_reclaim_001.sql";
        return false;
    }
    qCDebug(lcDb) << "[checkSchemaVersion] OK, version" << version;
    return true;
}

// ===== REQ-EXT-SESSION-002：启动期在线态对账 =====
// 为什么需要：online 的唯一权威来源是本进程的活连接——置 1 在 handleLogin，置 0 只在
// handleOffLine。服务端被 kill / 崩溃 / 断电时，当时在线的人没有任何机会走 handleOffLine，
// 于是库里永久留下 online=1 的幽灵：在线列表与用户信息把它们当在线返回，"加好友"还会
// 朝幽灵转发请求（无接收者、静默丢弃）。只有该用户自己重新登录再断开才会自愈。
// 为什么可以全量清零：启动瞬间本进程一条连接都没有，任何 online=1 都必然是上个进程的残留；
// 而"断开即归零"由 handleOffLine 保证，故清零后不会冤枉任何真实在线的会话。
// 单实例假设（重要）：本项目是单进程单端口单库。若将来多实例共享同一库，全量清零会误清
// 另一个实例正在服务的在线用户——那时必须改成"按实例/租约"判活，不能沿用本实现。
bool OperateDB::reconcileOnlineFlags()
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) {
        qCCritical(lcDb) << "[reconcileOnline] 数据库不可达：无法对账上一进程遗留的在线态";
        return false;
    }
    QSqlQuery q(connection.database());
    if (!q.exec("SELECT COUNT(*) FROM user_info WHERE online <> 0")) {
        qCCritical(lcDb) << "[reconcileOnline] 统计遗留在线态失败:" << q.lastError().text();
        return false;
    }
    qint64 stale = 0;
    if (q.next()) {
        stale = q.value(0).toLongLong();
    }
    if (stale == 0) {
        qCDebug(lcDb) << "[reconcileOnline] 无遗留在线态，跳过";
        return true;
    }
    QSqlQuery upd(connection.database());
    if (!upd.exec("UPDATE user_info SET online = 0 WHERE online <> 0")) {
        qCCritical(lcDb) << "[reconcileOnline] 归零失败:" << upd.lastError().text();
        return false;
    }
    qCWarning(lcDb) << "[reconcileOnline] 已把上一进程遗留的在线态归零，条数=" << stale
                    << "（服务端异常终止后客户端没机会走 handleOffLine，属预期残留）";
    return true;
}

// ===== REQ-EXT-FILE-001 T6：存量迁移（design §7：幂等、失败不删旧、file_migration_map 双向映射）=====
void OperateDB::migrateLegacyFiles()
{
    {
        PooledSqlConnection probe = DatabaseConnectionPool::instance().acquire();
        if (!probe.isValid()) {
            qCCritical(lcDb) << "[migrateLegacy] 数据库不可达，跳过存量迁移";
            return;
        }
    }
    const QString fsRoot = QDir(Server::getinstance().filesPath).absolutePath();
    // F-3/D5 修复：映射键存 design §2 规定的相对形态 "<filesys>/<user>/..."（跨树确定、可重放），
    // 机器绝对前缀不再进入 file_migration_map。
    const QString fsBase = QFileInfo(fsRoot).fileName();
    int mapped = 0, skipped = 0, failed = 0;
    const QStringList userDirs = QDir(fsRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& user : userDirs) {
        if (user == "objects" || user == "tmp" || user == ".staging") {
            continue;   // 非用户根目录（D2 布局/暂存区）
        }
        QDirIterator it(QDir(fsRoot).filePath(user), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString oldAbs = QDir::cleanPath(it.next());
            bool artifact = false;
            const QStringList segs = oldAbs.split('/');
            for (const QString& seg : segs) {
                if (seg == "objects" || seg == ".staging" || seg.endsWith(".part")) {
                    artifact = true;
                    break;
                }
            }
            if (artifact) continue;   // 内容寻址对象与暂存实体不是迁移对象
            const QString rel = QDir(fsRoot).relativeFilePath(oldAbs);
            if (rel.isEmpty() || rel.startsWith("..")) {
                qCWarning(lcDb) << "[migrateLegacy] 文件不在 filesys 根内，未迁移:" << oldAbs;
                ++failed;
                continue;
            }
            const QString oldKey = fsBase + "/" + rel;      // design §2: filesys/<user>/... 相对键
            if (oldKey.size() > 191) {
                qCWarning(lcDb) << "[migrateLegacy] 相对路径超 file_migration_map 列宽(191)，未迁移:" << oldKey;
                ++failed;
                continue;
            }
            bool already = false;
            {
                PooledSqlConnection c = DatabaseConnectionPool::instance().acquire();
                if (!c.isValid()) { qCCritical(lcDb) << "[migrateLegacy] 数据库不可达，迁移中断（旧文件零删除）"; return; }
                QSqlQuery q(c.database());
                q.prepare("SELECT status FROM file_migration_map WHERE old_path=?");
                q.addBindValue(oldKey);
                if (q.exec() && q.next()) already = true;   // 重跑：已有映射记录即跳过（failed 留待人工复核）
            }
            if (already) { ++skipped; continue; }

            const QFileInfo fi(oldAbs);
            const FileObjectStore::CommitResult r = FileObjectStore::instance().commitUpload(
                user, fi.absolutePath(), fi.fileName(), fi.size(), oldAbs, /*copyOnly=*/true);
            PooledSqlConnection c2 = DatabaseConnectionPool::instance().acquire();
            if (!c2.isValid()) { qCCritical(lcDb) << "[migrateLegacy] 数据库不可达，迁移中断（旧文件零删除）"; return; }
            QSqlQuery m(c2.database());
            if (r.ok) {
                // 同名不同内容：不覆盖——条目已按 (1) 安置，映射记 skipped_name_conflict（验收4 后半）
                const QString status = (r.displayName == fi.fileName()) ? "mapped" : "skipped_name_conflict";
                m.prepare("INSERT INTO file_migration_map (old_path,file_id,object_id,status) VALUES (?,?,?,?)");
                m.addBindValue(oldKey);
                m.addBindValue(r.fileId);
                m.addBindValue(r.objectId);
                m.addBindValue(status);
                if (m.exec()) {
                    ++mapped;
                } else {
                    qCWarning(lcDb) << "[migrateLegacy] 映射行写入失败:" << oldAbs << m.lastError().text();
                    ++failed;
                }
            } else {
                // 任一文件失败：记 failed，继续其余，不中断、不删任何旧文件（验收5）
                qCWarning(lcDb) << "[migrateLegacy] 迁移失败:" << oldAbs << r.error;
                m.prepare("INSERT INTO file_migration_map (old_path,file_id,object_id,status) VALUES (?,?,NULL,'failed')");
                m.addBindValue(oldKey);
                m.exec();
                ++failed;
            }
        }
    }
    if (mapped || skipped || failed) {
        qCDebug(lcDb) << "[migrateLegacy] mapped" << mapped << "skipped" << skipped << "failed" << failed;
    }
    if (failed > 0) {
        qCWarning(lcDb) << "[migrateLegacy] 存在 failed/超长记录——进 FILE-002/GROUP-003 前必须复核 file_migration_map";
    }
}

// ===== REQ-EXT-FILE-001 F-3/D5 修复：存量键一次性确定性转换（幂等可重放）=====
// user_file/upload_session.parent_path 的旧"绝对机器路径"形态 → owner 相对逻辑键；
// file_migration_map.old_path → design §2 相对形态 "<filesys>/<user>/..."。
// 转换后同一逻辑请求在任何工作树命中同一行（跨树幂等/分享/隔离成立）。
// uk_loc 冲突：按 file_id 最早行保留，后到 active 行软删并镜像 releaseEntry 减引用（D3 不物理删）。
void OperateDB::normalizeLogicKeys()
{
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) {
        qCCritical(lcDb) << "[normalizeLogicKeys] 数据库不可达，跳过存量键转换";
        return;
    }
    QSqlDatabase db = conn.database();
    const QString fsRoot = QDir(Server::getinstance().filesPath).absolutePath();
    const QString fsBase = QFileInfo(fsRoot).fileName();
    int ufConverted = 0, ufCollapsed = 0, ufSkippedDeleted = 0, sessConverted = 0;
    int mapConverted = 0, mapDupRemoved = 0, unmapped = 0;

    // 1) user_file——F-5 修复：单遍确定性。判别与 FileObjectStore::isLogicKeyForm 对齐
    //    （T3/FINDING-DEV-3 修复）：旧 SQL 形状谓词（'/' 开头且 '/' 结尾且无 "//"）把
    //    "/filesys/<owner>/…/"、含盘符 ':'、含 '\'、含 "."/".." 段的旧行误判为 canonical，
    //    与 normalizeParent 的 fs 根 basename 判别不一致，存量行因此不被转换（数据面未收口）。
    //    现判据 canonical ⟺ normalizeParent 原样回显：canonical 行预种子为冲突基准；其余行按
    //    file_id 单遍——撞键时 **不改写 loser 的 parent_path**（改写=与存活行同三重撞
    //    uk_loc→1062 死锁在旧键）。存活优先级：canonical 行 > 最早旧行；loser active→仅
    //    软删+镜像 releaseEntry 减引用（D3 不物理删），deleted→静默跳过（重放稳定无告警）。
    QSet<QString> seenUf;
    {
        QSqlQuery s(db);
        if (s.exec("SELECT owner_name,parent_path,display_name FROM user_file")) {
            while (s.next()) {
                const QString owner = s.value(0).toString();
                const QString pp = s.value(1).toString();
                if (pp.isEmpty() || FileObjectStore::normalizeParent(owner, pp) != pp)
                    continue;                       // 非 canonical：交第 2 段转换
                seenUf.insert(owner + "|" + pp + "|" + s.value(2).toString());
            }
        }
    }
    {
        QSqlQuery rows(db);
        if (rows.exec("SELECT file_id,owner_name,parent_path,display_name,object_id,status FROM user_file "
                      "ORDER BY file_id")) {
            QVector<QVariantList> batch;
            while (rows.next()) {
                QVariantList r;
                for (int i = 0; i < 6; ++i) r.append(rows.value(i));
                batch.append(r);
            }
            for (const QVariantList& r : batch) {
                const qint64 fid = r.at(0).toLongLong();
                const QString owner = r.at(1).toString();
                const QString stored = r.at(2).toString();
                const QString name = r.at(3).toString();
                const QVariant obj = r.at(4);
                const bool active = (r.at(5).toString() == "active");
                const QString key = FileObjectStore::normalizeParent(owner, stored);
                if (key.isEmpty()) {
                    ++unmapped;
                    qCWarning(lcDb) << "[normalizeLogicKeys] user_file 行无法映射，保留原值" << fid << owner << stored;
                    continue;
                }
                if (key == stored) continue;   // 已 canonical：基线阶段已登记
                const QString triple = owner + "|" + key + "|" + name;
                QSqlQuery u(db);
                if (seenUf.contains(triple)) {
                    if (!active) { ++ufSkippedDeleted; continue; }   // 重放稳定：不再触碰、不再告警
                    u.prepare("UPDATE user_file SET status='deleted' WHERE file_id=?");
                    u.addBindValue(fid);
                    if (!u.exec()) { qCWarning(lcDb) << "[normalizeLogicKeys] 冲突收敛失败" << fid << u.lastError().text(); continue; }
                    if (!obj.isNull()) {
                        QSqlQuery d(db);
                        d.prepare("UPDATE file_object SET refcount=GREATEST(refcount-1,0),"
                                  "status=IF(refcount<=0,'pending_reclaim','ready') WHERE object_id=?");
                        d.addBindValue(obj.toLongLong());
                        if (!d.exec()) qCWarning(lcDb) << "[normalizeLogicKeys] 收敛减引用失败" << obj.toLongLong() << d.lastError().text();
                    }
                    ++ufCollapsed;
                } else {
                    u.prepare("UPDATE user_file SET parent_path=? WHERE file_id=?");
                    u.addBindValue(key);
                    u.addBindValue(fid);
                    if (!u.exec()) { qCWarning(lcDb) << "[normalizeLogicKeys] 转换失败" << fid << u.lastError().text(); continue; }
                    seenUf.insert(triple);
                    ++ufConverted;
                }
            }
        }
    }

    // 2) upload_session：同判别（无唯一键，可改写则改写；与会话行同规则跳过 canonical）
    {
        QSqlQuery rows(db);
        if (rows.exec("SELECT session_id,owner_name,parent_path FROM upload_session "
                      "ORDER BY session_id")) {
            QVector<QVariantList> batch;
            while (rows.next()) {
                QVariantList r;
                for (int i = 0; i < 3; ++i) r.append(rows.value(i));
                batch.append(r);
            }
            for (const QVariantList& r : batch) {
                const QString key = FileObjectStore::normalizeParent(r.at(1).toString(), r.at(2).toString());
                if (key == r.at(2).toString()) continue;
                if (key.isEmpty()) { ++unmapped; continue; }
                QSqlQuery u(db);
                u.prepare("UPDATE upload_session SET parent_path=? WHERE session_id=?");
                u.addBindValue(key);
                u.addBindValue(r.at(0).toLongLong());
                if (u.exec()) ++sessConverted;
            }
        }
    }

    // 3) file_migration_map：机器绝对 old_path → "<filesys>/..." 相对键；
    //    转换后撞主键（两棵树映射同一旧文件）：按 migrated_at,old_path 序保留先者，删后者。
    {
        QVector<QString> legacy;
        QSqlQuery all(db);
        if (all.exec("SELECT old_path FROM file_migration_map ORDER BY migrated_at, old_path")) {
            while (all.next()) {
                const QString op = all.value(0).toString();
                if (!op.startsWith(fsBase + "/")) legacy.append(op);
            }
        }
        for (const QString& op : legacy) {
            QString n = op;
            n.replace('\\', '/');
            const QStringList segs = QDir::cleanPath(n).split('/', Qt::SkipEmptyParts);
            int idx = -1;
            for (int i = 0; i < segs.size(); ++i) {
                if (segs.at(i).compare(fsBase, Qt::CaseInsensitive) == 0) { idx = i; break; }
            }
            if (idx < 0) {
                ++unmapped;
                qCWarning(lcDb) << "[normalizeLogicKeys] 映射行锚定失败，保留原值" << op;
                continue;
            }
            const QString newKey = QStringList(segs.mid(idx)).join('/');
            if (newKey.size() > 191) { ++unmapped; continue; }
            QSqlQuery c(db);
            c.prepare("SELECT COUNT(*) FROM file_migration_map WHERE old_path=?");
            c.addBindValue(newKey);
            bool taken = false;
            if (c.exec() && c.next()) taken = (c.value(0).toLongLong() > 0);
            QSqlQuery u(db);
            if (taken) {
                u.prepare("DELETE FROM file_migration_map WHERE old_path=?");
                u.addBindValue(op);
                if (u.exec()) ++mapDupRemoved;
            } else {
                u.prepare("UPDATE file_migration_map SET old_path=? WHERE old_path=?");
                u.addBindValue(newKey);
                u.addBindValue(op);
                if (u.exec()) ++mapConverted;
            }
        }
    }

    if (ufConverted || ufCollapsed || ufSkippedDeleted || sessConverted || mapConverted || mapDupRemoved || unmapped) {
        qCDebug(lcDb) << "[normalizeLogicKeys] user_file" << ufConverted << "收敛" << ufCollapsed
                      << "deleted跳过" << ufSkippedDeleted
                      << "upload_session" << sessConverted
                      << "map" << mapConverted << "去重删" << mapDupRemoved
                      << "无法映射" << unmapped;
    }
}

// ===== REQ-EXT-FILE-001 T4：upload_session 持久化（staging 归属/offset/过期）=====
qint64 OperateDB::createUploadSession(const QString &owner, const QString &parentPath,
                                      const QString &displayName, qint64 expectedSize,
                                      const QString &clientSha256, const QString &stagingPath,
                                      qint64 confirmedOffset, int ttlSec,
                                      const QString &clientRequestId)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return -1;
    QSqlQuery q(connection.database());
    q.prepare("INSERT INTO upload_session (owner_name,parent_path,display_name,expected_size,"
              "client_sha256,staging_path,confirmed_offset,status,expires_at,client_request_id) "
              "VALUES (?,?,?,?,?,?,?,'uploading',?,?)");
    q.addBindValue(owner);
    q.addBindValue(parentPath);
    q.addBindValue(displayName);
    q.addBindValue(expectedSize);
    q.addBindValue(clientSha256.isEmpty() ? QVariant() : QVariant(clientSha256));
    q.addBindValue(stagingPath);
    q.addBindValue(confirmedOffset);
    q.addBindValue(QDateTime::currentDateTime().addSecs(ttlSec > 0 ? ttlSec : 3600)
                       .toString("yyyy-MM-dd HH:mm:ss"));
    q.addBindValue(clientRequestId.isEmpty() ? QVariant() : QVariant(clientRequestId));
    if(!q.exec()){
        qCWarning(lcDb) << "[createUploadSession] 失败:" << q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

// REQ-EXT-FILE-002 §2 步骤3：请求级幂等短路的读侧。
// 展示名优先取 user_file（服务端定名含 (1) 后缀），退化到会话登记的请求名。
OperateDB::UploadSessionLookup OperateDB::lookupUploadSessionByRequestId(
        const QString &owner, const QString &clientRequestId)
{
    UploadSessionLookup out;
    if (owner.isEmpty() || clientRequestId.isEmpty()) return out;
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) return out;
    QSqlQuery q(connection.database());
    q.prepare("SELECT s.session_id,s.status,s.confirmed_offset,s.file_id,s.display_name,"
              "       uf.display_name,s.parent_path,s.expected_size,s.client_sha256 "
              "FROM upload_session s LEFT JOIN user_file uf ON uf.file_id=s.file_id "
              "WHERE s.owner_name=? AND s.client_request_id=? FOR UPDATE");
    q.addBindValue(owner);
    q.addBindValue(clientRequestId);
    if (!q.exec() || !q.next()) return out;
    out.found = true;
    out.sessionId = q.value(0).toLongLong();
    out.status = q.value(1).toString();
    out.confirmedOffset = q.value(2).toLongLong();
    out.fileId = q.value(3).isNull() ? -1 : q.value(3).toLongLong();
    out.displayName = q.value(5).isNull() ? q.value(4).toString() : q.value(5).toString();
    out.parentPath = q.value(6).toString();
    out.expectedSize = q.value(7).toLongLong();
    out.clientSha256 = q.value(8).toString();
    return out;
}

// REQ-EXT-FILE-002 §2 步骤3：失败/过期会话的"同请求键重试"——原地重置，
// 绝不再插一行（uk_req 保证 (owner, client_request_id) 唯一）。
bool OperateDB::restartUploadSession(qint64 sessionId, const QString &parentPath,
                                     const QString &displayName, qint64 expectedSize,
                                     const QString &clientSha256, const QString &stagingPath,
                                     qint64 confirmedOffset, int ttlSec,
                                     const QString &clientRequestId)
{
    if (sessionId < 0) return false;
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    QString sql = "UPDATE upload_session SET parent_path=?,display_name=?,expected_size=?,"
                  "client_sha256=?,staging_path=?,confirmed_offset=?,status='uploading',"
                  "expires_at=?,file_id=NULL";
    if (!clientRequestId.isEmpty()) {
        sql += ",client_request_id=?";   // S14：按暂存键复用时把请求键改写到本行（任何 reqId 重放都收敛于此）
    }
    sql += " WHERE session_id=?";
    q.prepare(sql);
    q.addBindValue(parentPath);
    q.addBindValue(displayName);
    q.addBindValue(expectedSize);
    q.addBindValue(clientSha256.isEmpty() ? QVariant() : QVariant(clientSha256));
    q.addBindValue(stagingPath);
    q.addBindValue(confirmedOffset);
    q.addBindValue(QDateTime::currentDateTime().addSecs(ttlSec > 0 ? ttlSec : 3600)
                       .toString("yyyy-MM-dd HH:mm:ss"));
    if (!clientRequestId.isEmpty()) {
        q.addBindValue(clientRequestId);
    }
    q.addBindValue(sessionId);
    if (!q.exec()) {
        qCWarning(lcDb) << "[restartUploadSession] 失败:" << q.lastError().text();
        return false;
    }
    return true;
}

// REQ-EXT-TRANSFER-003 S12/D7：按 (owner, staging_path) 找可复用的未完成会话行。
// 暂存路径 = sha1(owner|parent|display|size) 的确定性产物 = "同一逻辑上传"的唯一身份；
// 状态白名单排除 done（已完成由请求级幂等短路处理），取 session_id 最大者。
qint64 OperateDB::findReusableUploadSession(const QString &owner, const QString &stagingPath)
{
    if (owner.isEmpty() || stagingPath.isEmpty()) return -1;
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) return -1;
    QSqlQuery q(connection.database());
    q.prepare("SELECT session_id FROM upload_session WHERE owner_name=? AND staging_path=? "
              "AND status IN ('uploading','expired','failed','verifying') "
              "ORDER BY session_id DESC LIMIT 1");
    q.addBindValue(owner);
    q.addBindValue(stagingPath);
    if (!q.exec() || !q.next()) return -1;
    return q.value(0).toLongLong();
}

// REQ-EXT-FILE-002 §2 步骤4：秒传命中即登记完成态会话（零正文传输）。
// staging_path 空串表示本次请求没有暂存实体（未落任何碎片文件）。
qint64 OperateDB::insertCompletedUploadSession(const QString &owner, const QString &parentPath,
                                              const QString &displayName, qint64 expectedSize,
                                              const QString &clientSha256,
                                              const QString &clientRequestId, qint64 fileId)
{
    if (owner.isEmpty() || clientRequestId.isEmpty() || fileId < 0) return -1;
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) return -1;
    QSqlQuery q(connection.database());
    q.prepare("INSERT INTO upload_session (owner_name,parent_path,display_name,expected_size,"
              "client_sha256,staging_path,confirmed_offset,status,expires_at,client_request_id,file_id) "
              "VALUES (?,?,?,?,?,'',?,'done',?,?,?)");
    q.addBindValue(owner);
    q.addBindValue(parentPath);
    q.addBindValue(displayName);
    q.addBindValue(expectedSize);
    q.addBindValue(clientSha256.isEmpty() ? QVariant() : QVariant(clientSha256));
    q.addBindValue(expectedSize);                        // confirmed_offset = size（已完成）
    q.addBindValue(QDateTime::currentDateTime().addSecs(3600).toString("yyyy-MM-dd HH:mm:ss"));
    q.addBindValue(clientRequestId);
    q.addBindValue(fileId);
    if (!q.exec()) {
        qCWarning(lcDb) << "[insertCompletedUploadSession] 失败:" << q.lastError().text();
        return -1;
    }
    return q.lastInsertId().toLongLong();
}

bool OperateDB::updateUploadOffset(qint64 sessionId, qint64 confirmedOffset)
{
    if(sessionId < 0) return false;
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("UPDATE upload_session SET confirmed_offset=? WHERE session_id=? AND status='uploading'");
    q.addBindValue(confirmedOffset);
    q.addBindValue(sessionId);
    return q.exec();
}

// REQ-EXT-TRANSFER-003 S10：confirmedOffset>=0 时同写 confirmed_offset——
// 完成路径写 expected_size（完成态不依赖节流），失败路径写 0（暂存已删，值无意义）。
bool OperateDB::finishUploadSession(qint64 sessionId, const QString &status, qint64 fileId,
                                    qint64 confirmedOffset)
{
    if(sessionId < 0) return false;
    if(status != "done" && status != "failed" && status != "expired" && status != "verifying")
        return false;
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    if (fileId >= 0 && confirmedOffset >= 0) {
        q.prepare("UPDATE upload_session SET status=?,file_id=?,confirmed_offset=? WHERE session_id=?");
        q.addBindValue(status);
        q.addBindValue(fileId);
        q.addBindValue(confirmedOffset);
    } else if (fileId >= 0) {
        q.prepare("UPDATE upload_session SET status=?,file_id=? WHERE session_id=?");
        q.addBindValue(status);
        q.addBindValue(fileId);
    } else if (confirmedOffset >= 0) {
        q.prepare("UPDATE upload_session SET status=?,confirmed_offset=? WHERE session_id=?");
        q.addBindValue(status);
        q.addBindValue(confirmedOffset);
    } else {
        q.prepare("UPDATE upload_session SET status=? WHERE session_id=?");
        q.addBindValue(status);
    }
    q.addBindValue(sessionId);
    return q.exec();
}

bool OperateDB::handleRegit(char *strname, char *strpwd)
{
    if(strname==nullptr||strpwd==nullptr){
        return false;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    q.prepare("select * from user_info where name = ?");
    q.addBindValue(QString::fromUtf8(strname));
    qCDebug(lcDb)<<"query user exist for:"<<QString::fromUtf8(strname);
    if(!q.exec()){
        qCWarning(lcDb)<<"query user exist failed:"<<q.lastError().text();
        return false;
    }
    if(q.next()){
        return false;
    }

    const QString salt = generatePasswordSalt();
    q.prepare("insert into user_info(name,pwd,salt) values(?,?,?)");
    q.addBindValue(QString::fromUtf8(strname));
    q.addBindValue(applyPasswordSalt(QString::fromUtf8(strpwd), salt));
    q.addBindValue(salt);
    qCDebug(lcDb)<<"insert new user:"<<QString::fromUtf8(strname);
    return q.exec();
}

bool OperateDB::createShareLink(const QString &hash, const QString &owner, const QString &path, const QDateTime &expire)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        qCWarning(lcDb) << "[createShareLink] 获取数据库连接失败（连接无效）";
        return false;
    }
    QSqlQuery q(connection.database());
    // 查重：同一 owner + 同一文件若已存在链接，则直接复用，避免重复记录堆积
    if(!getShareLinkByPath(owner, path).isEmpty()){
        qCDebug(lcDb) << "[createShareLink] 该文件已存在分享链接，复用已有记录: owner=" << owner << " path=" << path;
        return true;
    }
    q.prepare("INSERT INTO share_link (token_hash, owner_name, relative_path, expire_time) VALUES (?, ?, ?, ?)");
    q.addBindValue(hash);
    q.addBindValue(owner);
    q.addBindValue(path);
    // 必须显式格式化为 "yyyy-MM-dd HH:mm:ss" 字符串再绑定。
    // 直接绑定 QDateTime 时，Qt MySQL 驱动会序列化成 ISO 8601 格式（带 'T' 分隔符，如
    // 2026-07-29T18:00:00），若时间含毫秒/时区，MySQL 可能拒绝或存错。
    // Qt 5.14 的 QMYSQL 驱动在绑定 QDateTime 时会输出 ISO 格式（带 T / 毫秒），
    // MySQL 的 DATETIME 在严格模式下会拒绝带 T 的值，导致插入失败。
    // 这里显式格式化为 MySQL 原生 DATETIME 字符串，避免该问题。
    q.addBindValue(expire.toString("yyyy-MM-dd HH:mm:ss"));
    if(!q.exec()){
        qCWarning(lcDb) << "[createShareLink] 插入分享链接失败:" << q.lastError().text()
                 << " | hash=" << hash << " owner=" << owner << " path=" << path;
        return false;
    }
    return true;
}

bool OperateDB::getShareLinkByHash(const QString &hash, ShareLinkInfo &outInfo)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("SELECT owner_name, relative_path, expire_time, revoked FROM share_link WHERE token_hash = ?");
    q.addBindValue(hash);
    if(q.exec() && q.next()){
        outInfo.ownerName = q.value(0).toString();
        outInfo.relativePath = q.value(1).toString();
        outInfo.expireTime = q.value(2).toDateTime();
        outInfo.revoked = q.value(3).toInt();
        return true;
    }
    return false;
}

QString OperateDB::getShareLinkByPath(const QString &owner, const QString &path)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return QString();
    QSqlQuery q(connection.database());
    // created_at 是秒精度，同秒两行时排序不稳定；以 id 兜底，保证"同一逻辑行"在
    // getShareLinkByPath / getShareLinkInfoByPath 两个入口上取值一致
    q.prepare("SELECT token_hash FROM share_link WHERE owner_name = ? AND relative_path = ? "
              "ORDER BY created_at DESC, id DESC LIMIT 1");
    q.addBindValue(owner);
    q.addBindValue(path);
    if(q.exec() && q.next()){
        return q.value(0).toString();
    }
    return QString();
}

// REQ-EXT-SHARE-001：按 owner+path 取最新一行的完整信息（getShareLinkByPath 只回 hash，
// 判断"有效/过期/撤销"还需要 expire_time 与 revoked）
bool OperateDB::getShareLinkInfoByPath(const QString &owner, const QString &path, ShareLinkInfo &outInfo)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("SELECT token_hash, expire_time, revoked FROM share_link "
              "WHERE owner_name = ? AND relative_path = ? ORDER BY created_at DESC, id DESC LIMIT 1");
    q.addBindValue(owner);
    q.addBindValue(path);
    if(q.exec() && q.next()){
        outInfo.ownerName = owner;
        outInfo.relativePath = path;
        outInfo.tokenHash = q.value(0).toString();
        outInfo.expireTime = q.value(1).toDateTime();
        outInfo.revoked = q.value(2).toInt() != 0;
        return true;
    }
    return false;
}

// REQ-EXT-SHARE-001：续期只延长 expire_time，token 与相对路径不动；
// 已撤销的行（revoked=1）不参与，续期不能让一条被作废的链接复活
bool OperateDB::renewShareLink(const QString &owner, const QString &path, const QDateTime &newExpire)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("UPDATE share_link SET expire_time = ? WHERE owner_name = ? AND relative_path = ? AND revoked = 0");
    q.addBindValue(newExpire.toString("yyyy-MM-dd HH:mm:ss"));
    q.addBindValue(owner);
    q.addBindValue(path);
    if(!q.exec()){
        qCWarning(lcDb) << "[renewShareLink] 续期失败:" << q.lastError().text()
                        << " owner=" << owner << " path=" << path;
        return false;
    }
    return q.numRowsAffected() > 0;
}

// REQ-EXT-SHARE-001：重新生成前作废旧行（精确路径，不含 path/ 子路径）
bool OperateDB::removeShareLinkExact(const QString &owner, const QString &path)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("DELETE FROM share_link WHERE owner_name = ? AND relative_path = ?");
    q.addBindValue(owner);
    q.addBindValue(path);
    if(!q.exec()){
        qCWarning(lcDb) << "[removeShareLinkExact] 作废旧链接失败:" << q.lastError().text()
                        << " owner=" << owner << " path=" << path;
        return false;
    }
    return true;
}

// 跨功能联动：删除文件/目录时，清理对应的分享链接记录
// path 为相对 owner 根目录的路径；若为目录则同时清理其下所有文件（前缀匹配）
bool OperateDB::deleteShareLinkByPath(const QString &owner, const QString &path)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("DELETE FROM share_link WHERE owner_name = ? AND (relative_path = ? OR relative_path LIKE ?)");
    q.addBindValue(owner);
    q.addBindValue(path);
    q.addBindValue(path + "/%");
    bool ok = q.exec();
    if(!ok){
        qCWarning(lcDb) << "[deleteShareLinkByPath] 失败:" << q.lastError().text()
                 << " owner=" << owner << " path=" << path;
    }
    return ok;
}

// 跨功能联动：重命名/移动文件时，平移 share_link.relative_path
// - 精确匹配 oldPath（文件自身）
// - 前缀匹配 oldPath/（目录下的子文件整体平移）
bool OperateDB::updateShareLinkPath(const QString &owner, const QString &oldPath, const QString &newPath)
{
    if(oldPath == newPath) return true;
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlDatabase db = connection.database();

    // 1) 文件自身：relative_path = oldPath → newPath
    QSqlQuery q1(db);
    q1.prepare("UPDATE share_link SET relative_path = ? WHERE owner_name = ? AND relative_path = ?");
    q1.addBindValue(newPath);
    q1.addBindValue(owner);
    q1.addBindValue(oldPath);
    bool ok = q1.exec();
    if(!ok){
        qCWarning(lcDb) << "[updateShareLinkPath] 精确更新失败:" << q1.lastError().text()
                 << " owner=" << owner << " old=" << oldPath << " new=" << newPath;
    }

    // 2) 目录子文件：oldPath/sub → newPath/sub（仅匹配严格长于 oldPath 的条目）
    if(!oldPath.isEmpty()){
        QSqlQuery q2(db);
        q2.prepare("UPDATE share_link SET relative_path = CONCAT(?, SUBSTRING(relative_path, ?)) "
                   "WHERE owner_name = ? AND relative_path LIKE ? AND relative_path <> ?");
        q2.addBindValue(newPath + "/");
        q2.addBindValue(oldPath.size() + 2);          // 1-based：跳过 "oldPath/" 之后
        q2.addBindValue(owner);
        q2.addBindValue(oldPath + "/%");
        q2.addBindValue(oldPath);
        if(!q2.exec()){
            qCWarning(lcDb) << "[updateShareLinkPath] 目录前缀平移失败:" << q2.lastError().text();
            ok = false;
        }
    }
    return ok;
}

// 跨功能联动：删除好友时，清理两人互发的私聊记录
bool OperateDB::deleteFriendMessages(const QString &userA, const QString &userB)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("DELETE FROM message_info WHERE (seed_name = ? AND rev_name = ?) OR (seed_name = ? AND rev_name = ?)");
    q.addBindValue(userA);
    q.addBindValue(userB);
    q.addBindValue(userB);
    q.addBindValue(userA);
    bool ok = q.exec();
    if(!ok){
        qCWarning(lcDb) << "[deleteFriendMessages] 失败:" << q.lastError().text()
                 << " a=" << userA << " b=" << userB;
    }
    return ok;
}

int OperateDB::handleLogin(char *strname, char *strpwd, const QString& clientId, quint64& outVersion)
{
    outVersion = 0;
    if(strname==nullptr||strpwd==nullptr){
        return 0;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return 0;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    QString nameParam = QString::fromUtf8(strname);
    const QString clientHash = QString::fromUtf8(strpwd);

    // 按用户名取存储哈希与盐（salt 为空 = 迁移前老数据）
    q.prepare("SELECT pwd, salt FROM user_info WHERE name = ?");
    q.addBindValue(nameParam);
    qCDebug(lcDb)<<"login select for:"<<nameParam;
    if(!q.exec()){
        qCWarning(lcDb)<<"login select failed:"<<q.lastError().text();
        return 0;
    }
    if(!q.next()){
        qCWarning(lcDb)<<"user name or password not found";
        return -1;
    }
    const QString storedPwd = q.value(0).toString();
    const QString salt = q.value(1).toString();
    QString effectiveHash = applyPasswordSalt(clientHash, salt);
    if(effectiveHash != storedPwd){
        qCWarning(lcDb)<<"user name or password not found";
        return -1;
    }

    // 老用户登录成功 → 自动补盐升级（条件更新，不覆盖并发下已写入的新盐）
    if(salt.isEmpty()){
        QSqlQuery upgrade(db);
        const QString newSalt = generatePasswordSalt();
        const QString newHash = applyPasswordSalt(clientHash, newSalt);
        upgrade.prepare("UPDATE user_info SET pwd = ?, salt = ? WHERE name = ? AND salt = ''");
        upgrade.addBindValue(newHash);
        upgrade.addBindValue(newSalt);
        upgrade.addBindValue(nameParam);
        if(upgrade.exec() && upgrade.numRowsAffected() > 0){
            effectiveHash = newHash;
        }
    }

    // ===== REQ-EXT-SESSION-001：登录版本原子分配 + 登录档案 + 在线置位，同一事务 =====
    // LAST_INSERT_ID(login_version+1)：行锁串行化并发登录，版本号唯一单调；禁止读后加一。
    if(!db.transaction()){
        qCCritical(lcDb)<<"login transaction begin failed:"<<db.lastError().text();
        return 0;
    }
    QSqlQuery vq(db);
    vq.prepare("UPDATE user_info SET login_version = LAST_INSERT_ID(login_version + 1) WHERE name = ?");
    vq.addBindValue(nameParam);
    if(!vq.exec() || vq.numRowsAffected() != 1){
        qCCritical(lcDb)<<"login version bump failed:"<<vq.lastError().text();
        db.rollback();
        return 0;
    }
    QSqlQuery lq(db);
    if(!lq.exec("SELECT LAST_INSERT_ID()") || !lq.next()){
        qCCritical(lcDb)<<"login version readback failed:"<<lq.lastError().text();
        db.rollback();
        return 0;
    }
    outVersion = lq.value(0).toULongLong();

    QSqlQuery rq(db);
    rq.prepare("INSERT INTO login_record(username, client_id, login_version, login_time) VALUES(?,?,?,NOW())");
    rq.addBindValue(nameParam);
    rq.addBindValue(clientId);
    rq.addBindValue(static_cast<qlonglong>(outVersion));
    if(!rq.exec()){
        qCCritical(lcDb)<<"login record insert failed:"<<rq.lastError().text();
        db.rollback();
        outVersion = 0;
        return 0;
    }

    QSqlQuery oq(db);
    oq.prepare("update user_info set online = 1 where name = ? and pwd = ?");
    oq.addBindValue(nameParam);
    oq.addBindValue(effectiveHash);
    qCDebug(lcDb)<<"login update online for:"<<nameParam;
    if (!oq.exec()) {
        qCCritical(lcDb) << "UPDATE online status failed:" << oq.lastError();
        db.rollback();
        outVersion = 0;
        return 0;
    }

    if(!db.commit()){
        qCCritical(lcDb)<<"login transaction commit failed:"<<db.lastError().text();
        db.rollback();
        outVersion = 0;
        return 0;
    }
    qCDebug(lcDb)<<"[LOGINVER] user="<<nameParam<<" ver="<<outVersion<<" client_id="<<clientId;
    return 1;
}

void OperateDB::handleOffLine(const char *strname)
{
    if(strname==nullptr){
        return ;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return ;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    q.prepare("update user_info set online = 0 where name = ?");
    q.addBindValue(QString::fromUtf8(strname));
    qCDebug(lcDb)<<"offline update for:"<<QString::fromUtf8(strname);
    q.exec();
}

int OperateDB::handleFindUser(const char *userName)
{
    if(userName==nullptr){
        return -1;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return -1;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    q.prepare("select * from user_info where name = ?");
    q.addBindValue(QString::fromUtf8(userName));
    qCDebug(lcDb)<<"find user:"<<QString::fromUtf8(userName);
    q.exec();
    if(q.next()){
        return q.value(3).toInt();
    }
    return 2;
}

QStringList OperateDB::handleOnlineUsers(const QString &strname)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return QStringList();
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    // 查询在线且不是自己，且不在好友表中的用户
    q.prepare(R"(SELECT name FROM user_info 
                 WHERE online = 1 AND name != ? 
                 AND name NOT IN (
                     SELECT friendName FROM friend WHERE name = ?
                     UNION
                     SELECT name FROM friend WHERE friendName = ?
                 ))");
    q.addBindValue(strname);
    q.addBindValue(strname);
    q.addBindValue(strname);
    qCDebug(lcDb)<<"online users query for:"<<strname;
    q.exec();

    QStringList ret;
    while(q.next())
    {
        ret.append(q.value(0).toString());
    }
    return ret;
}

int OperateDB::handleAddFriend(char *usrname, char *tarname)
{
    if(usrname==nullptr||tarname==nullptr){
        return -1;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return -1;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    q.prepare("select * from user_info where name = ? and online = 1");
    q.addBindValue(QString::fromUtf8(tarname));
    q.exec();
    if(q.next()){
        return 1;
    }
    return 0;
}

int OperateDB::handleAddFriendFin(char *usrname, char *tarname)
{
    if(usrname==nullptr||tarname==nullptr){
        return -1;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return -1;
    }
    QSqlDatabase db = connection.database();
    
    // 开启事务
    db.transaction();
    QSqlQuery q(db);

    // 使用两个名字字段存储好友关系
    q.prepare("INSERT INTO friend (name, friendName) VALUES (?, ?)");
    q.addBindValue(QString::fromUtf8(usrname));
    q.addBindValue(QString::fromUtf8(tarname));
    if(!q.exec()) {
        db.rollback();
        return 0;
    }

    if(!db.commit()) {
        db.rollback();
        return 0;
    }
    
    qCDebug(lcDb)<<"AddFriendFin:"<<QString::fromUtf8(usrname)<<"<->"<<QString::fromUtf8(tarname);
    return 1;
}

QStringList OperateDB::handleFlushFriend(const QString &strname)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return QStringList();
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    // 查询 name=strname 时的 friendName，以及 friendName=strname 时的 name
    q.prepare(R"(SELECT friendName FROM friend WHERE name = ? 
                 UNION 
                 SELECT name FROM friend WHERE friendName = ?)");
    q.addBindValue(strname);
    q.addBindValue(strname);
    q.exec();

    QStringList ret;
    while(q.next()){
        ret.append(q.value(0).toString());
    }
    return ret;
}

bool OperateDB::handleDeleteFriend(char *userName, char *tarName)
{
    if(userName==nullptr||tarName==nullptr){
        return 0;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QSqlDatabase db = connection.database();
    
    // 开启事务
    db.transaction();
    QSqlQuery q(db);

    // 删除双向好友记录
    q.prepare("DELETE FROM friend WHERE (name = ? AND friendName = ?) OR (name = ? AND friendName = ?)");
    q.addBindValue(QString::fromUtf8(userName));
    q.addBindValue(QString::fromUtf8(tarName));
    q.addBindValue(QString::fromUtf8(tarName));
    q.addBindValue(QString::fromUtf8(userName));
    if(!q.exec()) {
        db.rollback();
        return false;
    }

    if(!db.commit()) {
        db.rollback();
        return false;
    }
    
    qCDebug(lcDb)<<"deletefriend:"<<QString::fromUtf8(userName)<<"-"<<QString::fromUtf8(tarName);
    return true;
}

bool OperateDB::seedchatMsg(char *seedName, char *revName, QString msg, QString seedTime)
{
    if(seedName==nullptr||revName==nullptr){
        return 0;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    q.prepare("select * from user_info where name = ? and online = 1");
    q.addBindValue(QString::fromUtf8(revName));
    bool ans=1;
    ans&=q.exec();
    ans&=q.next();
    qCDebug(lcDb)<<"select online ans:"<<ans<<" for:"<<QString::fromUtf8(revName);

    q.prepare(R"(insert into message_info(seed_name,rev_name,msg,issuccess,seed_time)
        values(?,?,?,?,?))");
    q.addBindValue(QString::fromUtf8(seedName));
    q.addBindValue(QString::fromUtf8(revName));
    q.addBindValue(msg);
    q.addBindValue(ans ? 1 : 0);
    q.addBindValue(seedTime);
    bool execRet=q.exec();
    qCDebug(lcDb)<<"chat msg insert:"<<execRet;
    return ans;
}

QStringList OperateDB::getchatMsg(char *seedName, char *revName)
{
    QStringList ret;
    if(seedName==nullptr||revName==nullptr){
        return ret;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return ret;
    }
    QSqlDatabase db = connection.database();

    // 单次事务内完成"读取未送达 + 标记已送达"：并发拉取不会重复读到同一批行。
    if(!db.transaction()){
        qCWarning(lcDb)<<"getchatMsg begin transaction failed:"<<db.lastError().text();
        return ret;
    }

    // FOR UPDATE：锁定本次将返回的行，使并发拉取在读到相同行之前被串行化。
    QSqlQuery q(db);
    q.prepare("select id,msg,seed_time from message_info "
              "where seed_name = ? and rev_name = ? and issuccess = 0 for update");
    q.addBindValue(QString::fromUtf8(seedName));
    q.addBindValue(QString::fromUtf8(revName));
    qCDebug(lcDb)<<"getchatMsg for:"<<QString::fromUtf8(seedName)<<"->"<<QString::fromUtf8(revName);
    if(!q.exec()){
        qCWarning(lcDb)<<"getchatMsg select failed:"<<q.lastError().text();
        db.rollback();
        return ret;
    }
    QVector<qlonglong> ids;
    while(q.next()){
        ids.append(q.value(0).toLongLong());
        ret.append(q.value(1).toString());   // msg
        ret.append(q.value(2).toString());   // seed_time
    }
    q.finish();   // 结束迭代，避免结果集状态影响后续语句

    if(!ids.isEmpty()){
        QStringList placeholders;
        for(int i=0;i<ids.size();++i){
            placeholders.append(QStringLiteral("?"));
        }
        // 独立 QSqlQuery，仅按本次返回的主键更新，并保留 issuccess=0 条件：
        // 只标记"确实已返回"的行，事务提交前并发到达的新行不会被误标。
        QSqlQuery upd(db);
        upd.prepare(QStringLiteral("update message_info set issuccess = 1 "
                                   "where id in (%1) and issuccess = 0")
                        .arg(placeholders.join(QLatin1Char(','))));
        for(int i=0;i<ids.size();++i){
            upd.addBindValue(ids.at(i));
        }
        if(!upd.exec()){
            qCWarning(lcDb)<<"getchatMsg update issuccess=1 failed:"<<upd.lastError().text();
            db.rollback();
            return QStringList();   // 未能标记已送达则不返回，宁可下次重投也不重复下发
        }
        qCDebug(lcDb)<<"getchatMsg update issuccess=1 rows:"<<upd.numRowsAffected();
    }

    if(!db.commit()){
        qCWarning(lcDb)<<"getchatMsg commit failed:"<<db.lastError().text();
        db.rollback();
        return QStringList();
    }
    return ret;
}

bool OperateDB::handleGetUserInfo(const char *userName, QString &outName, int &outOnline)
{
    if(userName==nullptr){
        return false;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);
    //使用预编译查询防止 SQL 注入
    q.prepare("select name, online from user_info where name = ?");
    q.addBindValue(QString::fromUtf8(userName));
    if(!q.exec() || !q.next()){
        return false;
    }
    outName = q.value(0).toString();
    outOnline = q.value(1).toInt();
    return true;
}

bool OperateDB::handleLogoff(const char* name, const char* pwd)
{
    if(name==nullptr||pwd==nullptr){
        return false;
    }

    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()){
        return false;
    }
    QSqlDatabase db = connection.database();

    db.transaction();
    QSqlQuery q(db);

    // 1. 按用户名取哈希与盐后校验密码（salt 为空 = 迁移前老数据，按裸哈希比对）
    q.prepare("SELECT pwd, salt FROM user_info WHERE name = ?");
    q.addBindValue(QString::fromUtf8(name));
    if(!q.exec() || !q.next()){
        qCWarning(lcDb)<<"logoff: user/pwd mismatch or user not found:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }
    const QString effectiveHash = applyPasswordSalt(QString::fromUtf8(pwd), q.value(1).toString());
    if(effectiveHash != q.value(0).toString()){
        qCWarning(lcDb)<<"logoff: user/pwd mismatch or user not found:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }

    // 2. 删除 message_info 中涉及该用户的记录
    q.prepare("DELETE FROM message_info WHERE seed_name = ? OR rev_name = ?");
    q.addBindValue(QString::fromUtf8(name));
    q.addBindValue(QString::fromUtf8(name));
    if(!q.exec()){
        qCWarning(lcDb)<<"logoff: failed to delete messages for:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }

    // 3. 删除 friend 表中涉及该用户的所有记录（双向）
    q.prepare("DELETE FROM friend WHERE name = ? OR friendName = ?");
    q.addBindValue(QString::fromUtf8(name));
    q.addBindValue(QString::fromUtf8(name));
    if(!q.exec()){
        qCWarning(lcDb)<<"logoff: failed to delete friend records for:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }

    // 4. 删除 user_info 表中的用户记录
    q.prepare("DELETE FROM user_info WHERE name = ? AND pwd = ?");
    q.addBindValue(QString::fromUtf8(name));
    q.addBindValue(effectiveHash);
    if(!q.exec()){
        qCWarning(lcDb)<<"logoff: failed to delete user_info for:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }

    // 3.5 — 删除 share_link 表
    q.prepare("DELETE FROM share_link WHERE owner_name = ?");
    q.addBindValue(QString::fromUtf8(name));
    if(!q.exec()){
        qCDebug(lcDb)<<"logoff: warning - failed to delete share_links (might be empty):"<<QString::fromUtf8(name);
        // 不回滚，视为成功
    }

    // 3.6 — 删除 publish_links 表（既有表，补充清理）
    q.prepare("DELETE FROM publish_links WHERE creator_name = ?");
    q.addBindValue(QString::fromUtf8(name));
    if(!q.exec()){
        qCDebug(lcDb)<<"logoff: warning - failed to delete publish_links (might be empty):"<<QString::fromUtf8(name);
        // 不回滚，视为成功
    }

    // 3.7 — REQ-EXT-SESSION-001：清理 login_record 档案（表未迁移时同样视为成功）
    q.prepare("DELETE FROM login_record WHERE username = ?");
    q.addBindValue(QString::fromUtf8(name));
    if(!q.exec()){
        qCDebug(lcDb)<<"logoff: warning - failed to delete login_records (might be empty):"<<QString::fromUtf8(name);
        // 不回滚，视为成功
    }

    if(!db.commit()){
        qCWarning(lcDb)<<"logoff: commit failed for:"<<QString::fromUtf8(name);
        db.rollback();
        return false;
    }

    qCDebug(lcDb)<<"logoff: successfully deleted user:"<<QString::fromUtf8(name);
    return true;
}

bool OperateDB::createPublishLink(const QString &code, const QString &path, const QString &creator)
{
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if(!connection.isValid()) return false;
    QSqlQuery q(connection.database());
    q.prepare("INSERT INTO publish_links (short_code, file_path, creator_name) VALUES (?, ?, ?)");
    q.addBindValue(code);
    q.addBindValue(path);
    q.addBindValue(creator);
    return q.exec();
}

namespace {
OperateDB::GroupResult groupFailure(int code)
{
    OperateDB::GroupResult result;
    result.code = code;
    result.groupId = 0;
    result.version = 0;
    result.eventType = 0;
    return result;
}

bool groupIsValidName(const QString &value, int maxLength)
{
    return !value.isEmpty() && value.size() <= maxLength && !value.contains(QChar('\0'))
            && !value.contains(QRegExp("[\\r\\n\\t]"));
}

QString groupRole(QSqlDatabase &db, quint64 groupId, const QString &actor)
{
    QSqlQuery q(db);
    q.prepare("SELECT role FROM group_member WHERE group_id=? AND user_name=? AND status='active'");
    q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
    q.addBindValue(actor);
    return q.exec() && q.next() ? q.value(0).toString() : QString();
}

QStringList groupActiveUsers(QSqlDatabase &db, quint64 groupId)
{
    QStringList users;
    QSqlQuery q(db);
    q.prepare("SELECT user_name FROM group_member WHERE group_id=? AND status='active'");
    q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
    if (q.exec()) {
        while (q.next()) users.append(q.value(0).toString());
    }
    return users;
}

void groupAppendRecord(QByteArray &body, quint64 groupId, const QString &groupName,
                       const QString &status, const QString &role, quint64 version)
{
    groupAppendU64(body, groupId);
    groupAppendString(body, groupName.toUtf8());
    groupAppendString(body, status.toUtf8());
    groupAppendString(body, role.toUtf8());
    groupAppendU64(body, version);
}

// REQ-EXT-GROUP-002 T6：成员事件系统消息——调用方已在同一事务内，本函数沿用同一
// 序号通道（group_info.last_seq 原子递增）追加一条 msg_type='system' 行。
// 返回 false 时调用方必须回滚；code 为业务失败码（-2105 序号分配/入库失败）。
bool groupAppendSystemMessage(QSqlDatabase &db, quint64 groupId, const QString &sender,
                              const QString &content, OperateDB::GroupMessageRecord &out, int &code)
{
    QSqlQuery q(db);
    q.prepare("UPDATE group_info SET last_seq=LAST_INSERT_ID(last_seq+1) WHERE group_id=?");
    q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
    if (!q.exec() || q.numRowsAffected() != 1) { code = -2105; return false; }
    q.prepare("SELECT LAST_INSERT_ID()");
    if (!q.exec() || !q.next()) { code = -2105; return false; }
    const quint64 seq = q.value(0).toULongLong();
    if (seq == 0) { code = -2105; return false; }
    q.prepare("INSERT INTO group_message(group_id,seq,client_msg_id,sender_name,msg_type,content) "
              "VALUES (?,?,NULL,?,'system',?)");
    q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
    q.addBindValue(QVariant::fromValue<qulonglong>(seq));
    q.addBindValue(sender);
    q.addBindValue(content);
    if (!q.exec()) { code = -2105; return false; }
    const quint64 msgId = q.lastInsertId().toULongLong();
    q.prepare("SELECT ROUND(UNIX_TIMESTAMP(created_at)*1000) FROM group_message WHERE msg_id=?");
    q.addBindValue(QVariant::fromValue<qulonglong>(msgId));
    if (!q.exec() || !q.next()) { code = -2105; return false; }
    out.seq = seq;
    out.msgId = msgId;
    out.sender = sender;
    out.msgType = 1;
    out.createdAtMs = q.value(0).toULongLong();
    out.content = content;
    return true;
}
}

OperateDB::GroupResult OperateDB::handleGroupOperation(const GroupRequest &request,
                                                       const QString &actor)
{
    if (actor.isEmpty()) return groupFailure(-1001); // 未认证主体
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) return groupFailure(-1002); // 数据库不可达
    QSqlDatabase db = connection.database();
    GroupResult result = groupFailure(-1000);
    QSqlQuery q(db);

    if (request.operation == GroupCreate) {
        if (!groupIsValidName(request.groupName, 128)) return groupFailure(-1003);
        if (!db.transaction()) return groupFailure(-1004);
        q.prepare("INSERT INTO group_info (group_name, creator_name, status, version) VALUES (?, ?, 'active', 1)");
        q.addBindValue(request.groupName);
        q.addBindValue(actor);
        if (!q.exec()) { db.rollback(); return groupFailure(-1005); }
        const quint64 groupId = q.lastInsertId().toULongLong();
        q.prepare("INSERT INTO group_member (group_id,user_name,role,status) VALUES (?,?,'owner','active')");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
        q.addBindValue(actor);
        if (!q.exec() || !db.commit()) { db.rollback(); return groupFailure(-1006); }
        result = groupFailure(0);
        result.groupId = groupId;
        result.version = 1;
        result.notifyUsers << actor;
        result.eventType = 1;
        groupAppendU64(result.responseBody, groupId);
        return result;
    }

    if (request.operation == GroupList) {
        q.prepare("SELECT gi.group_id,gi.group_name,gi.status,gm.role,gi.version "
                  "FROM group_member gm JOIN group_info gi ON gi.group_id=gm.group_id "
                  "WHERE gm.user_name=? AND gm.status='active' ORDER BY gi.updated_at DESC");
        q.addBindValue(actor);
        if (!q.exec()) return groupFailure(-1007);
        QByteArray body;
        QList<QByteArray> rows;
        while (q.next()) {
            QByteArray row;
            groupAppendRecord(row, q.value(0).toULongLong(), q.value(1).toString(),
                              q.value(2).toString(), q.value(3).toString(), q.value(4).toULongLong());
            rows.append(row);
        }
        groupAppendU32(body, quint32(rows.size()));
        for (const QByteArray &row : rows) body.append(row);
        result = groupFailure(0);
        result.responseBody = body;
        return result;
    }

    if (request.operation == GroupQueryInvitations) {
        q.prepare("SELECT i.invitation_id,i.group_id,gi.group_name,i.inviter_name,i.status,"
                  "UNIX_TIMESTAMP(i.expires_at) FROM group_invitation i "
                  "JOIN group_info gi ON gi.group_id=i.group_id "
                  "WHERE i.target_name=? AND i.status='pending' ORDER BY i.created_at DESC");
        q.addBindValue(actor);
        if (!q.exec()) return groupFailure(-1008);
        QList<QByteArray> rows;
        while (q.next()) {
            QByteArray row;
            groupAppendU64(row, q.value(0).toULongLong());
            groupAppendU64(row, q.value(1).toULongLong());
            groupAppendString(row, q.value(2).toString().toUtf8());
            groupAppendString(row, q.value(3).toString().toUtf8());
            groupAppendString(row, q.value(4).toString().toUtf8());
            groupAppendU64(row, q.value(5).toULongLong());
            rows.append(row);
        }
        QByteArray body;
        groupAppendU32(body, quint32(rows.size()));
        for (const QByteArray &row : rows) body.append(row);
        result = groupFailure(0);
        result.responseBody = body;
        return result;
    }

    // REQ-EXT-GROUP-002：群消息操作走独立子路径（发送/拉取/已读/未读摘要）
    if (request.operation == GroupSendMessage || request.operation == GroupPullMessages
            || request.operation == GroupReadConfirm || request.operation == GroupUnreadSummary) {
        return handleGroupMessageOperation(request, actor);
    }

    if (request.groupId == 0 && request.operation != GroupAcceptInvitation
            && request.operation != GroupRejectInvitation) return groupFailure(-1009);

    quint64 groupId = request.groupId;
    if (request.operation == GroupAcceptInvitation || request.operation == GroupRejectInvitation) {
        q.prepare("SELECT group_id FROM group_invitation WHERE invitation_id=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.invitationId));
        if (!q.exec() || !q.next()) return groupFailure(-1010);
        groupId = q.value(0).toULongLong();
    }

    if (request.operation == GroupListMembers || request.operation == GroupSummary) {
        q.prepare("SELECT status FROM group_info WHERE group_id=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
        if (!q.exec() || !q.next()) return groupFailure(-1011);
        const QString groupStatus = q.value(0).toString();
        if (groupRole(db, groupId, actor).isEmpty()) return groupFailure(-1012);
        if (request.operation == GroupListMembers) {
            q.prepare("SELECT user_name,role,status FROM group_member WHERE group_id=? AND status='active' ORDER BY joined_at");
            q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
            if (!q.exec()) return groupFailure(-1013);
            QList<QByteArray> rows;
            while (q.next()) {
                QByteArray row;
                groupAppendString(row, q.value(0).toString().toUtf8());
                groupAppendString(row, q.value(1).toString().toUtf8());
                groupAppendString(row, q.value(2).toString().toUtf8());
                rows.append(row);
            }
            QByteArray body;
            groupAppendU32(body, quint32(rows.size()));
            for (const QByteArray &row : rows) body.append(row);
            result = groupFailure(0);
            result.groupId = groupId;
            result.responseBody = body;
            return result;
        }
        q.prepare("SELECT group_name,status,version FROM group_info WHERE group_id=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
        if (!q.exec() || !q.next()) return groupFailure(-1014);
        const QString name = q.value(0).toString();
        const quint64 version = q.value(2).toULongLong();
        q.prepare("SELECT COUNT(*) FROM group_member WHERE group_id=? AND status='active'");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
        if (!q.exec() || !q.next()) return groupFailure(-1015);
        QByteArray body;
        groupAppendU64(body, groupId);
        groupAppendString(body, name.toUtf8());
        groupAppendString(body, groupStatus.toUtf8());
        groupAppendU64(body, version);
        groupAppendU32(body, q.value(0).toUInt());
        result = groupFailure(0);
        result.groupId = groupId;
        result.version = version;
        result.responseBody = body;
        return result;
    }

    if (!db.transaction()) return groupFailure(-1016);
    q.prepare("SELECT status,version FROM group_info WHERE group_id=? FOR UPDATE");
    q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
    if (!q.exec() || !q.next()) { db.rollback(); return groupFailure(-1017); }
    const QString groupStatus = q.value(0).toString();
    quint64 version = q.value(1).toULongLong();

    if (request.operation == GroupAcceptInvitation || request.operation == GroupRejectInvitation) {
        if (groupStatus != "active") { db.rollback(); return groupFailure(-1020); }
        q.prepare("SELECT target_name,status,expires_at,inviter_name FROM group_invitation WHERE invitation_id=? FOR UPDATE");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.invitationId));
        if (!q.exec() || !q.next()) { db.rollback(); return groupFailure(-1018); }
        if (q.value(0).toString() != actor || q.value(1).toString() != "pending") {
            db.rollback(); return groupFailure(-1019);
        }
        const bool expired = q.value(2).toDateTime() < QDateTime::currentDateTime();
        if (request.operation == GroupAcceptInvitation) {
            if (expired) { db.rollback(); return groupFailure(-1020); }
            q.prepare("SELECT status FROM group_member WHERE group_id=? AND user_name=? FOR UPDATE");
            q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(actor);
            if (!q.exec()) { db.rollback(); return groupFailure(-1021); }
            if (q.next()) {
                q.prepare("UPDATE group_member SET status='active',role='member',updated_at=CURRENT_TIMESTAMP "
                          "WHERE group_id=? AND user_name=?");
            } else {
                q.prepare("INSERT INTO group_member(group_id,user_name,role,status) VALUES (?,?,'member','active')");
            }
            q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(actor);
            if (!q.exec()) { db.rollback(); return groupFailure(-1022); }
            q.prepare("UPDATE group_invitation SET status='accepted',handled_at=NOW() WHERE invitation_id=?");
            q.addBindValue(QVariant::fromValue<qulonglong>(request.invitationId));
            if (!q.exec()) { db.rollback(); return groupFailure(-1023); }
            // REQ-EXT-GROUP-002 T6：加入事件在同一事务、同一序号通道追加系统消息
            GroupMessageRecord joinMsg;
            int sysCode = 0;
            if (!groupAppendSystemMessage(db, groupId, actor,
                                          actor + QStringLiteral(" 加入了群聊"), joinMsg, sysCode)) {
                db.rollback(); return groupFailure(sysCode);
            }
            ++version;
            q.prepare("UPDATE group_info SET version=? WHERE group_id=?");
            q.addBindValue(QVariant::fromValue<qulonglong>(version)); q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
            if (!q.exec() || !db.commit()) { db.rollback(); return groupFailure(-1024); }
            result = groupFailure(0); result.groupId = groupId; result.version = version;
            result.notifyUsers = groupActiveUsers(db, groupId); result.eventType = 1;
            result.message = joinMsg; result.broadcastMessage = true;
            groupAppendU64(result.responseBody, groupId);
            return result;
        }
        q.prepare("UPDATE group_invitation SET status=?,handled_at=NOW() WHERE invitation_id=?");
        q.addBindValue(expired ? "expired" : "rejected");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.invitationId));
        if (!q.exec() || !db.commit()) { db.rollback(); return groupFailure(-1025); }
        result = groupFailure(0); result.groupId = groupId; result.eventType = 2;
        return result;
    }

    const QString actorRole = groupRole(db, groupId, actor);
    if (actorRole.isEmpty()) { db.rollback(); return groupFailure(-1026); }
    if (groupStatus != "active") { db.rollback(); return groupFailure(-1027); }

    if (request.operation == GroupInvite) {
        if (actorRole != "owner" && actorRole != "admin") { db.rollback(); return groupFailure(-1028); }
        if (!groupIsValidName(request.targetName, 32) || !groupIsValidName(request.requestKey, 64)) { db.rollback(); return groupFailure(-1029); }
        q.prepare("SELECT 1 FROM user_info WHERE name=?"); q.addBindValue(request.targetName);
        if (!q.exec() || !q.next()) { db.rollback(); return groupFailure(-1030); }
        q.prepare("SELECT status FROM group_member WHERE group_id=? AND user_name=? FOR UPDATE");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(request.targetName);
        if (!q.exec()) { db.rollback(); return groupFailure(-1031); }
        if (q.next() && q.value(0).toString() == "active") { db.rollback(); return groupFailure(-1032); }
        q.prepare("SELECT invitation_id,status FROM group_invitation WHERE group_id=? AND request_key=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(request.requestKey);
        if (q.exec() && q.next()) {
            const quint64 invitationId = q.value(0).toULongLong();
            if (!db.commit()) { db.rollback(); return groupFailure(-1033); }
            result = groupFailure(0); result.groupId = groupId; result.eventType = 2; result.notifyUsers << request.targetName;
            groupAppendU64(result.responseBody, invitationId); return result;
        }
        q.prepare("SELECT 1 FROM group_invitation WHERE group_id=? AND target_name=? AND status='pending' FOR UPDATE");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(request.targetName);
        if (q.exec() && q.next()) { db.rollback(); return groupFailure(-1034); }
        q.prepare("INSERT INTO group_invitation(group_id,inviter_name,target_name,status,expires_at,request_key) "
                  "VALUES (?,?,?,'pending',DATE_ADD(NOW(),INTERVAL 7 DAY),?)");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(actor);
        q.addBindValue(request.targetName); q.addBindValue(request.requestKey);
        if (!q.exec() || !db.commit()) { db.rollback(); return groupFailure(-1035); }
        result = groupFailure(0); result.groupId = groupId; result.eventType = 2; result.notifyUsers << request.targetName;
        groupAppendU64(result.responseBody, q.lastInsertId().toULongLong()); return result;
    }

    GroupMessageRecord sysMsg;
    bool sysMsgPending = false;
    int sysCode = 0;
    if (request.operation == GroupLeave) {
        if (actorRole == "owner") { db.rollback(); return groupFailure(-1036); }
        q.prepare("UPDATE group_member SET status='left',updated_at=CURRENT_TIMESTAMP WHERE group_id=? AND user_name=? AND status='active'");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(actor);
        if (!q.exec() || q.numRowsAffected() != 1) { db.rollback(); return groupFailure(-1037); }
        // REQ-EXT-GROUP-002 T6：退出事件系统消息（同一事务/序号通道）
        if (!groupAppendSystemMessage(db, groupId, actor,
                                      actor + QStringLiteral(" 退出了群聊"), sysMsg, sysCode)) {
            db.rollback(); return groupFailure(sysCode);
        }
        sysMsgPending = true;
    } else if (request.operation == GroupRemoveMember) {
        if (actorRole != "owner" && actorRole != "admin") { db.rollback(); return groupFailure(-1038); }
        q.prepare("SELECT role,status FROM group_member WHERE group_id=? AND user_name=? FOR UPDATE");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(request.targetName);
        if (!q.exec() || !q.next() || q.value(1).toString() != "active" || q.value(0).toString() != "member") { db.rollback(); return groupFailure(-1039); }
        q.prepare("UPDATE group_member SET status='removed',updated_at=CURRENT_TIMESTAMP WHERE group_id=? AND user_name=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(request.targetName);
        if (!q.exec()) { db.rollback(); return groupFailure(-1040); }
        // REQ-EXT-GROUP-002 T6：移除事件系统消息（同一事务/序号通道）
        if (!groupAppendSystemMessage(db, groupId, actor,
                                      request.targetName + QStringLiteral(" 被移出群聊"), sysMsg, sysCode)) {
            db.rollback(); return groupFailure(sysCode);
        }
        sysMsgPending = true;
    } else if (request.operation == GroupSetRole) {
        if (actorRole != "owner" || (request.role != "admin" && request.role != "member")) { db.rollback(); return groupFailure(-1041); }
        q.prepare("UPDATE group_member SET role=?,updated_at=CURRENT_TIMESTAMP WHERE group_id=? AND user_name=? AND status='active' AND role<>'owner'");
        q.addBindValue(request.role); q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(request.targetName);
        if (!q.exec() || q.numRowsAffected() != 1) { db.rollback(); return groupFailure(-1042); }
    } else if (request.operation == GroupTransferOwner) {
        if (actorRole != "owner") { db.rollback(); return groupFailure(-1043); }
        q.prepare("UPDATE group_member SET role='owner',updated_at=CURRENT_TIMESTAMP WHERE group_id=? AND user_name=? AND status='active' AND role<>'owner'");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(request.targetName);
        if (!q.exec() || q.numRowsAffected() != 1) { db.rollback(); return groupFailure(-1044); }
        q.prepare("UPDATE group_member SET role='admin',updated_at=CURRENT_TIMESTAMP WHERE group_id=? AND user_name=? AND status='active' AND role='owner'");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId)); q.addBindValue(actor);
        if (!q.exec() || q.numRowsAffected() != 1) { db.rollback(); return groupFailure(-1045); }
    } else if (request.operation == GroupDissolve) {
        if (actorRole != "owner") { db.rollback(); return groupFailure(-1046); }
        q.prepare("UPDATE group_info SET status='dissolved' WHERE group_id=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
        if (!q.exec()) { db.rollback(); return groupFailure(-1047); }
    } else {
        db.rollback(); return groupFailure(-1048);
    }

    ++version;
    q.prepare("UPDATE group_info SET version=? WHERE group_id=?");
    q.addBindValue(QVariant::fromValue<qulonglong>(version)); q.addBindValue(QVariant::fromValue<qulonglong>(groupId));
    if (!q.exec() || !db.commit()) { db.rollback(); return groupFailure(-1049); }
    result = groupFailure(0);
    result.groupId = groupId;
    result.version = version;
    result.notifyUsers = groupActiveUsers(db, groupId);
    result.eventType = 1;
    if (sysMsgPending) { result.message = sysMsg; result.broadcastMessage = true; }
    groupAppendU64(result.responseBody, groupId);
    return result;
}

// ===== REQ-EXT-GROUP-002：群消息操作（发送/拉取/已读/未读摘要） =====
// 权限事实源与 GROUP-001 同源：group_info.status='active' + group_member.status='active'。
// 失败码段 -2xxx：-2101 非成员 / -2102 群不存在或已解散 / -2103 内容/幂等键非法 /
// -2104 幂等冲突异常 / -2105 序号分配或入库失败。不泄露他群存在性。
OperateDB::GroupResult OperateDB::handleGroupMessageOperation(const GroupRequest &request,
                                                              const QString &actor)
{
    if (actor.isEmpty()) return groupFailure(-1001); // 未认证主体
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) return groupFailure(-1002); // 数据库不可达
    QSqlDatabase db = connection.database();
    GroupResult result = groupFailure(-2000);
    QSqlQuery q(db);

    // ---- 未读摘要（95/96）：按 actor 汇总其全部 active 群，不需要 group_id ----
    if (request.operation == GroupUnreadSummary) {
        q.prepare("SELECT gm.group_id,gi.last_seq,gm.last_read_seq,"
                  "(SELECT COUNT(*) FROM group_message m WHERE m.group_id=gm.group_id "
                  " AND m.seq>gm.last_read_seq AND m.seq<=gi.last_seq) "
                  "FROM group_member gm JOIN group_info gi ON gi.group_id=gm.group_id "
                  "WHERE gm.user_name=? AND gm.status='active' AND gi.status='active' "
                  "ORDER BY gm.group_id");
        q.addBindValue(actor);
        if (!q.exec()) return groupFailure(-2105);
        QList<QByteArray> rows;
        while (q.next()) {
            QByteArray row;
            groupAppendU64(row, q.value(0).toULongLong());
            groupAppendU64(row, q.value(1).toULongLong());
            groupAppendU64(row, q.value(2).toULongLong());
            groupAppendU64(row, q.value(3).toULongLong());
            rows.append(row);
        }
        QByteArray body;
        groupAppendU32(body, quint32(rows.size()));
        for (const QByteArray &row : rows) body.append(row);
        result = groupFailure(0);
        result.responseBody = body;
        return result;
    }

    // 以下三类操作必须携带有效 group_id
    if (request.groupId == 0) return groupFailure(-2102);

    if (request.operation == GroupSendMessage) {
        // 服务端权威校验：内容非空且 <=2000B UTF-8；幂等键非空且 <=64B。
        // 载荷无 sender 字段——发送者一律取 actor。
        const QByteArray contentUtf8 = request.content.toUtf8();
        const QByteArray clientKeyUtf8 = request.clientMsgId.toUtf8();
        if (contentUtf8.isEmpty() || contentUtf8.size() > 2000
                || clientKeyUtf8.isEmpty() || clientKeyUtf8.size() > 64) {
            return groupFailure(-2103);
        }
        if (!db.transaction()) return groupFailure(-2105);
        // 群行锁：串行化同群发送，序号不重复、不倒退由行锁保证，uk_group_seq 兜底
        q.prepare("SELECT status,last_seq FROM group_info WHERE group_id=? FOR UPDATE");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        if (!q.exec() || !q.next() || q.value(0).toString() != "active") {
            db.rollback(); return groupFailure(-2102);
        }
        // 成员门：非成员/已退出/被移除即时失效
        q.prepare("SELECT role FROM group_member WHERE group_id=? AND user_name=? AND status='active' FOR UPDATE");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(actor);
        if (!q.exec() || !q.next()) { db.rollback(); return groupFailure(-2101); }
        // 幂等短路：同 client_msg_id 已存在 → 返回既有消息；REPEATABLE READ 下 gap 锁
        // 使并发同键重试串行，后到事务命中本分支。
        q.prepare("SELECT seq,msg_id,sender_name,msg_type,ROUND(UNIX_TIMESTAMP(created_at)*1000),content "
                  "FROM group_message WHERE group_id=? AND client_msg_id=? FOR UPDATE");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(request.clientMsgId);
        if (!q.exec()) { db.rollback(); return groupFailure(-2105); }
        if (q.next()) {
            GroupMessageRecord existing;
            existing.seq = q.value(0).toULongLong();
            existing.msgId = q.value(1).toULongLong();
            existing.sender = q.value(2).toString();
            existing.msgType = quint8(q.value(3).toString() == "system" ? 1 : 0);
            existing.createdAtMs = q.value(4).toULongLong();
            existing.content = q.value(5).toString();
            if (existing.sender != actor) { db.rollback(); return groupFailure(-2104); }
            if (!db.commit()) { db.rollback(); return groupFailure(-2104); }
            result = groupFailure(0);
            result.groupId = request.groupId;
            result.message = existing;
            result.broadcastMessage = false; // 重放不重复广播
            groupAppendString(result.responseBody, clientKeyUtf8);
            groupAppendU64(result.responseBody, existing.seq);
            groupAppendU64(result.responseBody, existing.msgId);
            groupAppendU64(result.responseBody, existing.createdAtMs);
            return result;
        }
        // 新消息：事务内原子分配序号（LAST_INSERT_ID 为连接级，同事务同连接）
        q.prepare("UPDATE group_info SET last_seq=LAST_INSERT_ID(last_seq+1),version=version+1 WHERE group_id=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        if (!q.exec() || q.numRowsAffected() != 1) { db.rollback(); return groupFailure(-2105); }
        q.prepare("SELECT LAST_INSERT_ID()");
        if (!q.exec() || !q.next()) { db.rollback(); return groupFailure(-2105); }
        const quint64 seq = q.value(0).toULongLong();
        if (seq == 0) { db.rollback(); return groupFailure(-2105); }
        q.prepare("INSERT INTO group_message(group_id,seq,client_msg_id,sender_name,msg_type,content) "
                  "VALUES (?,?,?,?,'text',?)");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(QVariant::fromValue<qulonglong>(seq));
        q.addBindValue(request.clientMsgId);
        q.addBindValue(actor);
        q.addBindValue(request.content);
        if (!q.exec()) { db.rollback(); return groupFailure(-2105); }
        const quint64 msgId = q.lastInsertId().toULongLong();
        q.prepare("SELECT ROUND(UNIX_TIMESTAMP(created_at)*1000) FROM group_message WHERE msg_id=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(msgId));
        if (!q.exec() || !q.next()) { db.rollback(); return groupFailure(-2105); }
        const quint64 createdAtMs = q.value(0).toULongLong();
        if (!db.commit()) { db.rollback(); return groupFailure(-2105); }
        // 先提交后广播：广播动作由 MsgHandler 在本函数返回后触发（设计 §5.2 不变式）
        result = groupFailure(0);
        result.groupId = request.groupId;
        result.message.seq = seq;
        result.message.msgId = msgId;
        result.message.sender = actor;
        result.message.msgType = 0;
        result.message.createdAtMs = createdAtMs;
        result.message.content = request.content;
        result.broadcastMessage = true;
        result.notifyUsers = groupActiveUsers(db, request.groupId);
        groupAppendString(result.responseBody, clientKeyUtf8);
        groupAppendU64(result.responseBody, seq);
        groupAppendU64(result.responseBody, msgId);
        groupAppendU64(result.responseBody, createdAtMs);
        return result;
    }

    if (request.operation == GroupPullMessages) {
        const quint8 mode = request.pullMode;       // 0 前向补齐 / 1 后向翻页，越界由解析层拒绝
        const int limit = qBound(1, int(request.limit), 100);
        q.prepare("SELECT status,last_seq FROM group_info WHERE group_id=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        if (!q.exec() || !q.next() || q.value(0).toString() != "active") return groupFailure(-2102);
        const quint64 groupLastSeq = q.value(1).toULongLong();
        q.prepare("SELECT last_read_seq FROM group_member WHERE group_id=? AND user_name=? AND status='active'");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(actor);
        if (!q.exec() || !q.next()) return groupFailure(-2101);
        const quint64 lastReadSeq = q.value(0).toULongLong();

        q.prepare(QStringLiteral("SELECT seq,msg_id,sender_name,msg_type,"
                                 "ROUND(UNIX_TIMESTAMP(created_at)*1000),content FROM group_message "
                                 "WHERE group_id=? AND seq %1 ? ORDER BY seq %2 LIMIT ?")
                  .arg(mode == 0 ? QStringLiteral(">") : QStringLiteral("<="),
                       mode == 0 ? QStringLiteral("ASC") : QStringLiteral("DESC")));
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(QVariant::fromValue<qulonglong>(request.cursorSeq));
        q.addBindValue(limit + 1); // 多取一条判定 has_more
        if (!q.exec()) return groupFailure(-2105);
        QList<QByteArray> records;
        while (q.next()) {
            const quint64 recSeq = q.value(0).toULongLong();
            QByteArray row;
            groupAppendU64(row, recSeq);
            groupAppendU64(row, q.value(1).toULongLong());
            groupAppendString(row, q.value(2).toString().toUtf8());
            row.append(char(quint8(q.value(3).toString() == "system" ? 1 : 0)));
            groupAppendU64(row, q.value(4).toULongLong());
            groupAppendString(row, q.value(5).toString().toUtf8());
            if (mode == 1) records.prepend(row); else records.append(row);
        }
        bool hasMore = records.size() > limit;
        if (hasMore) { if (mode == 1) records.removeFirst(); else records.removeLast(); }
        // next_cursor = 本批"实际返回"边界 seq（mode0=最大 / mode1=最小）：
        // 必须在裁掉多取行之后取，否则前向会跳过边界消息、后向会原地踏步。
        quint64 nextCursor = request.cursorSeq;
        if (!records.isEmpty()) {
            int cursorOffset = 0;
            quint64 boundary = 0;
            if (mode == 0) groupReadU64(records.last(), cursorOffset, boundary);
            else groupReadU64(records.first(), cursorOffset, boundary);
            nextCursor = boundary;
        }
        q.prepare("SELECT COUNT(*) FROM group_message WHERE group_id=? AND seq>? AND seq<=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(QVariant::fromValue<qulonglong>(lastReadSeq));
        q.addBindValue(QVariant::fromValue<qulonglong>(groupLastSeq));
        if (!q.exec() || !q.next()) return groupFailure(-2105);
        const quint64 unread = q.value(0).toULongLong();

        QByteArray body;
        body.append(char(mode));
        body.append(char(hasMore ? 1 : 0));
        groupAppendU64(body, nextCursor);
        groupAppendU64(body, groupLastSeq);
        groupAppendU64(body, unread);
        groupAppendU32(body, quint32(records.size()));
        for (const QByteArray &row : records) body.append(row);
        result = groupFailure(0);
        result.groupId = request.groupId;
        result.responseBody = body;
        return result;
    }

    if (request.operation == GroupReadConfirm) {
        // 单调推进：不可回退、不可越权代推（user_name 恒为 actor）、不可超过群水位
        q.prepare("SELECT status FROM group_info WHERE group_id=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        if (!q.exec() || !q.next() || q.value(0).toString() != "active") return groupFailure(-2102);
        q.prepare("SELECT 1 FROM group_member WHERE group_id=? AND user_name=? AND status='active'");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(actor);
        if (!q.exec() || !q.next()) return groupFailure(-2101);
        q.prepare("UPDATE group_member SET last_read_seq="
                  "LEAST(GREATEST(last_read_seq,?),"
                  "(SELECT last_seq FROM group_info WHERE group_id=?)) "
                  "WHERE group_id=? AND user_name=? AND status='active'");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.readSeq));
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(actor);
        if (!q.exec()) return groupFailure(-2105);
        q.prepare("SELECT last_read_seq FROM group_member WHERE group_id=? AND user_name=?");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(actor);
        if (!q.exec() || !q.next()) return groupFailure(-2105);
        result = groupFailure(0);
        result.groupId = request.groupId;
        groupAppendU64(result.responseBody, q.value(0).toULongLong());
        return result;
    }

    return groupFailure(-2000);
}

// ===== REQ-EXT-GROUP-003：群文件目录与访问控制 =====
// 判定点（全部服务端、每请求即时执行；与 GROUP-001/002 同源）：
//   P0 actor 有效 → P1 群存在 → P2 成员门(active) → P3 解散门(写操作) → P4 归属/存在性
//   → P5 删除授权(owner 或 上传者本人) → P6 名称(调用方预校验) → P7 对象存在性。
// 失败码：-1001 未认证 / -1002 库不可达 / -2101 非成员 / -2102 群不存在或已解散 /
//        -2103 归属或权限非法 / -2105 对象缺失或安置失败。不泄露他群存在性。
OperateDB::GroupFileResult OperateDB::handleGroupFileOperation(GroupFileRequest &request,
                                                              const QString &actor)
{
    GroupFileResult result;
    if (actor.isEmpty()) { result.code = -1001; return result; }
    PooledSqlConnection connection = DatabaseConnectionPool::instance().acquire();
    if (!connection.isValid()) { result.code = -1002; return result; }
    QSqlDatabase db = connection.database();
    QSqlQuery q(db);

    // P1：群存在（取状态）
    q.prepare("SELECT status FROM group_info WHERE group_id=?");
    q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
    if (!q.exec() || !q.next()) { result.code = -2102; return result; }
    const QString groupStatus = q.value(0).toString();

    // P2：成员门（status='active'；left/removed/无行一律拒绝——退出/被移除即时失权）
    const QString role = groupRole(db, request.groupId, actor);
    if (role.isEmpty()) { result.code = -2101; return result; }

    // P3：解散门——写操作（上传/删除）要求群 active；读操作（列表/下载）成员可只读历史
    const bool writeOp = (request.operation == GroupFileUploadInit
                          || request.operation == GroupFileDelete);
    if (writeOp && groupStatus != "active") { result.code = -2102; return result; }

    if (request.operation == GroupFileList) {
        q.prepare("SELECT COUNT(*) FROM group_file WHERE group_id=? AND status='active'");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        if (!q.exec() || !q.next()) { result.code = -2105; return result; }
        const quint32 total = q.value(0).toUInt();

        q.prepare("SELECT group_file_id,size_bytes,UNIX_TIMESTAMP(created_at)*1000,"
                  "uploader_name,display_name FROM group_file "
                  "WHERE group_id=? AND status='active' ORDER BY group_file_id LIMIT ? OFFSET ?");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        q.addBindValue(int(request.limit));
        q.addBindValue(int(request.offset));
        if (!q.exec()) { result.code = -2105; return result; }
        QList<QByteArray> records;
        while (q.next()) {
            QByteArray row;
            // 元数据四元：名称、大小、上传者、时间（不含任何物理路径）
            groupAppendU64(row, q.value(0).toULongLong());
            groupAppendU64(row, q.value(1).toULongLong());
            groupAppendU64(row, q.value(2).toULongLong());
            groupAppendString(row, q.value(3).toString().toUtf8());
            groupAppendString(row, q.value(4).toString().toUtf8());
            records.append(row);
        }
        QByteArray body;
        groupAppendU32(body, quint32(records.size()));
        groupAppendU32(body, total);
        for (const QByteArray &row : records) body.append(row);
        result.code = 0;
        result.responseBody = body;
        return result;
    }

    if (request.operation == GroupFileUploadInit) {
        if (request.fileSize < 0 || request.shaHex.isEmpty() || request.displayName.isEmpty()) {
            result.code = -2103;
            return result;
        }
        // P7 去重预检查：同 sha256+size 的既有物理对象（FOR UPDATE 防并发双建）
        q.prepare("SELECT object_id FROM file_object WHERE sha256=? AND size_bytes=? FOR UPDATE");
        q.addBindValue(request.shaHex);
        q.addBindValue(request.fileSize);
        qint64 objectId = -1;
        if (q.exec() && q.next()) objectId = q.value(0).toLongLong();

        if (objectId >= 0) {
            // 秒传命中：个人条目 + 群条目 + 引用计数同事务；零正文传输
            const FileObjectStore::GroupAttachResult ar = FileObjectStore::instance().attachGroupFile(
                        actor, request.groupId, request.displayName, objectId, request.fileSize,
                        /*createUserEntry=*/true);
            if (!ar.ok) {
                qCWarning(lcDb) << "[GroupFile] 秒传安置失败 group=" << request.groupId
                                << "原因=" << ar.error;
                result.code = -2105;
                return result;
            }
            result.code = 0;
            result.hit = true;
            result.groupFileId = quint64(ar.groupFileId);
            request.hostFileId = ar.fileId;
            request.objectId = objectId;
            request.displayName = ar.groupDisplayName;
            return result;
        }
        // 未命中：不建对象/条目，由调用方建立暂存会话（staging + upload_session）走既有分块
        result.code = 0;
        result.hit = false;
        return result;
    }

    if (request.operation == GroupFileDelete) {
        // P5 授权在 releaseGroupFile 内与 P4 归属同事务判定：owner 可删任意，上传者可删己
        const bool isOwner = (role == "owner");
        QString err;
        if (!FileObjectStore::instance().releaseGroupFile(qint64(request.groupFileId),
                                                          request.groupId, actor, isOwner, err)) {
            qCWarning(lcDb) << "[GroupFile] 删除拒绝 group=" << request.groupId
                            << " gf=" << request.groupFileId << "原因=" << err;
            result.code = -2103;
            return result;
        }
        result.code = 0;
        return result;
    }

    if (request.operation == GroupFileDownload) {
        // P4 归属：group_file_id 必须与信封 group_id 同群且 active（不泄露他群存在性）
        q.prepare("SELECT object_id,size_bytes,display_name FROM group_file "
                  "WHERE group_file_id=? AND group_id=? AND status='active'");
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupFileId));
        q.addBindValue(QVariant::fromValue<qulonglong>(request.groupId));
        if (!q.exec() || !q.next()) { result.code = -2103; return result; }
        const qint64 objectId = q.value(0).toLongLong();
        request.objectId = objectId;
        request.sizeBytes = q.value(1).toLongLong();
        request.displayName = q.value(2).toString();

        // P7 对象存在性（物理绝对路径由服务端解析，绝不接受客户端路径）
        q.prepare("SELECT storage_path FROM file_object WHERE object_id=?");
        q.addBindValue(objectId);
        if (!q.exec() || !q.next()) { result.code = -2105; return result; }
        result.code = 0;
        return result;
    }

    result.code = -2103;
    return result;
}
