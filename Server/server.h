#ifndef SERVER_H
#define SERVER_H

#include <QWidget>
#include <QString>
#include <QTcpServer>
#include <QTcpSocket>
#include "mytcpsocket.h"
#include <QList>
class Server : public QWidget
{
    Q_OBJECT

public:
    //记录ip地址和端口号
    QString m_strlp;
    quint16 m_usPort;
    //服务器所有用户文件路径
    QString filesPath;
    //数据库配置
    QString m_dbHost;
    quint16 m_dbPort;
    QString m_dbName;
    QString m_dbUser;
    QString m_dbPassword;
    ~Server();
    bool loadconfig();//加载配置文件，成功返回 true
    bool initialize();//显式初始化：加载配置、监听端口，全部成功返回 true
    static Server& getinstance();
private:
    Server(QWidget *parent = nullptr);
    Server(const Server& other)=delete;
    Server& operator=(const Server& other)=delete;
};
#endif // SERVER_H
