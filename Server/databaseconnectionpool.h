#ifndef DATABASECONNECTIONPOOL_H
#define DATABASECONNECTIONPOOL_H

#include <QHash>
#include <QMutex>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QtSql/QSqlDatabase>
#include <QtSql/QSqlError>
#include <QtGlobal>

class DatabaseConnectionPool;

class PooledSqlConnection
{
public:
    PooledSqlConnection();
    PooledSqlConnection(PooledSqlConnection &&other);
    ~PooledSqlConnection();

    PooledSqlConnection &operator=(PooledSqlConnection &&other);

    PooledSqlConnection(const PooledSqlConnection &) = delete;
    PooledSqlConnection &operator=(const PooledSqlConnection &) = delete;

    bool isValid() const;
    QSqlDatabase database() const;
    QSqlError lastError() const;

private:
    friend class DatabaseConnectionPool;

    explicit PooledSqlConnection(const QString &connectionName);
    void release();

    QString m_connectionName;
};

class DatabaseConnectionPool
{
public:
    static DatabaseConnectionPool &instance();

    void init(const QString &hostName,
              int port,
              const QString &userName,
              const QString &password,
              const QString &databaseName,
              int maxConnectionCount = 8);
    bool testConnection();
    PooledSqlConnection acquire();
    void close();

private:
    friend class PooledSqlConnection;

    DatabaseConnectionPool();
    ~DatabaseConnectionPool();

    DatabaseConnectionPool(const DatabaseConnectionPool &) = delete;
    DatabaseConnectionPool &operator=(const DatabaseConnectionPool &) = delete;

    QString createConnectionName();
    bool openConnection(const QString &connectionName);
    void release(const QString &connectionName);

    QMutex m_mutex;
    QQueue<QString> m_idleConnectionNames;   // 全局共享的空闲连接队列（不再按线程分区）
    QSet<QString> m_allConnectionNames;      // 所有创建过的连接名，用于统一释放
    int m_createdCount;                       // 已创建连接数（受全局上限约束）
    int m_nextConnectionId;                   // 连接名序号
    int m_maxConnectionCount;                 // 全局最大连接数
    bool m_inited;

    QString m_driverName;
    QString m_hostName;
    int m_port;
    QString m_userName;
    QString m_password;
    QString m_databaseName;
};

#endif // DATABASECONNECTIONPOOL_H
