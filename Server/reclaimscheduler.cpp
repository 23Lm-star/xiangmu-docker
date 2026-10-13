#include "logging.h"
#include "reclaimscheduler.h"
#include "databaseconnectionpool.h"
#include "server.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

namespace {
// 路径比较一律 cleanPath + 小写：Windows 盘符大小写与分隔符都可能不一致，
// 拿原串比会让"活跃会话在用"的判定静默失效（那是会误删续传现场的）。
QString normKey(const QString& p)
{
    return QDir::cleanPath(QFileInfo(p).absoluteFilePath()).toLower();
}
}   // namespace

ReclaimScheduler& ReclaimScheduler::instance()
{
    static ReclaimScheduler inst;
    return inst;
}

ReclaimScheduler::ReclaimScheduler(QObject* parent)
    : QObject(parent)
{
}

ReclaimScheduler::Config ReclaimScheduler::loadConfig() const
{
    Config c;    // 出来就是保守默认：1 小时一轮、每拍 20 条、暂存 6 小时、宽限 6 小时
    const QString path = QCoreApplication::applicationDirPath() + "/reclaim.cfg";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCInfo(lcFs) << "[RECLAIM] 未找到 reclaim.cfg，使用保守默认"
                     << "(1h/20条/500上限/暂存6小时/宽限6小时/真删)";
        return c;
    }
    const QStringList lines = QString::fromUtf8(f.readAll()).split('\n');
    f.close();
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(';')) continue;
        const int eq = line.indexOf('=');
        if (eq <= 0) continue;
        const QString key = line.left(eq).trimmed().toLower();
        const QString val = line.mid(eq + 1).trimmed();
        bool ok = false;
        const int n = val.toInt(&ok);
        const bool on = (val == "1" || val.toLower() == "true");
        if (key == "enabled")                     c.enabled = on;
        else if (key == "dry_run")                c.dryRun = on;
        // 数值一律要求 > 0：这些旋钮全都会放大删除力度，0 或负数不得静默生效。
        // 尤其 grace_hours=0 会删掉复活窗口还开着的对象——那正是宽限期要防的误删。
        else if (key == "interval_minutes"   && ok && n > 0) c.intervalMinutes = n;
        else if (key == "tick_ms"            && ok && n > 0) c.tickMs = n;
        else if (key == "per_tick"           && ok && n > 0) c.perTick = n;
        else if (key == "per_round_limit"    && ok && n > 0) c.perRoundLimit = n;
        else if (key == "staging_ttl_hours"  && ok && n > 0) c.stagingTtlHours = n;
        else if (key == "grace_hours"        && ok && n > 0) c.graceHours = n;
        else qCWarning(lcFs) << "[RECLAIM] reclaim.cfg 忽略无法识别的行:" << line;
    }
    return c;
}

void ReclaimScheduler::start()
{
    if (m_started) return;
    m_cfg = loadConfig();
    if (!m_cfg.enabled) {
        qCWarning(lcFs) << "[RECLAIM] reclaim.cfg 里 enabled=0，周期清理调度器不启动"
                        << "（待回收对象与废弃暂存将无人收割）";
        return;
    }
    m_started = true;
    m_timer.setInterval(m_cfg.tickMs);
    m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &ReclaimScheduler::onTick);
    m_timer.start();
    // m_lastRoundStartMs 保持 0 → 第一次心跳（tickMs 之后）就满足"距上轮已超间隔"，
    // 立刻跑一轮。进程刚起来正好把上一个进程崩溃留下的 reclaiming 行收干净。
    qCWarning(lcFs) << "[RECLAIM] 周期清理调度器已启动"
                    << "心跳=" << m_cfg.tickMs << "ms"
                    << "每拍上限=" << m_cfg.perTick << "条"
                    << "单轮上限=" << m_cfg.perRoundLimit << "条"
                    << "轮次间隔=" << m_cfg.intervalMinutes << "分钟"
                    << "暂存TTL=" << m_cfg.stagingTtlHours << "小时"
                    << "宽限期=" << m_cfg.graceHours << "小时"
                    << "dry_run=" << m_cfg.dryRun;
}

void ReclaimScheduler::stop()
{
    m_timer.stop();
    m_started = false;
    m_roundActive = false;
}

// 每次心跳只做"有界的一小片"：把扫盘+unlink切成可控小片，主线程最坏停顿因此可控。
// 一轮横跨多次心跳，不阻塞事件循环。
void ReclaimScheduler::onTick()
{
    const Config& cfg = m_cfg;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    if (!m_roundActive) {
        if (now - m_lastRoundStartMs < qint64(cfg.intervalMinutes) * 60000LL) return;
        m_roundActive = true;
        m_roundProcessed = 0;
        m_roundStartMs = now;
        m_lastRoundStartMs = now;
        m_roundFailedObjects.clear();
        m_roundFailedStaging.clear();
        m_roundStats = Stats();
        recoverStaleClaims(cfg, m_roundStats);
        qCDebug(lcFs) << "[RECLAIM] 轮次开始";
    }

    int budget = qMin(cfg.perTick, cfg.perRoundLimit - m_roundProcessed);
    int used = 0;
    if (budget > 0) {
        used = harvestReclaimObjects(cfg, budget, m_roundStats);   // 对象线优先：它带不变量
        budget -= used;
    }
    if (budget > 0) {
        used += harvestStagingFiles(cfg, budget, m_roundStats);
    }
    m_roundProcessed += used;

    // 本拍一条都没动而预算还有剩 = 两条线都取不出候选了 → 本轮自然收尾。
    // （不是"预算用完了"那种要继续的情况：这里用了整整一拍去确认，没有空转。）
    if (m_roundProcessed >= cfg.perRoundLimit || used == 0) {
        m_roundStats.elapsedMs = now - m_roundStartMs;
        logWatermark(cfg, m_roundStats);
        m_roundActive = false;
    }
}

ReclaimScheduler::Stats ReclaimScheduler::runRound(const Config& cfg)
{
    Stats st;
    if (m_roundActive) {
        qCWarning(lcFs) << "[RECLAIM] 定时轮次进行中，手动轮次跳过";
        return st;
    }
    m_roundFailedObjects.clear();
    m_roundFailedStaging.clear();
    const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
    recoverStaleClaims(cfg, st);
    int processed = 0;
    while (processed < cfg.perRoundLimit) {
        int budget = qMin(cfg.perTick, cfg.perRoundLimit - processed);
        int used = harvestReclaimObjects(cfg, budget, st);
        budget -= used;
        if (budget > 0) used += harvestStagingFiles(cfg, budget, st);
        if (used == 0) break;
        processed += used;
    }
    st.elapsedMs = QDateTime::currentMSecsSinceEpoch() - t0;
    m_lastRoundStartMs = t0;      // 手动跑过就不再让定时轮次紧接着重复一遍
    logWatermark(cfg, st);
    return st;
}

// 抢权 → unlink → 删记录全程同步，所以轮次之间本进程不可能有在途抢权：
// 此刻表里任何 reclaiming 行都必然是上一个进程崩溃留下的。不需要额外的时间戳列。
void ReclaimScheduler::recoverStaleClaims(const Config& cfg, Stats& st)
{
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) return;
    QSqlQuery q(conn.database());
    if (!q.exec("SELECT object_id,storage_path FROM file_object WHERE status='reclaiming'")) {
        qCWarning(lcFs) << "[RECLAIM] 崩溃残留扫描失败:" << q.lastError().text();
        return;
    }
    QList<qint64> ids;
    QList<QString> rels;
    while (q.next()) {
        ids.append(q.value(0).toLongLong());
        rels.append(q.value(1).toString());
    }
    for (int i = 0; i < ids.size(); ++i) {
        const qint64 oid = ids[i];
        const QString abs = safeObjectAbsPath(rels[i]);
        const bool fileGone = !abs.isEmpty() && !QFileInfo::exists(abs);
        const char* what = abs.isEmpty() ? "路径不可信→退回"
                          : fileGone    ? "文件已无→补删记录"
                                        : "文件仍在→退回";
        if (cfg.dryRun) {
            // dry_run 的承诺是"不做任何写"，这一条没有例外：只看、只报。
            // 抢权是同步的，这里也不会有"本进程正在删"的行被误判成残留。
            ++st.recovered;
            qCInfo(lcFs) << "[RECLAIM][dry-run] 崩溃残留 object=" << oid << what;
            continue;
        }
        QSqlQuery u(conn.database());
        if (fileGone) {
            // 文件已经没了 = 当年的 unlink 其实成功了，只是没来得及删记录 → 补完。
            u.prepare("UPDATE user_file SET object_id=NULL WHERE object_id=? AND status<>'active'");
            u.addBindValue(oid);
            u.exec();
            u.prepare("UPDATE group_file SET object_id=NULL WHERE object_id=? AND status<>'active'");
            u.addBindValue(oid);
            u.exec();
            u.prepare("DELETE FROM file_object WHERE object_id=? AND status='reclaiming'");
            u.addBindValue(oid);
            if (!u.exec()) continue;
        } else {
            // 文件还在（或路径不可信）= unlink 根本没发生过 → 退回待回收，下轮重试。
            u.prepare("UPDATE file_object SET status='pending_reclaim' WHERE object_id=? AND status='reclaiming'");
            u.addBindValue(oid);
            if (!u.exec()) continue;
        }
        ++st.recovered;
        qCWarning(lcFs) << "[RECLAIM] 处置崩溃残留 object=" << oid << what;
    }
}

// 返回本拍实际动过的条数（consumed），供限速切片计算剩余预算。
int ReclaimScheduler::harvestReclaimObjects(const Config& cfg, int budget, Stats& st)
{
    if (budget <= 0) return 0;
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) {
        qCWarning(lcFs) << "[RECLAIM] 数据库不可达，本拍对象线跳过";
        return 0;
    }
    QSqlDatabase db = conn.database();

    QSqlQuery q(db);
    // 候选走 idx_status_reclaim(status, reclaim_at)：不扫全表。
    // 两个 NOT EXISTS 是刻意的：refcount<=0 只是提示，真相从表里现算——删除路径上
    // 宁可多查一次，也不能因为一个计数 bug 把还有活引用的对象当成垃圾。
    //（外键给 user_file.object_id / group_file.object_id 自动建了索引，子查询走索引。）
    // 已处理的条目下一拍自然从候选集消失（状态已变），所以不需要游标——每拍重查即可，
    // 整轮因此天然幂等、可中断、可重入。
    q.prepare("SELECT fo.object_id, fo.storage_path FROM file_object fo "
              "WHERE fo.status='pending_reclaim' AND fo.refcount<=0 "
              "  AND fo.reclaim_at IS NOT NULL "
              "  AND fo.reclaim_at < DATE_SUB(NOW(), INTERVAL ? HOUR) "
              "  AND NOT EXISTS (SELECT 1 FROM user_file  uf "
              "                  WHERE uf.object_id=fo.object_id AND uf.status='active') "
              "  AND NOT EXISTS (SELECT 1 FROM group_file gf "
              "                  WHERE gf.object_id=fo.object_id AND gf.status='active') "
              "ORDER BY fo.reclaim_at ASC LIMIT ?");
    q.addBindValue(cfg.graceHours);
    q.addBindValue(budget + m_roundFailedObjects.size());   // 给本轮已知失败项留出位置
    if (!q.exec()) {
        qCWarning(lcFs) << "[RECLAIM] 对象候选查询失败:" << q.lastError().text();
        return 0;
    }
    QList<qint64> ids;
    QList<QString> rels;
    while (q.next()) {
        const qint64 oid = q.value(0).toLongLong();
        if (m_roundFailedObjects.contains(oid)) continue;   // 本轮已失败过，不重复烧预算
        ids.append(oid);
        rels.append(q.value(1).toString());
        if (ids.size() >= budget) break;
    }
    st.reclaimCandidates += ids.size();

    if (cfg.dryRun) {
        // 干跑：只报候选、不做任何写。返回 0 让轮次立刻收尾，不去反复重扫同一批。
        for (int i = 0; i < ids.size(); ++i) {
            qCInfo(lcFs) << "[RECLAIM][dry-run] 待回收 object=" << ids[i] << rels[i];
        }
        return 0;
    }

    int consumed = 0;
    for (int i = 0; i < ids.size(); ++i) {
        const qint64 oid = ids[i];
        // 落地前的最后一道闸：路径必须落在 <filesPath>/objects 子树内。
        const QString abs = safeObjectAbsPath(rels[i]);
        if (abs.isEmpty()) {
            qCWarning(lcFs) << "[RECLAIM] 拒绝落地：storage_path 不可信 object=" << oid << rels[i];
            ++st.skippedUnsafe;
            m_roundFailedObjects.insert(oid);
            ++consumed;
            continue;
        }

        // 第 1 步：抢权。这是删除权的唯一来源——影响行数 1 才算抢到。
        q.prepare("UPDATE file_object SET status='reclaiming' "
                  "WHERE object_id=? AND refcount<=0 AND status='pending_reclaim'");
        q.addBindValue(oid);
        if (!q.exec()) {
            qCWarning(lcFs) << "[RECLAIM] 抢权失败:" << q.lastError().text();
            break;
        }
        ++consumed;
        if (q.numRowsAffected() != 1) {
            // 影响 0 = 有人在同一行上复活成功了（复活的谓词不含 reclaiming）。
            // 让位，绝不碰磁盘——这不是异常，是设计里唯一正确的互斥结果。
            ++st.lostRace;
            continue;
        }

        // 第 2 步：unlink。文件已不在 = 上一次其实成功过（崩溃/重试）→ 按成功处理。
        // 这一条让整条路径幂等，也让"删记录失败后重来"能自愈。
        bool unlinked = false;
        if (!QFileInfo::exists(abs)) {
            unlinked = true;
        } else {
            unlinked = QFile::remove(abs);
            if (!unlinked) {
                // Windows 上被下载句柄占着就会走到这里——正是我们想要的：
                // 在用的对象本轮跳过、下轮重试，不需要额外登记谁在用。
                qCWarning(lcFs) << "[RECLAIM] unlink 失败（占用/权限），本轮跳过 object=" << oid << abs;
                ++st.unlinkFailed;
            }
        }
        if (!unlinked) {
            q.prepare("UPDATE file_object SET status='pending_reclaim' "
                      "WHERE object_id=? AND status='reclaiming'");
            q.addBindValue(oid);
            q.exec();
            m_roundFailedObjects.insert(oid);
            continue;
        }

        // 第 3 步：同一事务内摘悬挂引用 + 删对象行。
        // 摘引用是必需的：软删条目和已移除群文件都还留着 object_id，外键会挡住
        // DELETE FROM file_object（1451）。只动非 active 行——所有存量读路径都带
        // status='active' 过滤，所以它们的引用语义上早就没了。
        bool ok = false;
        if (db.transaction()) {
            do {
                QSqlQuery t(db);
                t.prepare("UPDATE user_file SET object_id=NULL WHERE object_id=? AND status<>'active'");
                t.addBindValue(oid);
                if (!t.exec()) break;
                t.prepare("UPDATE group_file SET object_id=NULL WHERE object_id=? AND status<>'active'");
                t.addBindValue(oid);
                if (!t.exec()) break;
                t.prepare("DELETE FROM file_object WHERE object_id=? AND status='reclaiming'");
                t.addBindValue(oid);
                if (!t.exec() || t.numRowsAffected() != 1) break;
                ok = db.commit();
            } while (false);
        }
        if (!ok) {
            db.rollback();
            // 文件已在第 2 步删掉、行还没删 → 状态与磁盘不一致。退回待回收，下轮
            // 候选命中时发现文件不存在会直接补完删记录。不一致窗口就此收敛，绝不吞掉。
            qCWarning(lcFs) << "[RECLAIM] 摘引用/删记录失败，退回待回收等下一轮补完 object=" << oid;
            QSqlQuery r(db);
            r.prepare("UPDATE file_object SET status='pending_reclaim' WHERE object_id=? AND status='reclaiming'");
            r.addBindValue(oid);
            r.exec();
            m_roundFailedObjects.insert(oid);
            continue;
        }
        ++st.reclaimDeleted;
    }
    return consumed;
}

int ReclaimScheduler::harvestStagingFiles(const Config& cfg, int budget, Stats& st)
{
    if (budget <= 0 || cfg.stagingTtlHours <= 0) return 0;
    QDir dir(stagingDir());
    if (!dir.exists()) return 0;

    const QSet<QString> active = activeSessionStagingPaths();
    const qint64 cutoffMs = QDateTime::currentMSecsSinceEpoch()
                            - qint64(cfg.stagingTtlHours) * 3600000LL;

    QFileInfoList parts = dir.entryInfoList(QStringList() << "*.part", QDir::Files);
    std::sort(parts.begin(), parts.end(), [](const QFileInfo& a, const QFileInfo& b) {
        return a.lastModified() < b.lastModified();      // 最老的先收
    });

    int consumed = 0;
    for (const QFileInfo& fi : parts) {
        // 已按最老优先排序：第一个没过 TTL 的后面都不用看了。
        if (fi.lastModified().toMSecsSinceEpoch() > cutoffMs) break;
        const QString abs = fi.absoluteFilePath();
        if (active.contains(normKey(abs))) continue;                // 有会话正在用
        if (m_roundFailedStaging.contains(normKey(abs))) continue;  // 本轮已失败过
        if (cfg.dryRun) {
            qCInfo(lcFs) << "[RECLAIM][dry-run] 待清暂存" << fi.fileName()
                         << "年龄(小时)=" << fi.lastModified().secsTo(QDateTime::currentDateTime()) / 3600.0;
            continue;
        }
        ++st.stagingScanned;
        if (QFile::remove(abs)) {
            ++st.stagingRemoved;
        } else {
            qCWarning(lcFs) << "[RECLAIM] 暂存删除失败（占用/权限），本轮跳过" << abs;
            m_roundFailedStaging.insert(normKey(abs));
        }
        if (++consumed >= budget) break;
    }
    return consumed;
}

QSet<QString> ReclaimScheduler::activeSessionStagingPaths() const
{
    QSet<QString> out;
    PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
    if (!conn.isValid()) return out;
    QSqlQuery q(conn.database());
    // 只有 uploading/verifying 的会话才算"在用"。expired/failed 是**有意保留** .part
    // 待续传的状态，但这种保留受 TTL 约束：过了 staging_ttl_hours 不再来续，就当作废弃。
    // 注：真正在上传的文件 mtime 每一片都在刷新，本来就过不了 TTL 那一关；
    // 这一句是"万一"的兜底，不是主要防线。
    if (q.exec("SELECT staging_path FROM upload_session "
               "WHERE status IN ('uploading','verifying')")) {
        while (q.next()) {
            const QString p = q.value(0).toString();
            if (!p.isEmpty()) out.insert(normKey(p));
        }
    }
    return out;
}

QString ReclaimScheduler::stagingDir() const
{
    return QDir(Server::getinstance().filesPath).absoluteFilePath(".staging");
}

QString ReclaimScheduler::safeObjectAbsPath(const QString& rel) const
{
    if (rel.isEmpty()) return QString();
    if (rel.startsWith("objects_tmp/")) return QString();   // 事务中途的占位，尚未成立
    if (rel.contains("..")) return QString();
    if (QFileInfo(rel).isAbsolute()) return QString();

    const QString fsRoot = QDir(Server::getinstance().filesPath).absolutePath();
    const QString objRoot = QDir::cleanPath(QDir(fsRoot).absoluteFilePath("objects"));
    const QString abs = QDir::cleanPath(QDir(fsRoot).absoluteFilePath(rel));
    // 只允许 objects/ 子树。storage_path 是服务端生成的，但"删除"这条路径上
    // 再便宜的越界校验都不算多——绝不给任何输入一个 unlink 任意文件的机会。
    if (abs != objRoot && !abs.startsWith(objRoot + '/')) return QString();
    return abs;
}

void ReclaimScheduler::logWatermark(const Config& cfg, const Stats& st) const
{
    // 设计里的五个指标：暂存总量与最老年龄 / 待回收数量与占用 /
    // 本轮删除数 / 剩余候选规模 / 删除失败计数。没有度量就没有治理。
    qint64 stagingCount = 0;
    qint64 stagingBytes = 0;
    qint64 oldestMs = 0;
    {
        const QFileInfoList parts =
                QDir(stagingDir()).entryInfoList(QStringList() << "*.part", QDir::Files);
        for (const QFileInfo& fi : parts) {
            ++stagingCount;
            stagingBytes += fi.size();
            const qint64 ms = fi.lastModified().toMSecsSinceEpoch();
            if (oldestMs == 0 || ms < oldestMs) oldestMs = ms;
        }
    }
    qint64 pendingCount = -1;
    qint64 pendingBytes = -1;
    {
        PooledSqlConnection conn = DatabaseConnectionPool::instance().acquire();
        if (conn.isValid()) {
            QSqlQuery q(conn.database());
            if (q.exec("SELECT COUNT(*), COALESCE(SUM(size_bytes),0) FROM file_object "
                       "WHERE status='pending_reclaim'") && q.next()) {
                pendingCount = q.value(0).toLongLong();
                pendingBytes = q.value(1).toLongLong();
            }
        }
    }
    const qint64 oldestAgeH = (oldestMs > 0)
            ? (QDateTime::currentMSecsSinceEpoch() - oldestMs) / 3600000LL : -1;

    qCInfo(lcFs) << "[RECLAIM] 轮次结束"
                 << "暂存=" << stagingCount << "个/" << (stagingBytes / 1048576) << "MB"
                 << "最老=" << oldestAgeH << "h"
                 << "待回收=" << pendingCount << "个/" << (pendingBytes / 1048576) << "MB"
                 << "本轮候选=" << st.reclaimCandidates
                 << "已回收=" << st.reclaimDeleted
                 << "暂存已清=" << st.stagingRemoved
                 << "让位复活=" << st.lostRace
                 << "unlink失败=" << st.unlinkFailed
                 << "崩溃残留=" << st.recovered
                 << "越界拒绝=" << st.skippedUnsafe
                 << "用时=" << st.elapsedMs << "ms"
                 << (cfg.dryRun ? "【dry-run：未做任何写】" : "");
}
