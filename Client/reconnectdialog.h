#ifndef RECONNECTDIALOG_H
#define RECONNECTDIALOG_H

#include <QDialog>
#include <QString>
#include <QStringList>

class QLabel;
class QPushButton;
class QTimer;
class QShowEvent;
class QCloseEvent;

// 与服务端连接断开时的提示弹窗：显示转圈动画 + "重新连接服务器"按钮。
// 点击按钮通过 reconnectClicked() 信号通知外部发起重连。纯代码实现，无 .ui。
class ReconnectDialog : public QDialog {
    Q_OBJECT
public:
    explicit ReconnectDialog(QWidget *parent = nullptr);

signals:
    void reconnectClicked();   // 用户点击"重新连接服务器"

protected:
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onReconnectBtnClicked();
    void onSpinnerTick();

private:
    void startSpinner();
    void stopSpinner();

    QLabel *m_spinnerLabel;
    QLabel *m_textLabel;
    QPushButton *m_reconnectBtn;
    QTimer *m_spinnerTimer;
    int m_spinnerIdx;
    static const QStringList s_spinnerFrames;
};

#endif // RECONNECTDIALOG_H
