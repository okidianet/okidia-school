// master/ui/SmartboardConfirmDialog/SmartboardConfirmDialog.cpp
#include "SmartboardConfirmDialog.h"
#include "../../../common/ui/ThemeManager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QDialogButtonBox>

SmartboardConfirmDialog::SmartboardConfirmDialog(const QMap<QString, QString>& boards, QWidget* parent)
    : QDialog(parent)
    , m_boards(boards) {
    setupUi(boards.size());
}

void SmartboardConfirmDialog::setupUi(int count) {
    setWindowTitle(tr("Akıllı Tahta Yayın Onayı"));
    setWindowIcon(ThemeManager::icon(":/ui/broadcast.svg"));
    resize(480, 360);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);
    mainLayout->setContentsMargins(16, 16, 16, 16);

    auto* headerLabel = new QLabel(this);
    headerLabel->setText(
        tr("<b>%1 adet Akıllı Tahtaya</b> ekran yayını yapmak üzeresiniz.<br>"
           "Ders akışının bozulmaması için yayını alacak tahtaları kontrol edin.<br>"
           "Yayın yapmak <i>istemediğiniz</i> tahtaların işaretini kaldırın:").arg(count)
    );
    headerLabel->setWordWrap(true);
    mainLayout->addWidget(headerLabel);

    m_listWidget = new QListWidget(this);
    for (auto it = m_boards.constBegin(); it != m_boards.constEnd(); ++it) {
        auto* item = new QListWidgetItem(it.value(), m_listWidget);
        item->setData(Qt::UserRole, it.key());
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        item->setIcon(ThemeManager::icon(":/ui/student.svg"));
        m_listWidget->addItem(item);
    }
    mainLayout->addWidget(m_listWidget);

    auto* checkRow = new QHBoxLayout();
    auto* chkAll = new QCheckBox(tr("Tümünü Seç / Kaldır"), this);
    chkAll->setChecked(true);
    connect(chkAll, &QCheckBox::toggled, this, &SmartboardConfirmDialog::onSelectAll);
    checkRow->addWidget(chkAll);
    checkRow->addStretch();
    mainLayout->addLayout(checkRow);

    auto* btnBox = new QDialogButtonBox(this);
    m_btnConfirm = btnBox->addButton(tr("Yayını Başlat"), QDialogButtonBox::AcceptRole);
    m_btnCancel = btnBox->addButton(tr("İptal"), QDialogButtonBox::RejectRole);
    m_btnConfirm->setIcon(ThemeManager::icon(":/ui/play.svg"));
    m_btnCancel->setIcon(ThemeManager::icon(":/ui/stop.svg"));

    connect(btnBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    mainLayout->addWidget(btnBox);
}

void SmartboardConfirmDialog::onSelectAll(bool checked) {
    for (int i = 0; i < m_listWidget->count(); ++i) {
        m_listWidget->item(i)->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    }
}

QStringList SmartboardConfirmDialog::selectedClientIds() const {
    QStringList result;
    for (int i = 0; i < m_listWidget->count(); ++i) {
        auto* item = m_listWidget->item(i);
        if (item->checkState() == Qt::Checked) {
            result << item->data(Qt::UserRole).toString();
        }
    }
    return result;
}
