#ifndef FOLDERSELECTDIALOG_H
#define FOLDERSELECTDIALOG_H

#include <QDialog>
#include <QString>
#include "protocol.h"

class QListWidget;
class QLabel;
class QPushButton;
class QListWidgetItem;

// 移动文件时选择目标目录的弹窗：通过 LISTDIR 协议浏览服务端目录树，
// 用户逐级进入子目录，最终"确定"返回当前浏览的相对目录路径。
class FolderSelectDialog : public QDialog {
    Q_OBJECT
public:
    explicit FolderSelectDialog(QWidget *parent = nullptr);
    QString selectedRel() const { return m_selectedRel; }

public slots:
    void handleListDir(PDU *pdu);          // 服务端 LISTDIR_RESPOND 填充列表
    void browse(const QString &rel);       // 触发浏览某相对目录（供外部在连接后调用）

signals:
    void listDirRequested(const QString &rel); // 请求浏览某相对目录

private slots:
    void onItemDoubleClicked(QListWidgetItem *item);
    void onEnterClicked();
    void onUpClicked();
    void onOkClicked();
    void onCancelClicked();

private:
    void updatePathLabel();

    QString m_browseRel;   // 当前浏览的相对根目录路径（空=根目录）
    QString m_selectedRel; // 确定时返回的目标相对目录
    QListWidget *m_list;
    QLabel *m_pathLabel;
    QPushButton *m_upBtn;
    QPushButton *m_enterBtn;
    QPushButton *m_okBtn;
    QPushButton *m_cancelBtn;
};

#endif // FOLDERSELECTDIALOG_H
