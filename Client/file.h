#ifndef FILE_H
#define FILE_H
#include "protocol.h"
#include <QWidget>
#include <QShowEvent>
#include <qlistwidget.h>
#include "sharefile.h"
#include <QMenu>
#include "sharelinkdialog.h"
#include "folderselectdialog.h"

class QPushButton;

namespace Ui {
class File;
}
//文件类:
class File : public QWidget
{
    Q_OBJECT

public:
    QString m_strUserPath;//用户路径
    QString m_strCurPath;//当前路径
    QString m_strShareFileName;//分享文件名
    QList<FileInfo*>m_fileInfoList;//当前路径文件信息
    ShareFile* m_shareFile;
    void flushfile();//刷新当前文件->引导刷新
    void updateCurrfile(QList<FileInfo*>qFileinfolist);//利用文件信息列表刷新文件信息
    void flushCurrLE();
    //按当前登录身份重算用户根与当前路径（每次登录成功必须调用；未登录时空转）
    void resetUserPath();
    void onGetShareLink();
    void handleListDirRespond(PDU *pdu);//LISTDIR_RESPOND 转发给目录选择弹窗
    // REQ-EXT-SHARE-001：外链已过期/已撤销时的处置选择（重新生成 / 续期）
    void promptShareLinkRecovery(quint32 state, const QString &note = QString());
    // REQ-EXT-SHARE-001：发 CREATE_PUBLINK；action 非空则在路径 NUL 之后附动作区
    void sendCreatePublishLink(const QString &relativePath, const QString &action, int seconds);
    explicit File(QWidget *parent = nullptr);
    ~File();
protected:
    void showEvent(QShowEvent *event) override;//页面变为可见时重建待刷新列表
private slots:
    void errnoMsg(const QString& Msg);
    //创建目录槽函数
    void on_credit_PB_clicked();
    //刷新文件槽函数
    void on_flush_PB_clicked();
    //删除文件槽函数
    void on_del_PB_clicked();
    //进入当前文件/目录槽函数
    void on_file_LW_itemDoubleClicked(QListWidgetItem *item);
    //返回上级目录槽函数
    void on_back_PB_clicked();
    //重命名文件/目录槽函数
    void on_rename_PB_clicked();

    // 移动文件到目标目录
    void on_move_PB_clicked();
    void requestListDir(const QString &rel);

    void on_inputPath_PB_clicked();

    // 新增：上传槽函数
    void on_upload_PB_clicked();

    // 购物车上传：分多次把文件加入待传清单，点「上传文件」统一入队
    void onAddPendingUpload_clicked();

    void on_download_PB_clicked();

    void on_share_PB_clicked();

    // 新增：生成分享链接
    void on_genlink_PB_clicked();

    // 新增：通过分享链接获取分享文件
    void on_fetchshare_PB_clicked();

    // 新增：右键菜单
    void onFileListContextMenu(const QPoint &pos);

    // REQ-EXT-TRANSFER-001：打开文件级任务列表
    void onTasks_PB_clicked();
    // REQ-EXT-TRANSFER-001：TransferManager 请求在后台计算 SHA-256（M5）
    void onHashDue(quint64 key, const QString& localPath);
    // REQ-EXT-TRANSFER-001：轮到此任务发送 97 秒传预检查（连接级单飞）
    void onFastcheckDue(quint64 key);


private:
    Ui::File *ui;
    bool m_pendingUiRefresh = false;//标记：存在待刷新的文件列表数据
    bool m_uiRefreshQueued = false;//防止重复排入 UI 重建回调
    void rebuildFileList();//集中执行 QListWidget clear() + addItem()
    void scheduleUiRefresh();//合并排队：最多排入一个 UI 刷新回调
    QString selectedFileRelativePath(QListWidgetItem *item);//计算选中文件相对用户根目录的路径
    QString relativePathForName(const QString &fileName) const;//按文件名算相对用户根目录的路径
    FolderSelectDialog* m_moveDlg = nullptr;               // 移动文件时的目标目录选择弹窗
    QPushButton* m_tasksBtn = nullptr;                     // 传输任务列表入口
    QPushButton* m_addUploadBtn = nullptr;                 // 添加待传（购物车上传入口）
    QStringList m_pendingUploads;                          // 待传清单（本地路径）
};

#endif // FILE_H
