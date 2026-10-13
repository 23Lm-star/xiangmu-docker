#include "logging.h"
#include "server.h"
#include <QFile>
#include <QDebug>
#include <QTcpServer>
#include "mytcpserver.h"
Server::Server(QWidget *parent)
    : QWidget(parent)
{
    // 构造函数不再加载配置或启动监听，由 initialize() 统一处理
}

Server::~Server()
{
}

bool Server::loadconfig()
{
    QFile file(":/connect.config");
    if(!file.open(QIODevice::ReadOnly)){
        qCCritical(lcApp) << "connect.config: 打开配置文件失败";
        return false;
    }
    QString s=QString(file.readAll());
    QStringList lst=s.split("\r\n");
    //读取配置文件里面的Ip地址和端口号，还有文件路径
    this->m_strlp=lst[0];
    this->m_usPort=lst[1].toUShort();
    this->filesPath=lst[2];
    //如果配置文件有更多行，读取数据库配置（兼容旧配置）
    if(lst.size() >= 8){
        m_dbHost = lst[3];
        m_dbPort = lst[4].toUShort();
        m_dbName = lst[5];
        m_dbUser = lst[6];
        m_dbPassword = lst[7];
    } else {
        qCCritical(lcApp) << "connect.config: expected 8 lines, got" << lst.size();
        file.close();
        return false;
    }
    qCDebug(lcApp)<<"loadconfig m_strlp"<<m_strlp<<"m_usPort"<<m_usPort<<"filesPath"<<filesPath<<endl;
    file.close();
    return true;
}

bool Server::initialize()
{
    // 第一步：加载配置文件
    if(!loadconfig()){
        qCCritical(lcApp) << "Server::initialize - loadconfig failed";
        return false;
    }
    // 第二步：检查配置完整性
    if(m_strlp.isEmpty() || m_usPort == 0 || filesPath.isEmpty() ||
       m_dbHost.isEmpty() || m_dbPort == 0 || m_dbName.isEmpty() ||
       m_dbUser.isEmpty() || m_dbPassword.isEmpty()){
        qCCritical(lcApp) << "Server::initialize - 配置不完整，请检查 connect.config";
        return false;
    }
    // 第三步：监听端口
    if(!MyTcpServer::getinstance().listen(QHostAddress(m_strlp), m_usPort)){
        qCCritical(lcApp) << "Server::initialize - listen failed:" << MyTcpServer::getinstance().errorString();
        return false;
    }
    qCDebug(lcApp) << "Server::initialize - 服务端启动成功，监听" << m_strlp << ":" << m_usPort;
    return true;
}

Server &Server::getinstance()
{
    static Server instance;
    return instance;
}
