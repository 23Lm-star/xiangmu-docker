#include "reconnectdialog.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QFont>
#include <QShowEvent>
#include <QCloseEvent>

// 转圈字符序列（ASCII，任意字体都能显示），show 时循环切换模拟旋转
const QStringList ReconnectDialog::s_spinnerFrames = QStringList()
    << "|" << "/" << "-" << "\\";

ReconnectDialog::ReconnectDialog(QWidget *parent)
    : QDialog(parent)
    , m_spinnerIdx(0)
{
    setAttribute(Qt::WA_StyledBackground, true);   // QSS 背景在 QWidget 子类上必须显式开启
    setWindowTitle("与服务器断开");
    setFixedSize(640, 360);   // REQ-UX-001 A5：面积 4 倍
    setModal(true);   // 模态：弹窗期间禁止操作主窗口，但事件循环仍运行（心跳/重连不阻塞）

    m_spinnerLabel = new QLabel("|", this);
    m_spinnerLabel->setAlignment(Qt::AlignCenter);
    QFont f = m_spinnerLabel->font();
    f.setPointSize(30);
    m_spinnerLabel->setFont(f);

    m_textLabel = new QLabel("与服务器连接已断开\n点击下方按钮尝试重新连接", this);
    m_textLabel->setAlignment(Qt::AlignCenter);

    m_reconnectBtn = new QPushButton("重新连接服务器", this);
    connect(m_reconnectBtn, &QPushButton::clicked, this, &ReconnectDialog::onReconnectBtnClicked);

    m_spinnerTimer = new QTimer(this);
    m_spinnerTimer->setInterval(120);
    connect(m_spinnerTimer, &QTimer::timeout, this, &ReconnectDialog::onSpinnerTick);

    QVBoxLayout *main = new QVBoxLayout(this);
    main->addWidget(m_spinnerLabel);
    main->addWidget(m_textLabel);
    main->addSpacing(10);
    main->addWidget(m_reconnectBtn);
}

void ReconnectDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    startSpinner();
}

void ReconnectDialog::closeEvent(QCloseEvent *event)
{
    stopSpinner();
    QDialog::closeEvent(event);
}

void ReconnectDialog::startSpinner()
{
    if (!m_spinnerTimer->isActive())
        m_spinnerTimer->start();
}

void ReconnectDialog::stopSpinner()
{
    m_spinnerTimer->stop();
}

void ReconnectDialog::onSpinnerTick()
{
    m_spinnerIdx = (m_spinnerIdx + 1) % s_spinnerFrames.size();
    m_spinnerLabel->setText(s_spinnerFrames.at(m_spinnerIdx));
}

void ReconnectDialog::onReconnectBtnClicked()
{
    emit reconnectClicked();
}
