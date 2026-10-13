#include "logging.h"
#include "client.h"
#include "index.h"

#include <QApplication>
#include <QFile>
#include <QString>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    // 默认安静（与 Server 同策略）：排查时设 QT_LOGGING_RULES="xmu.cli.debug=true"
    // Qt 5.14 实测环境变量优先于程序化规则，无需守卫。
    QLoggingCategory::setFilterRules(QStringLiteral("xmu.*.debug=false"));

    // 加载全局 QSS 炫酷主题
    QFile qssFile(":/style.qss");
    if (qssFile.open(QFile::ReadOnly)) {
        QString qssContent = QString::fromUtf8(qssFile.readAll());
        a.setStyleSheet(qssContent);
        qssFile.close();
    } else {
        qCWarning(lcCli) << "QSS 加载失败: 未找到资源 :/style.qss";
    }

    //通过架构函数来获取唯一示例
    Client::getinstance().show();
    return a.exec();
}
