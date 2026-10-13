#ifndef INDEX_H
#define INDEX_H

#include <QWidget>
#include "file.h"
#include "friend.h"
#include "group.h"
namespace Ui {
class Index;
}
//索引界面（内部界面），执行各种操作
class Index : public QWidget
{
    Q_OBJECT

public:
    Index(const Index&)=delete;
    Index& operator=(const Index&)=delete;
    //单例模式，唯一实例
    static Index &getinstance();
    //获取Friend界面的ui指针
    Friend* getFriend();
    //获取File界面的ui指针
    File  * getFile();
    Group * getGroup();
    //获取char的界面的指针
    ~Index();
private:
    explicit Index(QWidget *parent = nullptr);
private slots:
    void on_friend_PB_clicked();

    void on_file_PB_clicked();

    void on_group_PB_clicked();

    void on_pushButton_clicked();

private:
    Ui::Index *ui;
};

#endif // INDEX_H
