#ifndef MYTCPSERVER_H
#define MYTCPSERVER_H

#include <QObject>
#include <QTcpServer>
#include <QMutex>
#include <QHash>
#include <QStringList>
#include "mytcpsocket.h"
//管理所有客户端连接的一个类
class MyTcpServer : public QTcpServer
{
    Q_OBJECT
public:
    //单例模式，一个实例管理所有的客户端连接
    static MyTcpServer& getinstance();
    MyTcpServer(const MyTcpServer&)=delete;
    MyTcpServer& operator=(const MyTcpServer&)=delete;
public:
    //进行信号监听
    void incomingConnection(qintptr handle) override;
    //移除已经和客户端断开连接的socket对象
    void removeSocket(MyTcpSocket* m);
    //添加好友
    void seedAddFriend(char* tarname,PDU*pdu);
    //单次拥有型转发（供 addfriend() 在线路径使用，写后 memfree）
    void sendAddFriendAndRelease(char* tarname, PDU* pdu);
    //信息转发
    void reseed(char*tarname,PDU*pdu);
    // REQ-EXT-GROUP-001：向当前在线群成员广播已提交的成员/邀请事件。
    void sendGroupEvent(const QStringList& users, PDU* pdu);
    // REQ-EXT-GROUP-002 T3：群消息在线广播——锁内仅收集目标连接快照，锁外逐连接入
    // 每连接有界发送队列；队列满只丢该连接实时投递（拉取兜底），不阻塞事务与其他成员。
    void broadcastGroupMessage(const QStringList& users, PDU* pdu);
    // REQ-EXT-SEC-001 M-1: 分享握手登记表——分享人发出邀请时登记 (接收者, 服务端 canonical 源路径)，
    // 接收者同意移动时仅放行有登记的条目并在成功后消费。仅事件循环主线程（socket 处理器）访问，无需加锁。
    void addPendingShare(const QString& receiver, const QString& canonicalSource);
    bool hasPendingShare(const QString& receiver, const QString& canonicalSource) const;
    void consumePendingShare(const QString& receiver, const QString& canonicalSource);
    // REQ-EXT-SESSION-001 D3/D5：用户域登录版本内存登记表（权威持久源=库；进程重启
    // 由重连强制登录自然重建）。登录成功先 set 再踢旧；校验=内存比对，热路径零 DB。
    void setLoginVersion(const QString& user, quint64 ver);
    quint64 currentLoginVersion(const QString& user);   // 无登记返回 0
    void removeLoginVersion(const QString& user);       // 权威会话下线、或账号被注销（删除 user_info 行、计数器归零）时移除对应项
    // 关闭该用户名下已认证且版本≠登记表现值的全部存量连接（锁内快照、出锁 abort，仿僵尸扫描纪律；
    // 现值取单调登记表，并发下自含式正确：低版本登录者也会把自己关掉）
    void closeStaleSockets(const QString& user);
public slots:
    void handleUploadWriteFinished(quint64 connectionId, quint64 transferId,
                                   qint64 offset, qint64 length, bool success);
private:
    MyTcpServer();
    //记录所有已经建立连接的 socket对象 链表
    QList<MyTcpSocket*>tcpsocketlist;
    QMutex m_tcpsocketlistMutex;
    // REQ-EXT-SESSION-001：用户域登录版本登记表（独立短临界区锁；锁序 DB→registry→socketlist）
    QHash<QString,quint64> m_loginVersionRegistry;
    QMutex m_registryMutex;
    quint64 m_nextConnectionId = 0;

    //僵尸连接检测：每隔一段时间扫描，静默超过阈值的连接直接 abort 清理
    qint64 m_zombieTimeoutMs;   // 阈值(ms)，由 zombie_timeout.cfg 读取秒数，默认 90000 (90s)
    QTimer* m_sweepTimer;
    void onSweepTimeout();

    struct PendingShare { QString receiver; QString canonicalSource; qint64 addedMs; };
    QList<PendingShare> m_pendingShares;    // 30 分钟过期、上限 1024，添加时惰性清理
    static const qint64 PENDING_SHARE_TTL_MS = 30*60*1000;
    static const int PENDING_SHARE_MAX = 1024;
};

#endif // MYTCPSERVER_H
