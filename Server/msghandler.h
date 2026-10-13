#ifndef MSGHANDLER_H
#define MSGHANDLER_H

#include "protocol.h"
#include "fileobjectstore.h"
#include "resumableuploadsession.h"

#include <QString>
#include <QDateTime>
#include <QPair>
#include <QMap>
#include <QHash>

//用于作为 服务器处理客户端请求 的类
class MsgHandler
{
public:
    MsgHandler();
    //记录客户端发送的请求DPU
    PDU* pdu;
    QString ClientName;

    // ===== REQ-EXT-TRANSFER-001：每连接 per-transfer 上传上下文（D8：id=0 旧单流路径落入同一容器） =====
    struct UploadCtx {
        quint64  id = 0;                 // 0 = 旧单流槽
        ResumableUploadSession session;  // 既有类型，原样复用
        qint64   sessionId = -1;         // upload_session.session_id
        QString  expectedSha;            // 既有 m_uploadExpectedSha
        quint64  groupId = 0;            // 既有 m_groupUploadGroupId
        QString  groupDisplayName;       // 既有 m_groupUploadDisplayName
        bool     writeInFlight = false;  // 既有 m_uploadWriteInFlight（逐 transfer 化）
        qint64   lastProgressMs = 0;     // 空闲回收用
        // AUD-1：97 秒传预检查借 id=0 槽做暂存登记，这不是一次上传，**不占用上传并发名额**。
        // 首次收到该槽的 31 正文（或该槽被随后的 29 采纳）时本标记失效。
        bool     fastcheckPlaceholder = false;
        // AUD-4：107 在写盘在途时到达 → 延迟释放。立即 abort 会让在途写落在 ctx 消失之后，
        // 使暂存物理大小越过 upload_session.confirmed_offset。置位后由 handleUploadWriteFinished 收尾。
        bool     pendingAbort = false;
        // REQ-EXT-TRANSFER-003 S8/D6：进度落库节流（≥1 MiB 或 ≥2 s 才 UPDATE 一次）
        qint64   lastPersistOffset = 0;  // 上次落库时的已确认偏移
        qint64   lastPersistMs = 0;      // 上次落库时刻
    };
    QHash<quint64, UploadCtx> m_uploads;   // key 0 = 旧单流路径
    // handlMsg 按 PDU 落位写入的当前传输 id（29/31/40/101/105/107）；非传输族 = 0
    quint64 m_currentTransferId = 0;
    // 97 未命中建立的会话暂存：随后的 29 INIT（同 parent/display/size）直接采纳，
    // 避免重复建会话，并保留客户端摘要的服务端复算对照（FILE-002 D2）。
    struct PendingFastcheck {
        bool    active = false;
        qint64  sessionId = -1;
        QString shaHex;
        QString parent;
        QString display;
        qint64  size = 0;
    };
    PendingFastcheck m_pendingFastcheck;
    UploadCtx* findUpload(quint64 id);
    // REQ-EXT-TRANSFER-003 S5 补充：同一暂存路径可能被一个以上上下文引用（97 预检查槽 / 幂等重放 /
    // 旧客户端占位）。动文件（rename/delete/truncate）前必须释放**全部**指向该路径的句柄，
    // 否则 Windows 下会被占用句柄拒绝（实测：97 别名句柄让 commitUpload 的 rename 失败 → 会话判 failed）。
    void releaseOtherStagingHandles(quint64 keepId, const QString &stagingPath);
    UploadCtx& currentUpload();            // 不存在则新建（调用方须先过 acquireUploadSlot）
    // 上限（D7）与重复 id（T-17）判定；true = 可占用该槽（id=0 每次重置旧槽）
    bool acquireUploadSlot(quint64 id);
    void abortUpload(quint64 id);          // 释放槽位：upload_session 置 expired，staging 保留
    void reapIdleUploads(qint64 nowMs, int idleSec);   // 空闲回收（返回被回收条目数）
    // AUD-1：上传并发名额计数**排除** 97 预检查占位（id=0 纯暂存槽），其余条目照旧计入
    int  activeUploadCount() const {
        int n = 0;
        for (QHash<quint64, UploadCtx>::const_iterator it = m_uploads.constBegin();
             it != m_uploads.constEnd(); ++it) {
            if (!it->fastcheckPlaceholder) {
                ++n;
            }
        }
        return n;
    }
    //各种处理函数，进行各种类型请求的处理
    PDU* rigist();//注册
    PDU* login(QString& strname, bool& authOk, quint64& loginVersion);//登录（REQ-EXT-SEC-001：authOk 供连接绑定 actor_context；REQ-EXT-SESSION-001：loginVersion 供连接绑定用户域版本）
    PDU* finduser();//查找用户
    PDU *onlineuser(const QString & myname);//在线用户
    PDU* addfriend();//添加用户
    PDU* addfriendFin();//添加好友
    PDU* flushfriend(const  QString& strname);//刷新好友
    PDU* deletefriend();//删除好友
    PDU* chat();//聊天
    PDU* MKDIR();//创建目录
    PDU* deldir(const QString& loginName);//删除目录
    PDU* changedir();//跳转目录
    PDU* flushfile();//刷新文件
    PDU* renamefile(const QString& loginName);//重命名文件
    PDU* sharefile();//分享文件
    PDU* sharefilemove();//分享文件移动
    PDU* movefile(const QString& loginName);//移动文件
    PDU* listdir(const QString& loginName);//列出子目录（移动文件选择目标用）
    PDU* uploadfileinit();//上传文件初始化
    PDU* uploadfilefastcheck();//REQ-EXT-FILE-002：秒传预检查（命中零正文，未命中登记暂存会话）
    PDU* uploadfiledata();//上传文件数据
    PDU* downloadfile();
    PDU* chatmsg();//获取聊天消息
    PDU* captcha();//验证码生成
    QString generateCaptchaCode(int length = 6);
    PDU* getuserinformation();//获取用户信息
    PDU* logoff(bool& revoked);//注销账户（REQ-EXT-SEC-001：授权依据为连接级身份，revoked 供会话撤销）
    // REQ-EXT-GROUP-001：统一解析、校验并处理 60-87 群协议请求。
    PDU* groupRequest(uint requestType);
    // REQ-EXT-GROUP-003：统一解析、校验并处理群文件协议请求（99/101/103/105）。
    // 权限判定全部落在 OperateDB::handleGroupFileOperation（服务端即时判定）。
    PDU* groupFileRequest(uint requestType);

    // ===== REQ-EXT-SEC-001：连接级 actor_context（仅服务端登录成功时写入，客户端 PDU 字段不作为授权依据） =====
    QString m_actorName;   // 服务端派生的操作者用户名
    quint64 m_connId = 0;  // 连接关联标识（拒绝审计用）
    QString actorUserRoot() const;                     // 服务端生成的规范用户根：filesPath/<actor>
    bool pathInActorRoot(const QString& path) const;   // 路径必须落在 actor 本人用户根内
    // REQ-EXT-FILE-001 D5：内容寻址后文件不再物理落用户路径，下载/删除改为
    // 校验其所在目录（目录由 MKDIR 物理创建，必存在）落在 actor 根内。
    bool parentDirInActorRoot(const QString& filePath) const;
    // REQ-EXT-FILE-001 design §6：逻辑条目操作（改名/移动/分享登记）不要求物理存在，
    // 仅做词法规范化 + actor 根越权判断（isPathInSandbox 原样留给仍在磁盘执行的读写）。
    bool logicPathInActorRoot(const QString& path) const;
    PDU* createPublishLink(const QString& loginName);//创建外链分享
    
    // 新增：安全外链分享（BUG-ARC-PUBLINK-001）
    PDU* resolvePublishLink(const QString& loginName);
    PDU* queryFilePublishLink();

    // ===== 验证码：单码复用 + 服务端强制冷却 + 连续错误锁定 =====
    // 参数（与服务端逻辑强一致）
    static const int CAPTCHA_COOLDOWN_SEC = 60;   // 正常冷却：获取新码后 60s 内禁止再取
    static const int CAPTCHA_LOCKOUT_SEC = 600;   // 锁定：连续错误达上限后冷静 10 分钟
    static const int CAPTCHA_MAX_FAILS   = 5;     // 连续错误达到此值触发锁定
    static const int CAPTCHA_EXPIRE_SEC  = 60;    // 单条验证码有效期（秒）

    // 单码状态（每连接仅保留一个当前有效码，重取即让旧码失效）
    QString m_captchaCode;
    QDateTime m_captchaExpiry;

    // 冷却 / 锁定状态
    QDateTime m_captchaNextAllowed;   // 最早可再次成功获取验证码的时间
    QDateTime m_captchaLockUntil;     // 锁定结束时间（isValid() 且 now < 此值 => 锁定中）
    int m_captchaFailCount;           // 连续错误计数

    // 返回：0通过 / -2错误(不存在或不符) / -3过期 / -4已锁定；remainingSec 输出锁定剩余秒数
    int validateCaptcha(const char* caMsg, uint uiMsgLen, int& remainingSec);

    //下载文件时存储通过验证的规范路径
    QString m_downloadCanonicalPath;
    // REQ-EXT-TRANSFER-001 D7：下载方向并发名额是否可用（由 MyTcpSocket 在本连接会话前写入；
    // 超上限时 40 → ret=false、105 → code=-2107，且不建流）
    bool m_downloadSlotAvailable = true;
    // REQ-EXT-TRANSFER-002 S2：客户端在 40/105 声明的续传起点（由 MyTcpSocket 处理前写入；
    // 0 = 从头，旧客户端由 mkPDU 全量清零天然写 0）
    qint64 m_downloadResumeOffset = 0;

    // ===== REQ-EXT-FILE-001 T3/T4：内容寻址上传落定 =====
    // REQ-EXT-FILE-002 D2：上传声明的 SHA-256（= upload_session.client_sha256）现落在 UploadCtx.expectedSha，
    // 只作命中判定与复算对照，永不作为转正依据。
    quint64 m_lastGroupFileId = 0;   // 最近一次群上传落定的 group_file_id（0=无）
    // 上传完成时调用：commitUpload（对象 find-or-create + 条目 + 引用计数，同事务）
    // + finishUploadSession；staging 由 FileObjectStore 转正或丢弃。
    // 返回转正结果（调用方可忽略）；失败时 CommitResult.ok=false。
    FileObjectStore::CommitResult finalizeUploadOnComplete();

    // REQ-FS-003: 统一路径沙箱校验（集中一处，所有路径入口调用）
    // 根与路径分别做 canonical 归一（解析 ../ 与符号链接）后判前缀；
    // 仅当 根存在 且 路径存在 且 canonical(path) 落在 canonical(root) 内（含根本身）时返回 true。
    // root 为空默认整个 filesPath；movefile/listdir 传入用户目录根以保持用户级隔离。
    static bool isPathInSandbox(const QString &path, const QString &root = QString());
};

#endif // MSGHANDLER_H
