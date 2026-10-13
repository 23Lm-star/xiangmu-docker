#include "folderselectdialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QLabel>
#include <QPushButton>
#include <QFileInfo>
#include <QPixmap>

FolderSelectDialog::FolderSelectDialog(QWidget *parent)
    : QDialog(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);   // QSS 背景在 QWidget 子类上必须显式开启
    setWindowTitle("选择目标文件夹");
    setMinimumWidth(840);
    resize(840, 720);   // REQ-UX-001 A5：面积 4 倍

    m_pathLabel = new QLabel("当前目录：根目录", this);

    m_list = new QListWidget(this);
    connect(m_list, &QListWidget::itemDoubleClicked, this, &FolderSelectDialog::onItemDoubleClicked);

    m_upBtn = new QPushButton("返回上一级", this);
    m_enterBtn = new QPushButton("进入", this);
    m_okBtn = new QPushButton("确定", this);
    m_cancelBtn = new QPushButton("取消", this);

    connect(m_upBtn, &QPushButton::clicked, this, &FolderSelectDialog::onUpClicked);
    connect(m_enterBtn, &QPushButton::clicked, this, &FolderSelectDialog::onEnterClicked);
    connect(m_okBtn, &QPushButton::clicked, this, &FolderSelectDialog::onOkClicked);
    connect(m_cancelBtn, &QPushButton::clicked, this, &FolderSelectDialog::onCancelClicked);

    QHBoxLayout *btnLayout = new QHBoxLayout;
    btnLayout->addWidget(m_upBtn);
    btnLayout->addWidget(m_enterBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(m_okBtn);
    btnLayout->addWidget(m_cancelBtn);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->addWidget(m_pathLabel);
    mainLayout->addWidget(m_list);
    mainLayout->addLayout(btnLayout);
    // 注意：不在构造里 browse("")，避免 listDirRequested 在外部 connect 之前发出而丢失；
    // 首次加载由 File::on_move_PB_clicked 在 connect 之后显式触发。
}

void FolderSelectDialog::browse(const QString &rel)
{
    m_browseRel = rel;
    updatePathLabel();
    emit listDirRequested(rel);
}

void FolderSelectDialog::updatePathLabel()
{
    m_pathLabel->setText(QString("当前目录：%1").arg(m_browseRel.isEmpty() ? "根目录" : m_browseRel));
}

void FolderSelectDialog::handleListDir(PDU *pdu)
{
    m_list->clear();
    int len = pdu->uiMsgLen / sizeof(FileInfo);
    for (int i = 0; i < len; i++) {
        FileInfo fi;
        memcpy(&fi, pdu->caMsg + i * sizeof(FileInfo), sizeof(FileInfo));
        QString name = QString::fromUtf8(fi.caName);
        if (name.isEmpty()) continue;
        QListWidgetItem *item = new QListWidgetItem(QIcon(QPixmap(":/dir.png")), name);
        m_list->addItem(item);
    }
}

void FolderSelectDialog::onItemDoubleClicked(QListWidgetItem *item)
{
    if (!item) return;
    QString name = item->text();
    browse(m_browseRel.isEmpty() ? name : (m_browseRel + "/" + name));
}

void FolderSelectDialog::onEnterClicked()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item) return;
    QString name = item->text();
    browse(m_browseRel.isEmpty() ? name : (m_browseRel + "/" + name));
}

void FolderSelectDialog::onUpClicked()
{
    if (m_browseRel.isEmpty()) return; // 已在根目录
    int idx = m_browseRel.lastIndexOf('/');
    browse(idx < 0 ? "" : m_browseRel.left(idx));
}

void FolderSelectDialog::onOkClicked()
{
    // 优先以"当前高亮的文件夹"作为目标目录；若未高亮任何文件夹，
    // 则以"当前已浏览进入的目录"作为目标（支持直接移到当前目录）。
    // 这样单击文件夹后点确定、与双击/进入后点确定，都能正确落到目标目录，
    // 不会因 m_browseRel 仍为空而误移到根目录。
    QListWidgetItem *item = m_list->currentItem();
    if (item) {
        QString name = item->text();
        m_selectedRel = m_browseRel.isEmpty() ? name : (m_browseRel + "/" + name);
    } else {
        m_selectedRel = m_browseRel;
    }
    accept();
}

void FolderSelectDialog::onCancelClicked()
{
    reject();
}
