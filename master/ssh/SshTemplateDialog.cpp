#include "SshTemplateDialog.h"
#include "../../common/ui/ThemeManager.h"
#include "../../common/utils/OkidiaUtils.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QSettings>
#include <QInputDialog>
#include <QMessageBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QTextEdit>
#include <QLabel>
#include <QGroupBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>

SshTemplateDialog::SshTemplateDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Toplu Komut Şablon Yöneticisi"));
    resize(720, 420);

    m_table = new QTableWidget(0, 4, this);
    m_table->setHorizontalHeaderLabels({tr("Şablon Adı"), tr("Hedef OS"), tr("Nohup"), tr("Komut")});
    OkidiaUtils::setupTableHeaders(m_table, "ssh_template_table_v2", {160, 100, 70, 250}, true);
    m_table->setSortingEnabled(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    m_addBtn = new QPushButton(tr("Ekle"), this);
    m_addBtn->setIcon(ThemeManager::icon(":/ui/add.svg"));
    m_editBtn = new QPushButton(tr("Düzenle"), this);
    m_editBtn->setIcon(ThemeManager::icon(":/ui/edit.svg"));
    m_delBtn = new QPushButton(tr("Sil"), this);
    m_delBtn->setIcon(ThemeManager::icon(":/ui/trash.svg"));
    m_exportBtn = new QPushButton(tr("Dışa Aktar"), this);
    m_exportBtn->setIcon(ThemeManager::icon(":/ui/save_template.svg"));
    m_exportBtn->setToolTip(tr("Şablonları .oktpl / JSON dosyası olarak dışa aktar"));
    m_importBtn = new QPushButton(tr("İçe Aktar"), this);
    m_importBtn->setIcon(ThemeManager::icon(":/ui/file.svg"));
    m_importBtn->setToolTip(tr(".oktpl veya JSON dosyasından şablonları içe aktar"));
    m_refreshBtn = new QPushButton(tr("Yenile"), this);
    m_refreshBtn->setIcon(ThemeManager::icon(":/ui/refresh.svg"));
    m_closeBtn = new QPushButton(tr("Kapat"), this);

    auto* btnLay = new QHBoxLayout();
    btnLay->addWidget(m_addBtn);
    btnLay->addWidget(m_editBtn);
    btnLay->addWidget(m_delBtn);
    btnLay->addWidget(m_exportBtn);
    btnLay->addWidget(m_importBtn);
    btnLay->addWidget(m_refreshBtn);
    btnLay->addStretch();
    btnLay->addWidget(m_closeBtn);

    auto* mainLay = new QVBoxLayout(this);
    mainLay->addWidget(m_table);
    mainLay->addLayout(btnLay);

    connect(m_addBtn, &QPushButton::clicked, this, &SshTemplateDialog::onAdd);
    connect(m_editBtn, &QPushButton::clicked, this, &SshTemplateDialog::onEdit);
    connect(m_delBtn, &QPushButton::clicked, this, &SshTemplateDialog::onDelete);
    connect(m_exportBtn, &QPushButton::clicked, this, &SshTemplateDialog::onExportTemplates);
    connect(m_importBtn, &QPushButton::clicked, this, &SshTemplateDialog::onImportTemplates);
    connect(m_refreshBtn, &QPushButton::clicked, this, &SshTemplateDialog::loadTemplates);
    connect(m_closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_table, &QTableWidget::itemDoubleClicked, this, &SshTemplateDialog::onEdit);

    loadTemplates();
}

void SshTemplateDialog::loadTemplates() {
    bool wasSorting = m_table->isSortingEnabled();
    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);
    QSettings s("okidia", "master");
    int size = s.beginReadArray("ssh_cmd_templates");
    for (int i = 0; i < size; ++i) {
        s.setArrayIndex(i);
        QString name = s.value("name").toString();
        QString targetOs = s.value("target_os", "all").toString().toLower();
        bool isNohup = s.value("nohup", false).toBool();
        QString cmd = s.value("cmd").toString();
        
        QString osDisplay = tr("Tümü");
        QIcon osIcon = ThemeManager::icon(":/ui/settings.svg");
        if (targetOs == "linux") {
            osDisplay = tr("Linux");
            osIcon = ThemeManager::icon(":/ui/linux.svg");
        } else if (targetOs == "windows") {
            osDisplay = tr("Windows");
            osIcon = ThemeManager::icon(":/ui/windows.svg");
        }

        int r = m_table->rowCount();
        m_table->insertRow(r);
        m_table->setItem(r, 0, new OkidiaTableItem(name));
        auto* osItem = new OkidiaTableItem(osIcon, osDisplay);
        osItem->setData(Qt::UserRole + 2, targetOs);
        m_table->setItem(r, 1, osItem);

        auto* nohupItem = new OkidiaTableItem(isNohup ? tr("Evet") : tr("Hayır"));
        nohupItem->setTextAlignment(Qt::AlignCenter);
        nohupItem->setData(Qt::UserRole + 2, isNohup);
        m_table->setItem(r, 2, nohupItem);

        m_table->setItem(r, 3, new OkidiaTableItem(cmd));
    }
    s.endArray();
    m_table->setSortingEnabled(wasSorting);
}

void SshTemplateDialog::saveTemplates() {
    QSettings s("okidia", "master");
    s.beginWriteArray("ssh_cmd_templates");
    for (int i = 0; i < m_table->rowCount(); ++i) {
        s.setArrayIndex(i);
        s.setValue("name", m_table->item(i, 0)->text());
        QString targetOs = m_table->item(i, 1) ? m_table->item(i, 1)->data(Qt::UserRole + 2).toString() : "all";
        s.setValue("target_os", targetOs);
        bool isNohup = m_table->item(i, 2) ? m_table->item(i, 2)->data(Qt::UserRole + 2).toBool() : false;
        s.setValue("nohup", isNohup);
        s.setValue("cmd", m_table->item(i, 3) ? m_table->item(i, 3)->text() : "");
    }
    s.endArray();
    emit templatesChanged();
}

void SshTemplateDialog::onAdd() {
    QDialog d(this);
    d.setWindowTitle(tr("Şablon Ekle"));
    d.resize(460, 270);
    auto* form = new QFormLayout(&d);
    auto* nameEdit = new QLineEdit(&d);
    auto* osCombo = new QComboBox(&d);
    osCombo->addItem(ThemeManager::icon(":/ui/settings.svg"), tr("Tümü"), "all");
    osCombo->addItem(ThemeManager::icon(":/ui/linux.svg"), tr("Linux"), "linux");
    osCombo->addItem(ThemeManager::icon(":/ui/windows.svg"), tr("Windows"), "windows");

    auto* nohupCb = new QCheckBox(tr("Arka Planda Sürdür (nohup)"), &d);
    nohupCb->setToolTip(tr("İşaretliyse, komut uzak sunucuda nohup/disown ile arka planda kesintisiz çalıştırılır."));

    auto* cmdEdit = new QTextEdit(&d);
    cmdEdit->setMaximumHeight(100);
    form->addRow(tr("Şablon Adı:"), nameEdit);
    form->addRow(tr("Hedef OS:"), osCombo);
    form->addRow(tr("Çalışma Modu:"), nohupCb);
    form->addRow(tr("Komut:"), cmdEdit);
    
    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    form->addRow(btns);
    connect(btns, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    
    if (d.exec() == QDialog::Accepted) {
        QString name = nameEdit->text().trimmed();
        QString targetOs = osCombo->currentData().toString();
        bool isNohup = nohupCb->isChecked();
        QString cmd = cmdEdit->toPlainText().trimmed();
        if (!name.isEmpty() && !cmd.isEmpty()) {
            bool wasSorting = m_table->isSortingEnabled();
            m_table->setSortingEnabled(false);
            int r = m_table->rowCount();
            m_table->insertRow(r);
            m_table->setItem(r, 0, new OkidiaTableItem(name));
            auto* osItem = new OkidiaTableItem(osCombo->itemIcon(osCombo->currentIndex()), osCombo->currentText());
            osItem->setData(Qt::UserRole + 2, targetOs);
            m_table->setItem(r, 1, osItem);

            auto* nohupItem = new OkidiaTableItem(isNohup ? tr("Evet") : tr("Hayır"));
            nohupItem->setTextAlignment(Qt::AlignCenter);
            nohupItem->setData(Qt::UserRole + 2, isNohup);
            m_table->setItem(r, 2, nohupItem);

            m_table->setItem(r, 3, new OkidiaTableItem(cmd));
            m_table->setSortingEnabled(wasSorting);
            saveTemplates();
        }
    }
}

void SshTemplateDialog::onEdit() {
    int r = m_table->currentRow();
    if (r < 0) return;
    
    QDialog d(this);
    d.setWindowTitle(tr("Şablon Düzenle"));
    d.resize(460, 270);
    auto* form = new QFormLayout(&d);
    auto* nameEdit = new QLineEdit(m_table->item(r, 0)->text(), &d);
    auto* osCombo = new QComboBox(&d);
    osCombo->addItem(ThemeManager::icon(":/ui/settings.svg"), tr("Tümü"), "all");
    osCombo->addItem(ThemeManager::icon(":/ui/linux.svg"), tr("Linux"), "linux");
    osCombo->addItem(ThemeManager::icon(":/ui/windows.svg"), tr("Windows"), "windows");

    QString currentOs = m_table->item(r, 1) ? m_table->item(r, 1)->data(Qt::UserRole + 2).toString() : "all";
    int osIdx = osCombo->findData(currentOs);
    if (osIdx >= 0) osCombo->setCurrentIndex(osIdx);

    bool currentNohup = m_table->item(r, 2) ? m_table->item(r, 2)->data(Qt::UserRole + 2).toBool() : false;
    auto* nohupCb = new QCheckBox(tr("Arka Planda Sürdür (nohup)"), &d);
    nohupCb->setChecked(currentNohup);
    nohupCb->setToolTip(tr("İşaretliyse, komut uzak sunucuda nohup/disown ile arka planda kesintisiz çalıştırılır."));

    auto* cmdEdit = new QTextEdit(m_table->item(r, 3) ? m_table->item(r, 3)->text() : "", &d);
    cmdEdit->setMaximumHeight(100);
    form->addRow(tr("Şablon Adı:"), nameEdit);
    form->addRow(tr("Hedef OS:"), osCombo);
    form->addRow(tr("Çalışma Modu:"), nohupCb);
    form->addRow(tr("Komut:"), cmdEdit);
    
    auto* btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    form->addRow(btns);
    connect(btns, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    connect(btns, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    
    if (d.exec() == QDialog::Accepted) {
        QString name = nameEdit->text().trimmed();
        QString targetOs = osCombo->currentData().toString();
        bool isNohup = nohupCb->isChecked();
        QString cmd = cmdEdit->toPlainText().trimmed();
        if (!name.isEmpty() && !cmd.isEmpty()) {
            m_table->item(r, 0)->setText(name);
            auto* osItem = m_table->item(r, 1);
            if (!osItem) {
                osItem = new OkidiaTableItem();
                m_table->setItem(r, 1, osItem);
            }
            osItem->setText(osCombo->currentText());
            osItem->setIcon(osCombo->itemIcon(osCombo->currentIndex()));
            osItem->setData(Qt::UserRole + 2, targetOs);

            auto* nohupItem = m_table->item(r, 2);
            if (!nohupItem) {
                nohupItem = new OkidiaTableItem();
                nohupItem->setTextAlignment(Qt::AlignCenter);
                m_table->setItem(r, 2, nohupItem);
            }
            nohupItem->setText(isNohup ? tr("Evet") : tr("Hayır"));
            nohupItem->setData(Qt::UserRole + 2, isNohup);

            if (!m_table->item(r, 3)) {
                m_table->setItem(r, 3, new OkidiaTableItem());
            }
            m_table->item(r, 3)->setText(cmd);
            saveTemplates();
        }
    }
}

void SshTemplateDialog::onDelete() {
    auto selectedRows = m_table->selectionModel()->selectedRows();
    if (selectedRows.isEmpty()) {
        int r = m_table->currentRow();
        if (r >= 0) {
            if (QMessageBox::question(this, tr("Sil"), tr("Seçili şablonu silmek istediğinize emin misiniz?")) == QMessageBox::Yes) {
                m_table->removeRow(r);
                saveTemplates();
            }
        }
        return;
    }

    QString msg = (selectedRows.size() == 1)
        ? tr("Seçili şablonu silmek istediğinize emin misiniz?")
        : QString(tr("Seçili %1 adet şablonu silmek istediğinize emin misiniz?")).arg(selectedRows.size());

    if (QMessageBox::question(this, tr("Sil"), msg) != QMessageBox::Yes) {
        return;
    }

    // Satır indekslerini tersten sıralayarak sil (indeks kaymalarını önlemek için)
    QList<int> rows;
    for (const auto& index : selectedRows) {
        rows.append(index.row());
    }
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    for (int r : rows) {
        m_table->removeRow(r);
    }
    saveTemplates();
}

void SshTemplateDialog::onExportTemplates() {
    if (m_table->rowCount() == 0) {
        QMessageBox::information(this, tr("Dışa Aktar"), tr("Dışa aktarılacak kayıtlı hiçbir şablon bulunmuyor."));
        return;
    }

    auto selectedRows = m_table->selectionModel()->selectedRows();
    QList<int> targetRows;
    if (selectedRows.size() > 1 || (selectedRows.size() == 1 && m_table->rowCount() > 1)) {
        if (selectedRows.size() > 1) {
            for (const auto& index : selectedRows) {
                targetRows.append(index.row());
            }
            std::sort(targetRows.begin(), targetRows.end());
        } else {
            for (int i = 0; i < m_table->rowCount(); ++i) targetRows.append(i);
        }
    } else {
        for (int i = 0; i < m_table->rowCount(); ++i) targetRows.append(i);
    }

    QString filter = tr("Okidia Şablon Dosyaları (*.oktpl);;JSON Dosyaları (*.json);;Tüm Dosyalar (*)");
    QString defaultFileName = QString("okidia_ssh_templates_%1.oktpl")
                                .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmm"));
    QString filePath = OkidiaUtils::getSaveFileName(this, tr("Şablonları Dışa Aktar"), "ssh_templates",
                                                    defaultFileName, filter);
    if (filePath.isEmpty()) return;

    // Kullanıcı uzantı yazmadıysa otomatik olarak .oktpl ekle
    if (!filePath.endsWith(".oktpl", Qt::CaseInsensitive) && !filePath.endsWith(".json", Qt::CaseInsensitive)) {
        filePath += ".oktpl";
    }

    QJsonArray tplArray;
    for (int r : targetRows) {
        QJsonObject obj;
        obj["name"] = m_table->item(r, 0)->text();
        QString targetOs = m_table->item(r, 1) ? m_table->item(r, 1)->data(Qt::UserRole + 2).toString() : "all";
        obj["target_os"] = targetOs;
        bool isNohup = m_table->item(r, 2) ? m_table->item(r, 2)->data(Qt::UserRole + 2).toBool() : false;
        obj["nohup"] = isNohup;
        obj["cmd"] = m_table->item(r, 3) ? m_table->item(r, 3)->text() : "";
        tplArray.append(obj);
    }

    QJsonObject root;
    root["type"] = "okidia_ssh_templates";
    root["version"] = 2;
    root["exported_at"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root["templates"] = tplArray;

    QJsonDocument doc(root);
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, tr("Hata"), tr("Dosya yazma amacıyla açılamadı:\n") + file.errorString());
        return;
    }

    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    QMessageBox::information(this, tr("Başarılı"),
                             QString(tr("%1 adet komut şablonu başarıyla dışa aktarıldı:\n%2"))
                                 .arg(tplArray.size())
                                 .arg(filePath));
}

void SshTemplateDialog::onImportTemplates() {
    QString filter = tr("Okidia Şablon Dosyaları (*.oktpl *.json);;Okidia Şablonları (*.oktpl);;JSON Dosyaları (*.json);;Tüm Dosyalar (*)");
    QString filePath = OkidiaUtils::getOpenFileName(this, tr("Şablonları İçe Aktar"), "ssh_templates", filter);
    if (filePath.isEmpty()) return;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Hata"), tr("Dosya okuma amacıyla açılamadı:\n") + file.errorString());
        return;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseErr;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseErr);
    if (parseErr.error != QJsonParseError::NoError) {
        QMessageBox::warning(this, tr("Geçersiz Dosya"), tr("JSON ayrıştırma hatası:\n") + parseErr.errorString());
        return;
    }

    QJsonArray tplArray;
    if (doc.isObject()) {
        QJsonObject root = doc.object();
        if (root.contains("templates") && root["templates"].isArray()) {
            tplArray = root["templates"].toArray();
        }
    } else if (doc.isArray()) {
        tplArray = doc.array();
    }

    if (tplArray.isEmpty()) {
        QMessageBox::warning(this, tr("Uyarı"), tr("Dosya içerisinde içe aktarılacak geçerli bir komut şablonu bulunamadı."));
        return;
    }

    int importedCount = 0;
    int updatedCount = 0;
    int skippedCount = 0;

    bool applyToAllConflicts = false;
    enum ConflictAction { None, OverwriteAll, RenameAll, SkipAll };
    ConflictAction globalAction = None;

    bool wasSorting = m_table->isSortingEnabled();
    m_table->setSortingEnabled(false);

    for (const auto& val : tplArray) {
        if (!val.isObject()) continue;
        QJsonObject obj = val.toObject();

        QString origName = obj["name"].toString().trimmed();
        QString targetOs = obj["target_os"].toString().trimmed().toLower();
        bool isNohup = obj["nohup"].toBool(false);
        QString cmd = obj["cmd"].toString().trimmed();

        if (origName.isEmpty() || cmd.isEmpty()) continue;
        if (targetOs.isEmpty()) targetOs = "all";

        QString osDisplay = tr("Tümü");
        QIcon osIcon = ThemeManager::icon(":/ui/settings.svg");
        if (targetOs == "linux") {
            osDisplay = tr("Linux");
            osIcon = ThemeManager::icon(":/ui/linux.svg");
        } else if (targetOs == "windows") {
            osDisplay = tr("Windows");
            osIcon = ThemeManager::icon(":/ui/windows.svg");
        }

        // Aynı isim ve aynı hedef işletim sisteminde şablon var mı kontrol et
        int foundRow = -1;
        for (int i = 0; i < m_table->rowCount(); ++i) {
            QString rowName = m_table->item(i, 0) ? m_table->item(i, 0)->text() : "";
            QString rowOs = m_table->item(i, 1) ? m_table->item(i, 1)->data(Qt::UserRole + 2).toString() : "all";
            if (rowName.compare(origName, Qt::CaseInsensitive) == 0 && rowOs == targetOs) {
                foundRow = i;
                break;
            }
        }

        if (foundRow != -1) {
            // İsim ve OS çakışması durumu
            enum Choice { Overwrite, Rename, Skip };
            Choice chosenChoice = Rename;
            QString finalName = origName;

            if (applyToAllConflicts && globalAction != None) {
                if (globalAction == OverwriteAll) chosenChoice = Overwrite;
                else if (globalAction == RenameAll) chosenChoice = Rename;
                else if (globalAction == SkipAll) chosenChoice = Skip;
            } else {
                // Kullanıcıya şablon çakışmasını sor ve isim değiştirme imkanı sun
                QDialog conflictDlg(this);
                conflictDlg.setWindowTitle(tr("Şablon Çakışması"));
                conflictDlg.setMinimumWidth(460);

                auto* dlgLay = new QVBoxLayout(&conflictDlg);
                dlgLay->setSpacing(12);

                auto* headerLbl = new QLabel(
                    QString(tr("<b>'%1'</b> isimli ve <b>[%2]</b> kategorisinde bir şablon sistemde zaten kayıtlı.<br>"
                               "İçe aktarılan komuta farklı bir isim verebilir, mevcut olanı güncelleyebilir veya atlayabilirsiniz."))
                        .arg(origName)
                        .arg(osDisplay), &conflictDlg);
                headerLbl->setWordWrap(true);
                dlgLay->addWidget(headerLbl);

                // İsim düzenleme alanı (varsayılan olarak dosyadaki isim önerilir)
                auto* nameForm = new QFormLayout();
                auto* customNameEdit = new QLineEdit(origName, &conflictDlg);
                nameForm->addRow(tr("Yeni / Özel Şablon Adı:"), customNameEdit);
                dlgLay->addLayout(nameForm);

                // Komut önizleme
                auto* previewGroup = new QGroupBox(tr("İçe Aktarılan Komut Önizlemesi"), &conflictDlg);
                auto* previewLay = new QVBoxLayout(previewGroup);
                auto* previewEdit = new QTextEdit(cmd, previewGroup);
                previewEdit->setReadOnly(true);
                previewEdit->setMaximumHeight(70);
                previewLay->addWidget(previewEdit);
                dlgLay->addWidget(previewGroup);

                // Tüm çakışmalar için hatırla seçeneği
                auto* applyAllCb = new QCheckBox(tr("Kalan tüm çakışmalarda bu tercihi uygula"), &conflictDlg);
                dlgLay->addWidget(applyAllCb);

                // İşlem Butonları
                auto* btnLay = new QHBoxLayout();
                btnLay->addStretch();

                auto* renameBtn = new QPushButton(ThemeManager::icon(":/ui/edit.svg"), tr("Yeni İsimle Kaydet"), &conflictDlg);
                auto* overwriteBtn = new QPushButton(ThemeManager::icon(":/ui/save_template.svg"), tr("Üzerine Yaz"), &conflictDlg);
                auto* skipBtn = new QPushButton(ThemeManager::icon(":/ui/cancel.svg"), tr("Atla"), &conflictDlg);

                renameBtn->setDefault(true);
                btnLay->addWidget(renameBtn);
                btnLay->addWidget(overwriteBtn);
                btnLay->addWidget(skipBtn);
                dlgLay->addLayout(btnLay);

                int dialogResult = -1;
                connect(renameBtn, &QPushButton::clicked, [&]() {
                    dialogResult = 1;
                    conflictDlg.accept();
                });
                connect(overwriteBtn, &QPushButton::clicked, [&]() {
                    dialogResult = 2;
                    conflictDlg.accept();
                });
                connect(skipBtn, &QPushButton::clicked, [&]() {
                    dialogResult = 3;
                    conflictDlg.reject();
                });

                conflictDlg.exec();

                if (applyAllCb->isChecked()) {
                    applyToAllConflicts = true;
                    if (dialogResult == 1) globalAction = RenameAll;
                    else if (dialogResult == 2) globalAction = OverwriteAll;
                    else globalAction = SkipAll;
                }

                if (dialogResult == 1) {
                    chosenChoice = Rename;
                    finalName = customNameEdit->text().trimmed();
                    if (finalName.isEmpty() || finalName.compare(origName, Qt::CaseInsensitive) == 0) {
                        // Eğer kullanıcı adı değiştirmediyse o işletim sistemi kategorisinde benzersiz olacak şekilde kopya eki ver
                        int copyIdx = 1;
                        do {
                            finalName = QString("%1 (%2)").arg(origName).arg(copyIdx++);
                            bool exists = false;
                            for (int k = 0; k < m_table->rowCount(); ++k) {
                                QString kName = m_table->item(k, 0) ? m_table->item(k, 0)->text() : "";
                                QString kOs = m_table->item(k, 1) ? m_table->item(k, 1)->data(Qt::UserRole + 2).toString() : "all";
                                if (kName.compare(finalName, Qt::CaseInsensitive) == 0 && kOs == targetOs) {
                                    exists = true;
                                    break;
                                }
                            }
                            if (!exists) break;
                        } while (true);
                    }
                } else if (dialogResult == 2) {
                    chosenChoice = Overwrite;
                } else {
                    chosenChoice = Skip;
                }
            }

            if (chosenChoice == Skip) {
                skippedCount++;
                continue;
            } else if (chosenChoice == Overwrite) {
                auto* osItem = m_table->item(foundRow, 1);
                if (!osItem) {
                    osItem = new OkidiaTableItem();
                    m_table->setItem(foundRow, 1, osItem);
                }
                osItem->setText(osDisplay);
                osItem->setIcon(osIcon);
                osItem->setData(Qt::UserRole + 2, targetOs);

                auto* nohupItem = m_table->item(foundRow, 2);
                if (!nohupItem) {
                    nohupItem = new OkidiaTableItem();
                    nohupItem->setTextAlignment(Qt::AlignCenter);
                    m_table->setItem(foundRow, 2, nohupItem);
                }
                nohupItem->setText(isNohup ? tr("Evet") : tr("Hayır"));
                nohupItem->setData(Qt::UserRole + 2, isNohup);

                if (!m_table->item(foundRow, 3)) {
                    m_table->setItem(foundRow, 3, new OkidiaTableItem());
                }
                m_table->item(foundRow, 3)->setText(cmd);
                updatedCount++;
            } else { // Rename
                if (applyToAllConflicts && globalAction == RenameAll) {
                    int copyIdx = 1;
                    do {
                        finalName = QString("%1 (%2)").arg(origName).arg(copyIdx++);
                        bool exists = false;
                        for (int k = 0; k < m_table->rowCount(); ++k) {
                            QString kName = m_table->item(k, 0) ? m_table->item(k, 0)->text() : "";
                            QString kOs = m_table->item(k, 1) ? m_table->item(k, 1)->data(Qt::UserRole + 2).toString() : "all";
                            if (kName.compare(finalName, Qt::CaseInsensitive) == 0 && kOs == targetOs) {
                                exists = true;
                                break;
                            }
                        }
                        if (!exists) break;
                    } while (true);
                }

                int r = m_table->rowCount();
                m_table->insertRow(r);
                m_table->setItem(r, 0, new OkidiaTableItem(finalName));
                auto* osItem = new OkidiaTableItem(osIcon, osDisplay);
                osItem->setData(Qt::UserRole + 2, targetOs);
                m_table->setItem(r, 1, osItem);

                auto* nohupItem = new OkidiaTableItem(isNohup ? tr("Evet") : tr("Hayır"));
                nohupItem->setTextAlignment(Qt::AlignCenter);
                nohupItem->setData(Qt::UserRole + 2, isNohup);
                m_table->setItem(r, 2, nohupItem);

                m_table->setItem(r, 3, new OkidiaTableItem(cmd));
                importedCount++;
            }
        } else {
            // Çakışma yok, doğrudan yeni şablon satırı ekle
            int r = m_table->rowCount();
            m_table->insertRow(r);
            m_table->setItem(r, 0, new OkidiaTableItem(origName));
            auto* osItem = new OkidiaTableItem(osIcon, osDisplay);
            osItem->setData(Qt::UserRole + 2, targetOs);
            m_table->setItem(r, 1, osItem);

            auto* nohupItem = new OkidiaTableItem(isNohup ? tr("Evet") : tr("Hayır"));
            nohupItem->setTextAlignment(Qt::AlignCenter);
            nohupItem->setData(Qt::UserRole + 2, isNohup);
            m_table->setItem(r, 2, nohupItem);

            m_table->setItem(r, 3, new OkidiaTableItem(cmd));
            importedCount++;
        }
    }

    m_table->setSortingEnabled(wasSorting);

    if (importedCount > 0 || updatedCount > 0) {
        saveTemplates();
        QString detail = QString(tr("İçe aktarma tamamlandı!\n• %1 yeni şablon eklendi\n• %2 mevcut şablon güncellendi"))
                            .arg(importedCount)
                            .arg(updatedCount);
        if (skippedCount > 0) {
            detail += QString(tr("\n• %1 şablon atlandı")).arg(skippedCount);
        }
        QMessageBox::information(this, tr("Başarılı"), detail);
    } else if (skippedCount > 0) {
        QMessageBox::information(this, tr("Bilgi"), tr("Çakışan şablonlar atlandı, hiçbir değişiklik yapılmadı."));
    } else {
        QMessageBox::warning(this, tr("İçe Aktarma"), tr("Dosyadan hiçbir şablon içeri aktarılamadı."));
    }
}
