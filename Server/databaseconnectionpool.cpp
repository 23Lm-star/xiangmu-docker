#include "logging.h"
#include "databaseconnectionpool.h"

#include <QDebug>
#include <QThread>

PooledSqlConnection::PooledSqlConnection()
{
}

PooledSqlConnection::PooledSqlConnection(const QString &connectionName)
    : m_connectionName(connectionName)
{
}

PooledSqlConnection::PooledSqlConnection(PooledSqlConnection &&other)
    : m_connectionName(other.m_connectionName)
{
    other.m_connectionName.clear();
}

PooledSqlConnection::~PooledSqlConnection()
{
    release();
}

PooledSqlConnection &PooledSqlConnection::operator=(PooledSqlConnection &&other)
{
    if (this != &other) {
        release();
        m_connectionName = other.m_connectionName;
        other.m_connectionName.clear();
    }
    return *this;
}

bool PooledSqlConnection::isValid() const
{
    return !m_connectionName.isEmpty()
            && QSqlDatabase::contains(m_connectionName)
            && QSqlDatabase::database(m_connectionName).isOpen();
}

QSqlDatabase PooledSqlConnection::database() const
{
    if (m_connectionName.isEmpty() || !QSqlDatabase::contains(m_connectionName)) {
        return QSqlDatabase();
    }
    return QSqlDatabase::database(m_connectionName);
}

QSqlError PooledSqlConnection::lastError() const
{
    return database().lastError();
}

void PooledSqlConnection::release()
{
    if (m_connectionName.isEmpty()) {
        return;
    }

    DatabaseConnectionPool::instance().release(m_connectionName);
    m_connectionName.clear();
}

DatabaseConnectionPool &DatabaseConnectionPool::instance()
{
    static DatabaseConnectionPool pool;
    return pool;
}

DatabaseConnectionPool::DatabaseConnectionPool()
    : m_createdCount(0),
      m_nextConnectionId(0),
      m_maxConnectionCount(8),
      m_inited(false),
      m_driverName("QMYSQL"),
      m_port(3306)
{
}

DatabaseConnectionPool::~DatabaseConnectionPool()
{
    close();
}

void DatabaseConnectionPool::init(const QString &hostName,
                                  int port,
                                  const QString &userName,
                                  const QString &password,
                                  const QString &databaseName,
                                  int maxConnectionCount)
{
    QMutexLocker locker(&m_mutex);
    m_hostName = hostName;
    m_port = port;
    m_userName = userName;
    m_password = password;
    m_databaseName = databaseName;
    m_maxConnectionCount = qMax(1, maxConnectionCount);
    m_inited = true;
}

bool DatabaseConnectionPool::testConnection()
{
    PooledSqlConnection connection = acquire();
    return connection.isValid();
}

PooledSqlConnection DatabaseConnectionPool::acquire()
{
    QMutexLocker locker(&m_mutex);
    if (!m_inited) {
        qCDebug(lcDb) << "database connection pool is not initialized";
        return PooledSqlConnection();
    }

    while (!m_idleConnectionNames.isEmpty()) {
        const QString connectionName = m_idleConnectionNames.dequeue();
        if (!QSqlDatabase::contains(connectionName)) {
            continue;
        }

        QSqlDatabase db = QSqlDatabase::database(connectionName);
        if (!db.isOpen() && !db.open()) {
            qCWarning(lcDb) << "reopen mysql connection failed" << db.lastError().text();
            continue;
        }

        return PooledSqlConnection(connectionName);
    }

    if (m_createdCount >= m_maxConnectionCount) {
        qCDebug(lcDb) << "database connection pool exhausted";
        return PooledSqlConnection();
    }

    const QString connectionName = createConnectionName();
    if (!openConnection(connectionName)) {
        return PooledSqlConnection();
    }

    ++m_createdCount;
    m_allConnectionNames.insert(connectionName);
    return PooledSqlConnection(connectionName);
}

void DatabaseConnectionPool::close()
{
    QMutexLocker locker(&m_mutex);
    const QStringList connectionNames = m_allConnectionNames.values();
    m_idleConnectionNames.clear();
    m_allConnectionNames.clear();
    m_createdCount = 0;

    for (const QString &connectionName : connectionNames) {
        if (!QSqlDatabase::contains(connectionName)) {
            continue;
        }

        QSqlDatabase db = QSqlDatabase::database(connectionName, false);
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
    }

    m_inited = false;
}

QString DatabaseConnectionPool::createConnectionName()
{
    return QString("mysql_pool_%1").arg(++m_nextConnectionId);
}

bool DatabaseConnectionPool::openConnection(const QString &connectionName)
{
    QSqlDatabase db = QSqlDatabase::addDatabase(m_driverName, connectionName);
    db.setHostName(m_hostName);
    db.setPort(m_port);
    db.setUserName(m_userName);
    db.setPassword(m_password);
    db.setDatabaseName(m_databaseName);

    if (db.open()) {
        return true;
    }

    qCWarning(lcDb) << "open mysql connection failed" << db.lastError().text();
    db = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName);
    return false;
}

void DatabaseConnectionPool::release(const QString &connectionName)
{
    QMutexLocker locker(&m_mutex);
    if (!m_allConnectionNames.contains(connectionName)) {
        return;
    }

    m_idleConnectionNames.enqueue(connectionName);
}
