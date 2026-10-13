#ifndef SHARELINKDIALOG_H
#define SHARELINKDIALOG_H

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QApplication>
#include <QClipboard>
#include <QToolTip>

class ShareLinkDialog : public QDialog {
    Q_OBJECT
public:
    explicit ShareLinkDialog(const QString &filePath, QWidget *parent = nullptr);
    void setLink(const QString &link);

private slots:
    void onCopyClicked();

private:
    QLineEdit *m_linkEdit;
    QPushButton *m_copyBtn;
};

#endif // SHARELINKDIALOG_H
