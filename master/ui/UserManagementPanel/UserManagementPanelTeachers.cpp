// master/ui/UserManagementPanel/UserManagementPanelTeachers.cpp
#include "UserManagementPanel.h"
#include "../../../common/utils/Logger.h"
#include "../../../common/utils/OkidiaUtils.h"
#include "../../../common/utils/QrCodeGenerator.h"
#include "../../../common/db/DbRecords.h"
#include "../../../common/db/OkidiaDatabase.h"
#include "../../session/SessionManager.h"
#include "../../../common/ui/ThemeManager.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QHeaderView>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QCheckBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QDateTime>
#include <QCryptographicHash>
#include <QInputDialog>
#include <QProgressDialog>
#include <QEventLoop>
#include <QTimer>

void UserManagementPanel::setupTeacherTab() {
    m_teacherTable = new QTableWidget(0, 5, this);
    m_teacherTable->setHorizontalHeaderLabels({tr("Ad Soyad"), tr("Kullanıcı Adı"), tr("Branş"), tr("Durum"), tr("Kayıt Tarihi")});
    OkidiaUtils::setupTableHeaders(m_teacherTable, "teacher_mgmt_table", {160, 140, 120, 90, 120}, true);
    m_teacherTable->setSortingEnabled(true);
    m_teacherTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_teacherTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_teacherTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_teacherTable->setAlternatingRowColors(true);

    m_btnTeacherAdd      = new QPushButton(tr("Öğretmen Ekle"), this);
    m_btnTeacherAdd->setIcon(ThemeManager::icon(":/ui/add.svg"));
    m_btnTeacherEdit     = new QPushButton(tr("Düzenle"), this);
    m_btnTeacherEdit->setIcon(ThemeManager::icon(":/ui/edit.svg"));
    m_btnTeacherDelete   = new QPushButton(tr("Sil"), this);
    m_btnTeacherDelete->setIcon(ThemeManager::icon(":/ui/trash.svg"));
    m_btnTeacherMobileQr = new QPushButton(tr("Mobil Eşleştirme QR"), this);
    m_btnTeacherMobileQr->setIcon(ThemeManager::icon(":/ui/share.svg"));
    m_btnTeacherMobileQr->setToolTip(tr("Öğretmenin Okidia Mobil uygulaması için eşleştirme QR kodunu göster"));
    m_btnTeacherSync     = new QPushButton(tr("Tüm Tahtalara Eşitle"), this);
    m_btnTeacherSync->setIcon(ThemeManager::icon(":/ui/refresh.svg"));
    m_btnTeacherSync->setToolTip(tr("Öğretmen listesini ve kilit açma yetkilerini tüm akıllı tahtalara gönderir"));

    m_btnTeacherAdd->setProperty("class", "primary");
    m_btnTeacherMobileQr->setProperty("class", "primary");

    m_btnTeacherEdit->setEnabled(false);
    m_btnTeacherDelete->setEnabled(false);
    m_btnTeacherMobileQr->setEnabled(false);

    connect(m_teacherTable, &QTableWidget::currentCellChanged,
            this, &UserManagementPanel::onTeacherSelectionChanged);
    m_teacherTable->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_teacherTable, &QTableWidget::customContextMenuRequested,
            this, &UserManagementPanel::onTeacherContextMenu);
    connect(m_btnTeacherAdd,      &QPushButton::clicked, this, &UserManagementPanel::onAddTeacher);
    connect(m_btnTeacherEdit,     &QPushButton::clicked, this, &UserManagementPanel::onEditTeacher);
    connect(m_btnTeacherDelete,   &QPushButton::clicked, this, &UserManagementPanel::onDeleteTeacher);
    connect(m_btnTeacherMobileQr, &QPushButton::clicked, this, &UserManagementPanel::onShowTeacherMobilePairQr);
    connect(m_btnTeacherSync,     &QPushButton::clicked, this, &UserManagementPanel::onSyncTeachersToSmartboards);
}

void UserManagementPanel::loadTeachers() {
    m_teachers = OkidiaDatabase::instance().allTeachers();
    m_teacherTable->setSortingEnabled(false);
    m_teacherTable->setRowCount(0);

    for (const auto& t : m_teachers) {
        const int row = m_teacherTable->rowCount();
        m_teacherTable->insertRow(row);
        m_teacherTable->setItem(row, 0, new OkidiaTableItem(t.displayName));
        m_teacherTable->setItem(row, 1, new OkidiaTableItem(t.username));
        m_teacherTable->setItem(row, 2, new OkidiaTableItem(t.branch.isEmpty() ? tr("—") : t.branch));

        auto* statusItem = new OkidiaTableItem(t.isActive ? tr("Aktif") : tr("Pasif"));
        statusItem->setTextAlignment(Qt::AlignCenter);
        m_teacherTable->setItem(row, 3, statusItem);

        const QString regDate = t.createdAt > 0
            ? QDateTime::fromSecsSinceEpoch(t.createdAt).toString("dd.MM.yyyy HH:mm")
            : "—";
        m_teacherTable->setItem(row, 4, new OkidiaTableItem(regDate));

        m_teacherTable->item(row, 0)->setData(Qt::UserRole, t.id);
    }
    m_teacherTable->setSortingEnabled(true);
}

QString UserManagementPanel::selectedTeacherId() const {
    const int row = m_teacherTable->currentRow();
    if (row < 0) return {};
    auto* item = m_teacherTable->item(row, 0);
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void UserManagementPanel::onTeacherSelectionChanged() {
    const QString selId = selectedTeacherId();
    const bool hasSel   = !selId.isEmpty();
    const bool isYM     = (m_activeUser.role == "YM");

    if (m_btnTeacherEdit)     m_btnTeacherEdit->setEnabled(isYM && hasSel);
    if (m_btnTeacherDelete)   m_btnTeacherDelete->setEnabled(isYM && hasSel);
    if (m_btnTeacherMobileQr) m_btnTeacherMobileQr->setEnabled(hasSel);
}

void UserManagementPanel::onAddTeacher() {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Yeni Öğretmen Ekle"));
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dlg.setMinimumWidth(440);

    auto* form = new QFormLayout(&dlg);
    form->setContentsMargins(20, 20, 20, 16);
    form->setSpacing(12);

    auto* nameEdit   = new QLineEdit(&dlg);
    nameEdit->setPlaceholderText(tr("Örn: Ahmet Yılmaz"));

    auto* userEdit   = new QLineEdit(&dlg);
    userEdit->setPlaceholderText(tr("Örn: ahmet.yilmaz"));

    auto* userLay = new QHBoxLayout();
    userLay->addWidget(userEdit);
    auto* scanBtn = new QPushButton(tr("🔍 Tahtadan Çek"), &dlg);
    scanBtn->setToolTip(tr("Çevrimiçi bir akıllı tahtadaki yerel kullanıcıları tara ve otomatik doldur"));
    userLay->addWidget(scanBtn);

    auto* passEdit = new QLineEdit(&dlg);
    passEdit->setEchoMode(QLineEdit::Password);
    passEdit->setPlaceholderText(tr("Tahta Oturum Şifresi (İsteğe bağlı)"));

    auto* passNote = new QLabel(tr("<small style='color: #8b949e;'>Not: Bu şifre yalnızca akıllı tahtada henüz böyle bir kullanıcı yoksa ve yeni oluşturulacaksa geçerlidir. Tahtada zaten var olan kullanıcıların şifresi asla değiştirilmez.</small>"), &dlg);
    passNote->setWordWrap(true);

    auto* branchEdit = new QLineEdit(&dlg);
    branchEdit->setPlaceholderText(tr("Örn: Matematik, Fizik"));

    auto* activeCheck = new QCheckBox(tr("Aktif (Akıllı tahtalara giriş yapabilir)"), &dlg);
    activeCheck->setChecked(true);

    form->addRow(tr("Ad Soyad:"),       nameEdit);
    form->addRow(tr("Kullanıcı Adı:"),  userLay);
    form->addRow(tr("Tahta Şifresi:"),  passEdit);
    form->addRow(QString(),             passNote);
    form->addRow(tr("Branş:"),          branchEdit);
    form->addRow(QString(),             activeCheck);

    auto* errorLbl = new QLabel(&dlg);
    errorLbl->setProperty("status", "error");
    form->addRow(errorLbl);

    connect(scanBtn, &QPushButton::clicked, [&]() {
        if (!m_session) return;
        auto states = m_session->clientStates();
        QStringList onlineBoards;
        QMap<QString, QString> boardMap;
        for (auto it = states.begin(); it != states.end(); ++it) {
            // Sadece çevrimiçi ve Akıllı Tahta (Tip 3) olan istemcileri listele
            if (it.value().connected && it.value().agentType == 3) {
                QString name = it.value().alias.isEmpty() ? it.value().hostname : it.value().alias;
                QString key = QString("%1 [%2]").arg(name, it.value().clientId.left(8));
                onlineBoards.append(key);
                boardMap[key] = it.value().clientId;
            }
        }

        if (onlineBoards.isEmpty()) {
            QMessageBox::information(&dlg, tr("Akıllı Tahta Bulunamadı"),
                tr("Şu anda çevrimiçi veya bağlı bir akıllı tahta (Tip 3) bulunmuyor."));
            return;
        }

        QString targetClientId;
        if (onlineBoards.size() == 1) {
            targetClientId = boardMap.value(onlineBoards.first());
        } else {
            QInputDialog selectDlg(&dlg);
            selectDlg.setWindowTitle(tr("Akıllı Tahta Seç"));
            selectDlg.setLabelText(tr("Kullanıcıları taranacak akıllı tahtayı seçin:"));
            selectDlg.setComboBoxItems(onlineBoards);
            selectDlg.setWindowFlags(selectDlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
            if (selectDlg.exec() != QDialog::Accepted) return;
            QString chosen = selectDlg.textValue();
            if (chosen.isEmpty()) return;
            targetClientId = boardMap.value(chosen);
        }

        if (targetClientId.isEmpty()) return;

        QProgressDialog progress(tr("Akıllı tahtadan yerel kullanıcılar taranıyor..."), tr("İptal"), 0, 0, &dlg);
        progress.setWindowTitle(tr("Akıllı Tahta Kullanıcı Taraması"));
        progress.setWindowFlags(progress.windowFlags() & ~Qt::WindowContextHelpButtonHint);
        progress.setWindowModality(Qt::WindowModal);
        progress.show();

        QEventLoop loop;
        QTimer timeoutTimer;
        timeoutTimer.setSingleShot(true);

        bool received = false;
        bool isSuccess = false;
        QString errStr;
        QList<QPair<QString, QString>> receivedUsers;

        auto conn = connect(m_session, &SessionManager::localUsersReceived,
            [&](const QString& cid, bool success, const QString& errorMsg, const QList<QPair<QString, QString>>& users) {
                if (cid == targetClientId || targetClientId.isEmpty() ||
                    cid.contains(targetClientId) || targetClientId.contains(cid)) {
                    received = true;
                    isSuccess = success;
                    errStr = errorMsg;
                    receivedUsers = users;
                    loop.quit();
                }
            });

        connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
        connect(&progress, &QProgressDialog::canceled, &loop, &QEventLoop::quit);

        m_session->sendQueryLocalUsers(targetClientId);
        timeoutTimer.start(8000);
        loop.exec();

        disconnect(conn);
        progress.close();

        if (!received) {
            QMessageBox::warning(&dlg, tr("Zaman Aşımı"), tr("Akıllı tahtadan yerel kullanıcı yanıtı alınamadı (zaman aşımı)."));
            return;
        }

        if (!isSuccess) {
            QMessageBox::warning(&dlg, tr("Tarama Hatası"), errStr.isEmpty() ? tr("Kullanıcılar okunamadı.") : errStr);
            return;
        }

        if (receivedUsers.isEmpty()) {
            QMessageBox::information(&dlg, tr("Kullanıcı Bulunamadı"),
                tr("Seçilen akıllı tahtada standart bir yerel kullanıcı hesabı bulunamadı."));
            return;
        }

        QStringList userItems;
        for (const auto& u : receivedUsers) {
            userItems << QString("%1 (%2)").arg(u.first, u.second);
        }

        QInputDialog userPickDlg(&dlg);
        userPickDlg.setWindowTitle(tr("Öğretmen Kullanıcısı Seç"));
        userPickDlg.setLabelText(tr("Akıllı tahtadaki yerel kullanıcılardan birini seçin:"));
        userPickDlg.setComboBoxItems(userItems);
        userPickDlg.setWindowFlags(userPickDlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
        if (userPickDlg.exec() != QDialog::Accepted) return;
        QString selectedItem = userPickDlg.textValue();
        if (!selectedItem.isEmpty()) {
            int idx = userItems.indexOf(selectedItem);
            if (idx >= 0 && idx < receivedUsers.size()) {
                userEdit->setText(receivedUsers[idx].first);
                if (!receivedUsers[idx].second.isEmpty() && nameEdit->text().trimmed().isEmpty()) {
                    nameEdit->setText(receivedUsers[idx].second);
                }
            }
        }
    });

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Kaydet"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("İptal"));
    form->addRow(buttons);

    connect(buttons, &QDialogButtonBox::accepted, [&]() {
        if (nameEdit->text().trimmed().isEmpty() || userEdit->text().trimmed().isEmpty()) {
            errorLbl->setText(tr("Ad Soyad ve Kullanıcı Adı boş bırakılamaz."));
            return;
        }
        if (OkidiaDatabase::instance().teacherByUsername(userEdit->text().trimmed().toLower()).id != "") {
            errorLbl->setText(tr("Bu kullanıcı adı zaten başka bir öğretmende kayıtlı."));
            return;
        }
        dlg.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    TeacherRecord t;
    t.id = OkidiaUtils::generateUuid();
    t.username = userEdit->text().trimmed().toLower();
    t.displayName = nameEdit->text().trimmed();
    t.password = passEdit->text().trimmed();
    t.branch = branchEdit->text().trimmed();
    t.isActive = activeCheck->isChecked();
    t.createdAt = QDateTime::currentSecsSinceEpoch();

    QByteArray rawSeed = QCryptographicHash::hash(
        QString(t.id + "_teacher_seed_" + t.username).toUtf8(),
        QCryptographicHash::Sha256);
    t.seedHex = QString::fromUtf8(rawSeed.toHex());

    bool ok = false;
    if (m_session) {
        ok = m_session->sendDbTeacherUpsert(t);
    }

    if (ok) {
        OkidiaDatabase::instance().upsertTeacher(t);
        loadTeachers();

        // Tahtalara eşitleme sor
        auto syncRet = QMessageBox::question(this, tr("Akıllı Tahtalara Eşitle"),
            tr("Yeni öğretmen eklendi. Yetkilerin akıllı tahtalarda anında geçerli olması için şimdi tahtalara eşitlensin mi?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (syncRet == QMessageBox::Yes) {
            onSyncTeachersToSmartboards();
        }
    } else {
        QMessageBox::warning(this, tr("Hata"), tr("Öğretmen ekleme isteği sunucuya iletilemedi."));
    }
}

void UserManagementPanel::onEditTeacher() {
    const QString tid = selectedTeacherId();
    if (tid.isEmpty()) return;

    TeacherRecord t = OkidiaDatabase::instance().teacherById(tid);
    if (t.id.isEmpty()) return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Öğretmeni Düzenle"));
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dlg.setMinimumWidth(440);

    auto* form = new QFormLayout(&dlg);
    form->setContentsMargins(20, 20, 20, 16);
    form->setSpacing(12);

    auto* nameEdit   = new QLineEdit(t.displayName, &dlg);
    auto* branchEdit = new QLineEdit(t.branch, &dlg);
    auto* activeCheck = new QCheckBox(tr("Aktif (Akıllı tahtalara giriş yapabilir)"), &dlg);
    activeCheck->setChecked(t.isActive);

    auto* passEdit = new QLineEdit(&dlg);
    passEdit->setEchoMode(QLineEdit::Password);
    passEdit->setPlaceholderText(tr("Yeni şifre belirle (Değiştirmek istemiyorsanız boş bırakın)"));

    auto* passNote = new QLabel(tr("<small style='color: #8b949e;'>Not: Akıllı tahtada zaten var olan kullanıcıların mevcut şifreleri korunur, ezilmez.</small>"), &dlg);
    passNote->setWordWrap(true);

    auto* userLbl = new QLabel(QString("<b>%1</b> (Kullanıcı adı değiştirilemez)").arg(t.username), &dlg);
    userLbl->setStyleSheet("color: #8b949e;");

    form->addRow(tr("Kullanıcı Adı:"), userLbl);
    form->addRow(tr("Ad Soyad:"),      nameEdit);
    form->addRow(tr("Tahta Şifresi:"), passEdit);
    form->addRow(QString(),            passNote);
    form->addRow(tr("Branş:"),         branchEdit);
    form->addRow(QString(),            activeCheck);

    auto* errorLbl = new QLabel(&dlg);
    errorLbl->setProperty("status", "error");
    form->addRow(errorLbl);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Güncelle"));
    buttons->button(QDialogButtonBox::Cancel)->setText(tr("İptal"));
    form->addRow(buttons);

    connect(buttons, &QDialogButtonBox::accepted, [&]() {
        if (nameEdit->text().trimmed().isEmpty()) {
            errorLbl->setText(tr("Ad Soyad boş bırakılamaz."));
            return;
        }
        dlg.accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    t.displayName = nameEdit->text().trimmed();
    t.branch = branchEdit->text().trimmed();
    t.isActive = activeCheck->isChecked();
    if (!passEdit->text().trimmed().isEmpty()) {
        t.password = passEdit->text().trimmed();
    }

    bool ok = false;
    if (m_session) {
        ok = m_session->sendDbTeacherUpsert(t);
    }

    if (ok) {
        OkidiaDatabase::instance().upsertTeacher(t);
        loadTeachers();

        auto syncRet = QMessageBox::question(this, tr("Akıllı Tahtalara Eşitle"),
            tr("Öğretmen bilgileri güncellendi. Değişikliklerin tahtalara yansıması için şimdi eşitlensin mi?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (syncRet == QMessageBox::Yes) {
            onSyncTeachersToSmartboards();
        }
    } else {
        QMessageBox::warning(this, tr("Hata"), tr("Öğretmen güncelleme isteği sunucuya iletilemedi."));
    }
}

void UserManagementPanel::onDeleteTeacher() {
    const QString tid = selectedTeacherId();
    if (tid.isEmpty()) return;

    TeacherRecord t = OkidiaDatabase::instance().teacherById(tid);
    if (t.id.isEmpty()) return;

    auto ret = QMessageBox::question(this, tr("Öğretmen Sil"),
        tr("\"%1\" (%2) isimli öğretmeni silmek istediğinize emin misiniz?\n\n"
           "Bu işlem öğretmenin akıllı tahtalara mobil veya şifre ile giriş yetkisini iptal edecektir.")
            .arg(t.displayName.isEmpty() ? t.username : t.displayName, t.username),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes) return;

    bool ok = false;
    if (m_session) {
        ok = m_session->sendDbTeacherDelete(tid);
    }

    if (ok) {
        OkidiaDatabase::instance().removeTeacher(tid);
        loadTeachers();

        auto syncRet = QMessageBox::question(this, tr("Akıllı Tahtalara Eşitle"),
            tr("Öğretmen silindi. Silinen öğretmenin tahtalardan da kaldırılması için şimdi eşitlensin mi?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (syncRet == QMessageBox::Yes) {
            onSyncTeachersToSmartboards();
        }
    } else {
        QMessageBox::warning(this, tr("Hata"), tr("Öğretmen silme isteği sunucuya iletilemedi."));
    }
}

void UserManagementPanel::onShowTeacherMobilePairQr() {
    const QString tid = selectedTeacherId();
    if (tid.isEmpty()) return;

    TeacherRecord t = OkidiaDatabase::instance().teacherById(tid);
    if (t.id.isEmpty()) return;

    if (t.seedHex.isEmpty()) {
        QByteArray rawSeed = QCryptographicHash::hash(
            QString(t.id + "_teacher_seed_" + t.username).toUtf8(),
            QCryptographicHash::Sha256);
        t.seedHex = QString::fromUtf8(rawSeed.toHex());
    }

    // Format: OKIDIA_PAIR:<TEACHER_ID>:<SEED_HEX>:<DISPLAY_NAME>
    QString pairQrData = QStringLiteral("OKIDIA_PAIR:%1:%2:%3")
                             .arg(t.id)
                             .arg(t.seedHex)
                             .arg(t.displayName.isEmpty() ? t.username : t.displayName);

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Okidia Mobil Eşleştirme QR Kodu"));
    dlg.setWindowFlags(dlg.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dlg.setFixedWidth(440);

    auto* mainLay = new QVBoxLayout(&dlg);
    mainLay->setContentsMargins(24, 24, 24, 24);
    mainLay->setSpacing(16);

    auto* titleLbl = new QLabel(tr("📱 Okidia Mobil Eşleştirme"), &dlg);
    titleLbl->setStyleSheet("font-size: 18px; font-weight: bold; color: #58a6ff;");
    titleLbl->setAlignment(Qt::AlignCenter);
    mainLay->addWidget(titleLbl);

    auto* userLbl = new QLabel(tr("<b>Öğretmen:</b> %1 (%2) — <i>%3</i>")
                               .arg(t.displayName.isEmpty() ? t.username : t.displayName,
                                    t.username,
                                    t.branch.isEmpty() ? tr("Genel") : t.branch), &dlg);
    userLbl->setAlignment(Qt::AlignCenter);
    mainLay->addWidget(userLbl);

    auto* infoLbl = new QLabel(tr("Öğretmen cep telefonundaki Okidia Mobil uygulamasını açarak "
                                  "bu QR kodu bir kez taramalıdır. Eşleştirme tamamlandıktan sonra "
                                  "akıllı tahtaları internetsiz açabilecektir."), &dlg);
    infoLbl->setStyleSheet("font-size: 13px; color: #8b949e;");
    infoLbl->setAlignment(Qt::AlignCenter);
    infoLbl->setWordWrap(true);
    mainLay->addWidget(infoLbl);

    auto* qrContainer = new QFrame(&dlg);
    qrContainer->setStyleSheet("background-color: #ffffff; border-radius: 12px; padding: 10px;");
    auto* qrLayout = new QVBoxLayout(qrContainer);
    qrLayout->setAlignment(Qt::AlignCenter);

    auto* qrLabel = new QLabel(qrContainer);
    qrLabel->setFixedSize(260, 260);
    qrLabel->setAlignment(Qt::AlignCenter);
    QPixmap qrPix = QrCodeGenerator::generateQrPixmap(pairQrData, 250);
    qrLabel->setPixmap(qrPix);
    qrLayout->addWidget(qrLabel);

    mainLay->addWidget(qrContainer, 0, Qt::AlignCenter);

    auto* closeBtn = new QPushButton(tr("Kapat"), &dlg);
    closeBtn->setStyleSheet("min-width: 100px; padding: 6px; font-weight: bold;");
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    mainLay->addWidget(closeBtn, 0, Qt::AlignCenter);

    dlg.exec();
}

void UserManagementPanel::onSyncTeachersToSmartboards() {
    if (!m_session || !m_session->isConnected()) {
        QMessageBox::warning(this, tr("Bağlantı Hatası"),
            tr("Sunucu online değil. Tahtalara eşitleme yapabilmek için sunucu bağlantısı gereklidir."));
        return;
    }

    int smartboardCount = 0;
    for (const auto& st : m_session->allClientStates()) {
        if (st.connected && st.agentType == 3) {
            smartboardCount++;
        }
    }

    bool ok = m_session->sendTeacherKeySyncToAll();
    if (ok) {
        QString msg;
        if (smartboardCount > 0) {
            msg = tr("Öğretmen listesi ve kilit açma yetkileri ağdaki %1 adet Akıllı Tahta istemcisine başarıyla gönderildi.")
                      .arg(smartboardCount);
        } else {
            msg = tr("Öğretmen listesi ve kilit açma yetkileri sunucuya iletildi. (Çevrimiçi Akıllı Tahtalara aktarılacaktır)");
        }
        QMessageBox::information(this, tr("Eşitleme Başlatıldı"), msg);
    } else {
        QMessageBox::warning(this, tr("Hata"),
            tr("Öğretmen eşitleme isteği sunucuya iletilemedi."));
    }
}

void UserManagementPanel::onTeacherContextMenu(const QPoint& pos) {
    QModelIndex idx = m_teacherTable->indexAt(pos);
    if (!idx.isValid()) return;

    m_teacherTable->selectRow(idx.row());
    onTeacherSelectionChanged();

    const QString tid = selectedTeacherId();
    if (tid.isEmpty()) return;

    const bool isYM = (m_activeUser.role == "YM");
    const bool isConnected = m_session && m_session->isConnected();

    QMenu menu(this);

    auto* actQr = menu.addAction(ThemeManager::icon(":/ui/share.svg"), tr("Mobil Eşleştirme QR Kodunu Göster"));
    connect(actQr, &QAction::triggered, this, &UserManagementPanel::onShowTeacherMobilePairQr);

    if (isYM) {
        auto* actEdit = menu.addAction(ThemeManager::icon(":/ui/edit.svg"), tr("Öğretmeni Düzenle"));
        connect(actEdit, &QAction::triggered, this, &UserManagementPanel::onEditTeacher);

        auto* actSync = menu.addAction(ThemeManager::icon(":/ui/refresh.svg"), tr("Tüm Tahtalara Eşitle"));
        actSync->setEnabled(isConnected);
        connect(actSync, &QAction::triggered, this, &UserManagementPanel::onSyncTeachersToSmartboards);

        menu.addSeparator();

        auto* actDel = menu.addAction(ThemeManager::icon(":/ui/trash.svg"), tr("Öğretmeni Sil"));
        connect(actDel, &QAction::triggered, this, &UserManagementPanel::onDeleteTeacher);
    }

    menu.exec(m_teacherTable->viewport()->mapToGlobal(pos));
}
