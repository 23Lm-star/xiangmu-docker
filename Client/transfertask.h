#ifndef TRANSFERTASK_H
#define TRANSFERTASK_H

#include "protocol.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QWidget>

class QFile;
class QPushButton;
class QTableWidget;
class QLabel;
class Uploader;

// REQ-EXT-TRANSFER-001：客户端多文件有限并发的任务管理与调度（§5）。
// 传输单元 = 文件级任务；上传/下载两个方向各 3 个名额（D6），超限排队（不报错不丢文件）。
class TransferManager : public QObject
{
    Q_OBJECT
public:
    enum Kind { PersonalUpload, GroupUpload, PersonalDownload, GroupDownload };
    enum State { Queued, Hashing, Running, Paused, Done, Failed, Cancelled };

    struct Task {
        quint64 key = 0;         // 稳定行键（UI 行标识；续传换 id 不变）
        quint64 id = 0;          // 当前 wire transfer_id
        Kind    kind = PersonalUpload;
        State   state = Queued;
        QString localPath;       // 上传源路径
        QString displayName;     // 文件展示名
        QString dirPath;         // 个人上传目标目录（服务端路径）
        QString serverPath;      // 个人下载源（服务端路径）
        quint64 groupId = 0;     // 群作用域
        quint64 groupFileId = 0; // 群下载源
        qint64  totalBytes = 0;
        qint64  doneBytes = 0;
        QString failReason;
        QByteArray shaHex;       // 群上传预检查
        bool    hashPending = false;
        QFile*  recvFile = nullptr;
        QString savePath;
        Uploader* uploader = nullptr;
        bool    needFastcheck = true;   // 个人上传首次走 97 预检查；续传跳过（S3）
        bool    wasRestored = false;    // 快照恢复的任务：首次继续仍走 97（服务端 staging 可能已变）
        QString fastcheckToken;         // AUD-3：本次 97 的请求标识（服务端 98 回带，用于按标识归属）
        bool isUpload() const { return kind == PersonalUpload || kind == GroupUpload; }
    };

    static TransferManager& instance();

    quint64 allocId();

    // 入队（返回任务行键）
    quint64 enqueuePersonalUpload(const QString& localPath, const QString& dirPath, const QString& displayName);
    quint64 enqueueGroupUpload(quint64 groupId, const QString& localPath, const QString& displayName, const QByteArray& shaHex);
    quint64 enqueuePersonalDownload(const QString& serverPath, const QString& displayName);
    quint64 enqueueGroupDownload(quint64 groupId, quint64 groupFileId, const QString& displayName);

    Task* taskByKey(quint64 key) { return m_tasks.contains(key) ? &m_tasks[key] : nullptr; }
    QList<quint64> taskKeys() const;

    // 路由（由 Client::handlMsg 调用）
    void onFastcheckRespond(PDU* pdu);         // 98（个人上传预检查；无 transfer_id，按单飞槽归属）
    void onUploadInitRespond(PDU* pdu);        // 30
    void onUploadDataRespond(PDU* pdu);        // 32
    void onGroupUploadInitRespond(PDU* pdu);   // 102
    void onDownloadRespond(PDU* pdu);          // 41
    void onDownloadData(PDU* pdu);             // 42
    void onDownloadFinish(PDU* pdu);           // 43
    void onGroupDownloadRespond(PDU* pdu);     // 106
    void onAbortRespond(PDU* pdu);             // 108

    // 控制（上传/下载两方向可用；REQ-EXT-TRANSFER-002 起下载支持暂停/继续/取消/续传）
    void pause(quint64 key);
    void resume(quint64 key);
    void cancel(quint64 key);
    void retry(quint64 key);

    // 41 的 id 落在独立偏移（D2），其余传输族落在统一偏移
    static quint64 transferIdOf(const PDU* pdu);
    static QString stateText(State s);
    static bool isUploadKind(Kind k) { return k == PersonalUpload || k == GroupUpload; }

    void showPanel();

    // ===== 会话间持久化：按用户保存未完成任务，二次登录恢复 =====
    // 登录成功时调用。D-b：不清表重建——同用户（含 空名→真名）保持内存任务并按键合并快照
    // （内存为准、快照补缺）；只有确实是另一个账号时才丢弃上一个账号的内存任务。
    void setCurrentUser(const QString& name);
    // 程序退出前落盘（把内存中非终态任务写进快照）
    void saveSnapshotNow();
    // 本次会话恢复出的任务数（面板提示用）
    int restoredCount() const { return m_restoredCount; }
    QString currentUser() const { return m_currentUser; }

    // 97 预检查为连接级单飞（服务端旧槽）：File 计算完 SHA 后回调提交，按序发送
    void submitFastcheck(quint64 key, const QByteArray& shaHex);
    void failTask(quint64 key, const QString& reason);

    // ===== REQ-EXT-TRANSFER-003 G7：断线任务态收敛（纯客户端，不涉协议） =====
    // 与服务端断开：在途（Running/Hashing）任务即刻失败并归还名额；排队/暂停保持原态；
    // 不删本地分片、不动服务端暂存（留给"重试即续传"）。
    void onConnectionLost(const QString& reason);
    // 重连并重新登录后恢复调度（由 Client 的 LOGIN_RESPOND 分支调用）
    void onConnectionRestored();

signals:
    void tasksChanged();
    void hashDue(quint64 key, const QString& localPath);   // M5：请求后台计算 SHA-256
    void fastcheckDue(quint64 key);                        // 轮到此任务发送 97 预检查

private:
    TransferManager();
    void pump();
    void pumpFastcheck();
    void startTask(Task& t);
    void startGroupHash(Task& t);          // M5：群上传 SHA-256 在后台线程计算
    void sendGroupUploadInit(Task& t);
    void sendPersonalUploadInit(Task& t);
    void finishTask(Task& t, State s, const QString& reason = QString());
    void closeRecv(Task& t);
    void sendPause(quint64 id);
    void sendAbortIfActive(Task& t);
    Task* taskByTransferId(quint64 id);
    bool beginReceiving(Task& t, const QString& fileName);
    // REQ-EXT-TRANSFER-002 C5：续传 = 重开同一分片并 seek 到声明起点（不新建 (N) 文件）
    bool resumeReceiving(Task& t, qint64 offset);
    // REQ-EXT-TRANSFER-002 C2：下载「继续/重试」共用路径（摘旧映射 + 起点 = 分片大小）
    void resumeDownload(Task& t);
    quint64 stableKeyOf(quint64 transferId);

    QHash<quint64, Task> m_tasks;        // key = 稳定行键
    QHash<quint64, quint64> m_idToKey;   // transfer_id → 稳定行键
    QList<quint64> m_uploadQueue;        // 待启动的任务行键（FIFO）
    QList<quint64> m_downloadQueue;
    int m_activeUploads = 0;
    int m_activeDownloads = 0;
    quint64 m_nextId = 1;
    quint64 m_nextKey = 1;
    QWidget* m_panel = nullptr;
    QList<quint64> m_fastcheckQueue;      // 已算出摘要、待发 97 的任务键
    quint64 m_fastcheckInFlight = 0;      // 正在等待 98 的任务键（0 = 无）
    QString m_fastcheckInFlightToken;     // AUD-3：在途 97 的请求标识（与 98 回带比对；不匹配 = 迟到，丢弃）
    bool m_connectionDown = false;        // G7：与服务端断开期间不启动新任务（重连登录后复位）

    // 持久化（transfers_<user>.json，AppLocalDataLocation）
    void saveSnapshot();
    void restoreSavedTasks();
    QString snapshotPath(const QString& user) const;
    QString m_currentUser;
    QString m_lastKnownUser;              // D-a：最近一次有效登录名（m_currentUser 未就位时快照的兜底键）
    QString m_restoredUser;               // 已为本用户恢复过快照（防断线重连重复恢复）
    int m_restoredCount = 0;
};

// 文件级任务列表窗口（§5.5；M6：并发时不再用逐文件 QMessageBox）
class TransferTaskPanel : public QWidget
{
    Q_OBJECT
public:
    explicit TransferTaskPanel(QWidget* parent = nullptr);
public slots:
    void refresh();
private slots:
    void onPause();
    void onResume();
    void onCancel();
    void onRetry();
private:
    quint64 selectedKey() const;
    QTableWidget* m_table;
    QPushButton* m_pauseBtn;
    QPushButton* m_resumeBtn;
    QPushButton* m_cancelBtn;
    QPushButton* m_retryBtn;
    QLabel* m_hint;
};

#endif // TRANSFERTASK_H
