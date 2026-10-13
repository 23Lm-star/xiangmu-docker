#ifndef RECLAIMSCHEDULER_H
#define RECLAIMSCHEDULER_H

#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>

// REQ-EXT-RECLAIM-001：周期清理调度器——延迟删除的收割者。
//
// 为什么必须有它：D5 之后"删"被有意推迟了。引用归零只标 pending_reclaim 而不物理删，
// 是为了给"删的人"和"正要上传同内容的人"之间留出复活窗口；失败/中止的上传保留 .part
// 待续传。这两个机制各自正确，但合起来留下两处**无界增长点**：
//     废弃暂存文件（上传者再也不回来续传）
//     pending_reclaim 积压（引用归零但没人来收）
// 东西不会自己消失，所以需要一个只负责"收"的角色。
//
// ★ 三条设计红线——全部指向同一个目标：绝不影响在线业务 ★
//
//   1) 跑在主线程 QTimer 上，每次心跳只动有界条数。
//      清理的阻塞量其实很小：候选来自 DB 索引查询（对象路径就在 file_object.storage_path
//      里，**根本不用遍历对象目录**），.staging 是单个扁平目录（列一次毫秒级），unlink
//      每次亚毫秒。所以不新开线程——新线程反而要引入跨线程访问全局 DB 池、第二个
//      user_file/file_object 写入者、新的关闭顺序风险，等于用更大的风险换更小的收益。
//      额外白拿一道保护：Windows 上下载流持有句柄时 rename/unlink 都会失败，于是
//      正在被下载的对象本轮自动跳过、下轮重试——在用对象天然排除，不需要额外登记。
//
//   2) 候选走索引、LIMIT 分页；绝不扫全表，也绝不遍历对象目录。
//      对象线走 idx_status_reclaim(status, reclaim_at)；暂存线列 .staging 单目录。
//      已处理的条目下一拍自然从候选集里消失（状态已变/文件已删），所以**不需要游标**
//      ——每拍重查即可。这让整轮天然幂等、可中断、可重入。
//
//   3) 删除权 = 一次条件状态迁移；复活 = 条件更新。两者在同一行上串行化。
//          调度器： UPDATE ... SET status='reclaiming'
//                    WHERE object_id=? AND refcount<=0 AND status='pending_reclaim'
//          复活方： UPDATE ... SET refcount=refcount+1,status='ready'
//                    WHERE object_id=? AND status IN ('ready','pending_reclaim')
//      两边都是同一行上的条件 UPDATE，由 InnoDB 行锁串行化，**必有一方影响行数为 0**。
//      于是"删的人"与"正要复活的人"之间不存在窗口——不是靠"约定谁先跑"的时序依赖，
//      所以也不需要"读时兜底"这种补丁。抢权输了就整条跳过、不碰磁盘。
//
// 一轮的形状（两条收割线共用同一形状：取候选 → 抢权 → 落地）：
//   A. 待回收对象： 抢权(reclaiming) → unlink → 同一事务内摘悬挂引用 + 删对象行
//                   unlink 失败 → 回退 pending_reclaim，本轮不再碰它，下轮重试
//   B. 废弃暂存：   超 TTL 且无活跃会话引用 → 直接 unlink（无跨资源不变量，不需要抢权）
//
// 判据（都必须"且"）：
//   暂存： ① mtime 超过 staging TTL ② 不被任何 uploading/verifying 会话引用
//   对象： ① status=pending_reclaim ② refcount<=0 ③ reclaim_at 超过宽限期
//          ④ **表上确实没有 active 引用**（user_file/group_file 两表 NOT EXISTS）
//   第 ④ 条是刻意加的：refcount 只是提示，真相从表里现算。删除路径上宁可多查一次。
class ReclaimScheduler : public QObject
{
    Q_OBJECT
public:
    struct Config {
        bool enabled = true;
        bool dryRun = false;         // 只报候选、不做任何写；上线前观察判定用
        int tickMs = 1000;           // 心跳间隔
        int intervalMinutes = 60;    // 两轮之间的间隔
        int perTick = 20;            // 每次心跳最多动几条——限速的实际旋钮
        int perRoundLimit = 500;     // 单轮上限，剩下的留给下一轮
        // 两个保留期都以**小时**计。本项目里被收走的都是可再生的临时物（废弃暂存、
        // 引用归零的对象），长窗口只把"观察一轮真实回收"变成"等一周/等一个月"，
        // 所以按小时配，默认 6 小时——即观察一轮的最长等待。要按天用就写 24/168
        // 这种倍数，单位不换、语义不变。
        int stagingTtlHours = 6;     // 废弃暂存文件保留期
        int graceHours = 6;          // 待回收对象宽限期（复活窗口）
    };

    struct Stats {
        qint64 stagingScanned = 0;
        qint64 stagingRemoved = 0;
        qint64 reclaimCandidates = 0;
        qint64 reclaimDeleted = 0;
        qint64 lostRace = 0;         // 抢权影响行数 0：有人复活成功了，我们让位
        qint64 unlinkFailed = 0;
        qint64 recovered = 0;        // 崩溃残留的 reclaiming 行
        qint64 skippedUnsafe = 0;    // storage_path 越界/仍有活跃引用，拒绝落地
        qint64 elapsedMs = 0;
    };

    static ReclaimScheduler& instance();

    Config loadConfig() const;       // 读 reclaim.cfg；缺失或非法一律取保守默认
    void start();                    // 起心跳（可重复调用，第二次为空操作）
    void stop();

    // 同步跑完一轮，不依赖 QTimer——供人工验证与测试用。
    // 传自定义 cfg 可把 TTL/宽限期临时调小，立即观察真实删除行为。
    Stats runRound(const Config& cfg);

private slots:
    void onTick();

private:
    explicit ReclaimScheduler(QObject* parent = nullptr);
    ReclaimScheduler(const ReclaimScheduler&) = delete;
    ReclaimScheduler& operator=(const ReclaimScheduler&) = delete;

    // 崩溃残留：抢权→unlink→删记录全程同步，轮次之间本进程不可能有在途抢权，
    // 所以此刻表里任何 reclaiming 行都必然是上一个进程崩溃留下的。不需要时间戳列。
    void recoverStaleClaims(const Config& cfg, Stats& st);
    int harvestReclaimObjects(const Config& cfg, int budget, Stats& st);
    int harvestStagingFiles(const Config& cfg, int budget, Stats& st);
    void logWatermark(const Config& cfg, const Stats& st) const;

    // storage_path → 绝对路径。越界、空、未完成对象一律返回空串：
    // 绝不 unlink 任何未经校验的路径。
    QString safeObjectAbsPath(const QString& rel) const;
    QString stagingDir() const;
    QSet<QString> activeSessionStagingPaths() const;

    QTimer m_timer;
    Config m_cfg;                    // start() 时快照，避免每拍都读配置文件
    Stats m_roundStats;              // 跨拍累积的本轮统计（一轮可能横跨多次心跳）
    bool m_started = false;
    bool m_roundActive = false;
    int m_roundProcessed = 0;
    qint64 m_lastRoundStartMs = 0;
    qint64 m_roundStartMs = 0;
    // 本轮已失败的条目，避免"最老的那条一直失败"把预算烧光、轮次永不结束。
    QSet<qint64> m_roundFailedObjects;
    QSet<QString> m_roundFailedStaging;
};

#endif // RECLAIMSCHEDULER_H
