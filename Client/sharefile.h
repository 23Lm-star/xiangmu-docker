#ifndef SHAREFILE_H
#define SHAREFILE_H

#include <QWidget>

namespace Ui {
class ShareFile;
}

class ShareFile : public QWidget
{
    Q_OBJECT

public:
    QString m_strsharedName;
    explicit ShareFile(QWidget *parent = nullptr);
    ~ShareFile();
    void flusfriend();
private slots:
    void on_allchoose_PB_clicked();

    void on_nochoose_PB_clicked();

    void on_output_PB_clicked();

private:
    Ui::ShareFile *ui;
};

#endif // SHAREFILE_H
