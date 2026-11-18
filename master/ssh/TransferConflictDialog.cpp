#include "TransferConflictDialog.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include "../../common/ui/ThemeManager.h"

TransferConflictDialog::TransferConflictDialog(const QString& itemName, bool isDir, QWidget* parent)
    : QDialog(parent)
{
    setupUi(itemName, isDir);
}

void TransferConflictDialog::setupUi(const QString& itemName, bool isDir) {
    setWindowTitle(tr("Dosya Çakışması"));
    setModal(true);

    auto* mainLay = new QVBoxLayout(this);

    QString msg = isDir 
        ? QString(tr("Hedef dizinde '%1' adında bir klasör zaten mevcut. Ne yapmak istersiniz?")).arg(itemName)
        : QString(tr("Hedef dizinde '%1' adında bir dosya zaten mevcut. Ne yapmak istersiniz?")).arg(itemName);

    auto* lblMsg = new QLabel(msg, this);
    lblMsg->setWordWrap(true);
    mainLay->addWidget(lblMsg);

    auto* btnLay = new QVBoxLayout();

    if (isDir) {
        auto* btnMerge = new QPushButton(tr("Birleştir"), this);
        btnMerge->setProperty("class", "primary");
        btnMerge->setToolTip(tr("Klasör içeriklerini birleştirir. Aynı isimli dosyalar üstüne yazılır."));
        connect(btnMerge, &QPushButton::clicked, this, [this]() {
            m_resolution = Resolution::Merge;
            accept();
        });
        btnLay->addWidget(btnMerge);
    }

    auto* btnOverwrite = new QPushButton(isDir ? tr("Üstüne Yaz (Eskiyi Sil)") : tr("Üstüne Yaz"), this);
    btnOverwrite->setProperty("class", "danger"); // Using danger for overwrite as it's destructive
    btnOverwrite->setToolTip(isDir ? tr("Eski klasörü tamamen siler ve yenisini kopyalar.") : tr("Eski dosyanın üstüne yazar."));
    connect(btnOverwrite, &QPushButton::clicked, this, [this]() {
        m_resolution = Resolution::Overwrite;
        accept();
    });
    btnLay->addWidget(btnOverwrite);

    auto* btnCopy = new QPushButton(tr("Kopya Oluştur"), this);
    btnCopy->setToolTip(tr("Öğeyi farklı bir isimle (örn. (1) ekleyerek) kopyalar."));
    connect(btnCopy, &QPushButton::clicked, this, [this]() {
        m_resolution = Resolution::Copy;
        accept();
    });
    btnLay->addWidget(btnCopy);

    auto* btnSkip = new QPushButton(tr("Atla"), this);
    connect(btnSkip, &QPushButton::clicked, this, [this]() {
        m_resolution = Resolution::Skip;
        accept();
    });
    btnLay->addWidget(btnSkip);

    auto* btnCancel = new QPushButton(tr("İptal"), this);
    connect(btnCancel, &QPushButton::clicked, this, [this]() {
        m_resolution = Resolution::Cancel;
        reject();
    });
    btnLay->addWidget(btnCancel);

    mainLay->addLayout(btnLay);

    m_applyToAllCheckBox = new QCheckBox(tr("Tüm çakışmalar için bunu uygula"), this);
    mainLay->addWidget(m_applyToAllCheckBox);
}

bool TransferConflictDialog::applyToAll() const {
    return m_applyToAllCheckBox->isChecked();
}
