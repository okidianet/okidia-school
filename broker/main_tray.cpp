// broker/main_tray.cpp
// OKIDIA Broker Tray — masaüstü oturumunda çalışır (QApplication).
// Broker servisine /run/okidia/broker.sock üzerinden bağlanır.
// Servis yoksa yeniden denemeye devam eder.

#include <QApplication>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QTimer>
#include <QPixmap>
#include <QPainter>
#include <QPolygon>
#include <QFile>
#include <QString>
#include <QMessageBox>
#include <QPushButton>
#include <QAbstractButton>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QFileDialog>
#include <QInputDialog>
#include <QStandardPaths>
#include <QDateTime>
#include "../common/utils/Logger.h"
#include "../common/utils/OkidiaUtils.h"
#include "../common/utils/PlatformService.h"
#include "../common/network/LocalConnection.h"
#include "../common/OkidiaConfig.h"
#include <QClipboard>
#include <QTextStream>
#include <QDialog>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QLabel>
#include "../common/ui/ThemeManager.h"
#if defined(Q_OS_WIN)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif
#include "../common/crypto/BackupPackage.h"
#include "messages.pb.h"

// ── İkon yardımcısı ───────────────────────────────────────────────────────────
static QIcon makeBrokerIcon(const QColor& color)
{
    QPixmap px(22, 22);
    px.fill(Qt::transparent);
    QPainter p(&px);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(color);
    p.setPen(Qt::NoPen);
    QPolygon diamond;
    diamond << QPoint(11, 2) << QPoint(20, 11)
            << QPoint(11, 20) << QPoint(2, 11);
    p.drawPolygon(diamond);
    p.end();
    return QIcon(px);
}

static QIcon loadBrokerIcon(const QColor& fallback)
{
    // 1. Qt Gömülü Resource kontrolü (Tüm boyutlar: 16px - 256px)
    QIcon resIcon;
    resIcon.addFile(":/icons/broker/okidia-broker-16.png");
    resIcon.addFile(":/icons/broker/okidia-broker-22.png");
    resIcon.addFile(":/icons/broker/okidia-broker-24.png");
    resIcon.addFile(":/icons/broker/okidia-broker-32.png");
    resIcon.addFile(":/icons/broker/okidia-broker-48.png");
    resIcon.addFile(":/icons/broker/okidia-broker-64.png");
    resIcon.addFile(":/icons/broker/okidia-broker-128.png");
    resIcon.addFile(":/icons/broker/okidia-broker-256.png");

    if (!resIcon.isNull() && !resIcon.availableSizes().isEmpty()) {
        return resIcon;
    }

    // 2. Uygulama ve sistem dizini kontrolü (Windows / Linux)
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidatePaths = {
        appDir + "/../resources/icons/broker/okidia-broker.ico",
        appDir + "/resources/icons/broker/okidia-broker.ico",
        appDir + "/../resources/icons/broker/okidia-broker-22.png",
        appDir + "/resources/icons/broker/okidia-broker-22.png",
        "/usr/share/icons/hicolor/22x22/apps/okidia-broker.png",
        "/usr/share/icons/hicolor/scalable/apps/okidia-broker.svg"
    };
    for (const QString& path : candidatePaths) {
        if (QFile::exists(path)) {
            QIcon ico(path);
            if (!ico.isNull()) return ico;
        }
    }

    // 3. Sistem teması
    if (QIcon::hasThemeIcon("okidia-broker")) {
        return QIcon::fromTheme("okidia-broker");
    }

    return makeBrokerIcon(fallback);
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setOrganizationName("okidia");
    app.setApplicationName("broker-tray");
    app.setApplicationVersion(Okidia::Version::string());
    app.setQuitOnLastWindowClosed(false);

    OkidiaUtils::installTranslator(app);
    Logger::init(OkidiaConfig::logDir() + "/okidia-broker-tray.log");

    // ── Tek örnek koruması ────────────────────────────────────────────────────
#if defined(Q_OS_WIN)
    HANDLE hBrokerMutex = CreateMutexW(NULL, TRUE, L"Local\\OkidiaBrokerTray_SingleInstance_Mutex");
    if (hBrokerMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hBrokerMutex);
        return 0;
    }
#endif
    QString uName = qgetenv("USER");
    if (uName.isEmpty()) uName = qgetenv("USERNAME");
    if (uName.isEmpty()) uName = qgetenv("LOGNAME");
    const QString lockName = "okidia-broker-tray-" + uName;
    {
        // Önce mevcut instance var mı diye bak
        QLocalSocket probe;
        probe.connectToServer(lockName);
        if (probe.waitForConnected(300)) {
            // Zaten çalışıyor — sessizce çık
            probe.disconnectFromServer();
            Logger::info(QObject::tr("BrokerTray: Zaten çalışıyor, ikinci instance kapatılıyor."));
            return 0;
        }
    }
    // Kilit sunucusunu bu instance sahipleniyor
    auto* lockServer = new QLocalServer(&app);
    QLocalServer::removeServer(lockName);  // artık process varsa kalan soket temizle
    lockServer->listen(lockName);
    // Gelen bağlantıları sadece kapat (probe için)
    QObject::connect(lockServer, &QLocalServer::newConnection, [lockServer]() {
        if (auto* s = lockServer->nextPendingConnection()) s->deleteLater();
    });

    // ── İkonlar ───────────────────────────────────────────────────────────────
    const QIcon iconReady       = loadBrokerIcon(QColor("#2ecc71"));
    const QIcon iconConnecting  = loadBrokerIcon(QColor("#e67e22"));
    const QIcon iconError       = loadBrokerIcon(QColor("#e74c3c"));

    // ── Tray kurulumu ─────────────────────────────────────────────────────────
    auto* tray = new QSystemTrayIcon(&app);
    tray->setIcon(iconConnecting);
    tray->setToolTip(QObject::tr("OKIDIA Broker — Servise bağlanıyor..."));

    auto* menu = new QMenu();

    auto* actStatus  = menu->addAction(QObject::tr("OKIDIA Broker — Bağlanıyor..."));
    actStatus->setEnabled(false);

    auto* sep1 = menu->addSeparator();

    auto* actMasterTitle = menu->addAction(QObject::tr("Master: Bekleniyor"));
    actMasterTitle->setEnabled(false);

    QList<QAction*> masterActions;
    QStringList lastMasterNames;
    QString currentBrokerName = OkidiaConfig::instance().brokerName();
    int currentBrokerPort     = OkidiaConfig::instance().brokerPort();

    auto* sep2 = menu->addSeparator();

    auto* actClients = menu->addAction(QObject::tr("İstemci: —"));
    actClients->setEnabled(false);

    auto* actPending = menu->addAction(QObject::tr("Onay bekleyen: —"));
    actPending->setEnabled(false);

    auto* actCode     = menu->addAction(QObject::tr("Kurulum kodu: —"));
    actCode->setEnabled(false);
    actCode->setVisible(false);

    auto* actCopyCode = menu->addAction(QObject::tr("Kurulum Kodunu Kopyala"));
    actCopyCode->setVisible(false);

    auto* actShowCode = menu->addAction(QObject::tr("Kurulum Kodunu Göster…"));
    actShowCode->setVisible(false);

    auto* sep3        = menu->addSeparator();
    auto* actSettings = menu->addAction(QObject::tr("Sunucu Ayarları…"));
    auto* actStop     = menu->addAction(QObject::tr("Sunucuyu Durdur"));
    auto* actRestore  = menu->addAction(QObject::tr("Yedekten Geri Yükle…"));
    menu->addSeparator();
    menu->addAction(QObject::tr("Hakkında OKIDIA"), []() {
        const QString ver = qApp->applicationVersion().isEmpty() ? Okidia::Version::string() : qApp->applicationVersion();
        QMessageBox msgBox;
        msgBox.setWindowTitle(QObject::tr("Hakkında OKIDIA"));
        msgBox.setIconPixmap(QIcon(":/icons/broker/okidia-broker-32.png").pixmap(48, 48));
        msgBox.setTextFormat(Qt::RichText);
        msgBox.setText(
            QString("<h3>OKIDIA Broker <span style='font-size: 13px; font-weight: normal;'>v%1</span></h3>"
                    "<p><b>%2</b></p>"
                    "<p>%3</p>"
                    "<hr/>"
                    "<p><b>%4:</b> <a href='https://www.okidia.net'>https://www.okidia.net</a><br/>"
                    "<b>%5:</b> <a href='https://lisans.okidia.net'>https://lisans.okidia.net</a><br/>"
                    "<b>%6:</b> <a href='mailto:info@okidia.net'>info@okidia.net</a></p>"
                    "<hr/>"
                    "<p style='font-size: 11px; color: #7f8c8d;'>"
                    "<b>%7:</b> Qt 6 (LGPLv3), OpenSSL, Google Protocol Buffers, SQLite, Avahi.<br/>"
                    "%8"
                    "</p>")
            .arg(ver)
            .arg(QObject::tr("Okul İçi Denetim ve İletişim Ağı — Merkez Sunucu ve Yönlendirici"))
            .arg(QObject::tr("Uçtan uca mTLS şifrelemeli mesajlaşma, sıfır konfigürasyon (Zero-Config) keşif, dinamik oda yönetimi ve OTA dağıtım sunucusu."))
            .arg(QObject::tr("Web Sitesi"))
            .arg(QObject::tr("Lisans Sunucusu"))
            .arg(QObject::tr("İletişim"))
            .arg(QObject::tr("Açık Kaynak Bileşenler"))
            .arg(QObject::tr("Telif Hakkı © 2024-2026 OKIDIA. Tüm hakları saklıdır."))
        );
        QAbstractButton* btnLicense = msgBox.addButton(QObject::tr("Lisans Bilgisi"), QMessageBox::ActionRole);
        msgBox.addButton(QMessageBox::Ok);
        msgBox.exec();
        if (msgBox.clickedButton() == btnLicense) {
            OkidiaUtils::showLicenseDialog();
        }
    });

    tray->setContextMenu(menu);

    // sol tık → menü göster (popup() kullanmıyoruz — event loop bloğu yapar)
    // Qt sağ tıkta contextMenu'yu zaten native gösteriyor.
    tray->show();

    // ── Servis bağlantısı ─────────────────────────────────────────────────────
    const QString socketPath = OkidiaConfig::runDir() + "/broker.sock";
    auto* conn = new LocalConnection(socketPath, &app);

    // Bağlanamayınca yeniden dene
    auto* reconnectTimer = new QTimer(&app);
    reconnectTimer->setInterval(3000);
    QObject::connect(reconnectTimer, &QTimer::timeout, [conn]() {
        if (!conn->isConnected())
            conn->connectToServer();
    });

    QObject::connect(conn, &LocalConnection::connected, [&]() {
        Logger::info(QObject::tr("BrokerTray: Servise bağlandı"));
        reconnectTimer->stop();
        tray->setIcon(iconConnecting);
        tray->setToolTip(QObject::tr("OKIDIA Broker — Bağlı, durum bekleniyor..."));
        // Durum iste
        okidia::BrokerCommand cmd;
        cmd.set_type(okidia::BrokerCommand::STATUS);
        okidia::Envelope env;
        env.set_type(okidia::MessageType::BROKER_CMD);
        env.set_payload(cmd.SerializeAsString());
        conn->sendEnvelope(env);
    });

    QObject::connect(conn, &LocalConnection::disconnected, [tray, actStatus, actMasterTitle, actClients, actPending, actCode, actCopyCode, actShowCode, actStop, menu, &masterActions, &lastMasterNames, iconConnecting, reconnectTimer]() {
        Logger::info(QObject::tr("BrokerTray: Servis bağlantısı kesildi"));
        tray->setIcon(iconConnecting);
        tray->setToolTip(QObject::tr("OKIDIA Broker — Servise bağlanıyor..."));
        actStatus->setText(QObject::tr("OKIDIA Broker — Servis bulunamadı"));
        actMasterTitle->setText(QObject::tr("Master: —"));
        for (auto* act : masterActions) {
            menu->removeAction(act);
            act->deleteLater();
        }
        masterActions.clear();
        lastMasterNames.clear();
        actClients->setText(QObject::tr("İstemci: —"));
        actPending->setText(QObject::tr("Onay bekleyen: —"));
        actCode->setVisible(false);
        actCopyCode->setVisible(false);
        actShowCode->setVisible(false);
        actStop->setText(QObject::tr("Sunucuyu Başlat"));
        reconnectTimer->start();
    });

    QObject::connect(conn, &LocalConnection::envelopeReceived,
                     [tray, actStatus, actClients, actPending, actCode, actCopyCode, actShowCode,
                      iconReady, iconError, menu, actStop, actMasterTitle, sep2, &masterActions, &lastMasterNames,
                      &currentBrokerName, &currentBrokerPort](const okidia::Envelope& env) {
        if (env.type() != okidia::MessageType::BROKER_STATUS) return;
        okidia::BrokerStatus st;
        if (!st.ParseFromString(env.payload())) return;

        // Dinamik master eylemlerini sadece list değişirse güncelle
        QStringList currentMasterNames;
        for (int i = 0; i < st.master_names_size(); ++i) {
            currentMasterNames.append(QString::fromStdString(st.master_names(i)));
        }

        if (currentMasterNames != lastMasterNames) {
            for (auto* act : masterActions) {
                menu->removeAction(act);
                act->deleteLater();
            }
            masterActions.clear();

            if (st.master_conn()) {
                actMasterTitle->setText(QString(QObject::tr("Master: %1 Bağlı")).arg(st.master_names_size()));
                for (const QString& name : currentMasterNames) {
                    auto* act = new QAction("  •  " + name, menu);
                    act->setEnabled(false);
                    menu->insertAction(sep2, act);
                    masterActions.append(act);
                }
            } else {
                actMasterTitle->setText(QObject::tr("Master: Bekleniyor"));
            }
            lastMasterNames = currentMasterNames;
        }

        // İkon ve durum metni
        if (st.ready()) {
            tray->setIcon(iconReady);
            actStatus->setText(QObject::tr("OKIDIA Broker — Hazır"));
            actStop->setText(QObject::tr("Sunucuyu Durdur"));

            QString masterStr = st.master_conn()
                ? QString(QObject::tr("%1 Master Bağlı")).arg(st.master_names_size())
                : QObject::tr("Master: Bekleniyor");
            tray->setToolTip(QString(QObject::tr("OKIDIA Broker — Hazır\n%1\nİstemci: %2"))
                .arg(masterStr).arg(st.client_count()));
        } else if (!st.error_msg().empty()) {
            tray->setIcon(iconError);
            actStatus->setText(QString(QObject::tr("OKIDIA Broker — Hata: %1"))
                .arg(QString::fromStdString(st.error_msg())));
        }

        if (!st.broker_name().empty()) {
            currentBrokerName = QString::fromStdString(st.broker_name());
        }
        if (st.broker_port() > 0) {
            currentBrokerPort = st.broker_port();
        }

        actClients->setText(QString(QObject::tr("İstemci: %1")).arg(st.client_count()));

        const int pendingCount = st.pending_ips_size();
        actPending->setText(pendingCount > 0
            ? QString(QObject::tr("Onay bekleyen: %1")).arg(pendingCount)
            : QObject::tr("Onay bekleyen: yok"));

        // Kurulum kodu — sadece kullanılmamışsa ve masaüstünde dosya mevcutsa göster
        const QString code = QString::fromStdString(st.setup_code());

        const QString desktopPath = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
        bool desktopFileExists = false;
        if (!desktopPath.isEmpty()) {
            desktopFileExists = QFile::exists(desktopPath + "/OKIDIA_Kurulum_Kodu.txt") ||
                                QFile::exists(desktopPath + "/OKIDIA_Setup_Code.txt");
        }

        const bool showSetupActions = (!code.isEmpty() && desktopFileExists);
        actCode->setVisible(showSetupActions);
        actCopyCode->setVisible(showSetupActions);
        actShowCode->setVisible(showSetupActions);

        if (showSetupActions) {
            actCode->setText(QString(QObject::tr("Kurulum kodu: %1")).arg(code));

            // Tray tooltip'ine de yaz
            tray->setToolTip(QString(QObject::tr("OKIDIA Broker — Hazır\n"
                                                 "Kurulum kodu: %1\n"
                                                 "Master kurulumunda bu kodu girin.")).arg(code));

            // Balon bildirimi — 30sn görünür, yalnızca bir kez göster
            static bool codeNotified = false;
            if (!codeNotified) {
                codeNotified = true;
                tray->showMessage(
                    QObject::tr("OKIDIA — İlk Kurulum Kodu"),
                    QString(QObject::tr("Kod: %1\n\nMaster kurulumunda bu kodu girin.\nTek kullanımlıktır!")).arg(code),
                    QSystemTrayIcon::Information,
                    30000  // 30 saniye
                );
            }
        }
    });

    // ── Sunucu Ayarları Dialogu ───────────────────────────────────────────────
    QObject::connect(actSettings, &QAction::triggered, [conn, tray, &currentBrokerName, &currentBrokerPort]() {
        QDialog dlg;
        dlg.setWindowTitle(QObject::tr("Sunucu Ayarları — OKIDIA Broker"));
        dlg.setWindowIcon(QIcon(":/icons/broker/okidia-broker-32.png"));
        dlg.setMinimumWidth(380);

        auto* layout = new QVBoxLayout(&dlg);
        layout->setSpacing(16);

        auto* headerLabel = new QLabel(QObject::tr("<b>Merkez Sunucu Temel Yapılandırması</b>"), &dlg);
        headerLabel->setStyleSheet("font-size: 14px;");
        layout->addWidget(headerLabel);

        auto* form = new QFormLayout();
        form->setSpacing(12);

        auto* editName = new QLineEdit(&dlg);
        editName->setText(currentBrokerName);
        editName->setPlaceholderText(QObject::tr("Örn: OKIDIA-Broker, Lab-1-Broker"));
        form->addRow(QObject::tr("Sunucu Adı (mDNS):"), editName);

        auto* spinPort = new QSpinBox(&dlg);
        spinPort->setRange(1024, 65535);
        spinPort->setValue(currentBrokerPort > 0 ? currentBrokerPort : 11200);
        form->addRow(QObject::tr("İletişim Portu:"), spinPort);

        layout->addLayout(form);

        auto* infoLabel = new QLabel(QObject::tr("<small style='color: gray;'>Ayar değiştirildiğinde sunucu servisi yeni port ve isimle mDNS anonsunu ve dinlemeyi anında güncelleyecektir.</small>"), &dlg);
        infoLabel->setWordWrap(true);
        layout->addWidget(infoLabel);

        auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        layout->addWidget(btnBox);

        QObject::connect(btnBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        QObject::connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

        if (dlg.exec() == QDialog::Accepted) {
            QString newName = editName->text().trimmed();
            if (newName.isEmpty()) newName = "OKIDIA-Broker";
            int newPort = spinPort->value();

            currentBrokerName = newName;
            currentBrokerPort = newPort;

            // 1. Yerel config dosyasına yaz
            OkidiaConfig::saveBrokerConfig(newName, newPort);

            // 2. Servis bağlıysa IPC üzerinden ilet
            if (conn->isConnected()) {
                okidia::BrokerCommand cmd;
                cmd.set_type(okidia::BrokerCommand::SET_CONFIG);
                cmd.set_broker_name(newName.toStdString());
                cmd.set_broker_port(newPort);

                okidia::Envelope env;
                env.set_type(okidia::MessageType::BROKER_CMD);
                env.set_payload(cmd.SerializeAsString());
                conn->sendEnvelope(env);

                tray->showMessage(
                    QObject::tr("Sunucu Ayarları Güncellendi"),
                    QObject::tr("Yeni sunucu adı: %1\nYeni port: %2").arg(newName).arg(newPort),
                    QSystemTrayIcon::Information,
                    4000
                );
            }
        }
    });

    // ── Kurulum Kodunu Kopyala ve Göster Eylemleri ────────────────────────────
    QObject::connect(actCopyCode, &QAction::triggered, [actCode, tray]() {
        QString text = actCode->text();
        QString code = text.section(':', 1).trimmed();
        if (!code.isEmpty()) {
            QGuiApplication::clipboard()->setText(code);
            tray->showMessage(
                QObject::tr("OKIDIA Broker"),
                QObject::tr("Kurulum kodu panoya kopyalandı: %1").arg(code),
                QSystemTrayIcon::Information,
                3000
            );
        }
    });

    QObject::connect(actShowCode, &QAction::triggered, [actCode]() {
        QString text = actCode->text();
        QString code = text.section(':', 1).trimmed();
        if (!code.isEmpty()) {
            QMessageBox msgBox;
            msgBox.setWindowTitle(QObject::tr("OKIDIA — İlk Kurulum Kodu"));
            msgBox.setIconPixmap(loadBrokerIcon(QColor("#2ecc71")).pixmap(48, 48));
            msgBox.setText(QObject::tr("İlk Yönetici (Master) Kurulum Kodunuz:"));
            msgBox.setInformativeText(QString("<h2 style='color:#16a085; text-align:center;'>%1</h2>"
                                             "<p>%2</p>"
                                             "<p><small>%3</small></p>")
                                      .arg(code)
                                      .arg(QObject::tr("Bu kodu Master ilk kurulum sihirbazında (SetupWizard) girerek yönetici hesabınızı oluşturabilirsiniz."))
                                      .arg(QObject::tr("Not: Bu kod tek kullanımlıktır. Kod ayrıca Masaüstünüze 'OKIDIA_Kurulum_Kodu.txt' olarak da kaydedilmiştir.")));
            QPushButton* btnCopy = msgBox.addButton(QObject::tr("Kodu Kopyala"), QMessageBox::ActionRole);
            msgBox.addButton(QMessageBox::Ok);
            msgBox.exec();
            if (msgBox.clickedButton() == btnCopy) {
                QGuiApplication::clipboard()->setText(code);
            }
        }
    });

    // ── Sunucuyu Durdur / Başlat ──────────────────────────────────────────────
    QObject::connect(actStop, &QAction::triggered, [conn]() {
        if (conn->isConnected()) {
            okidia::BrokerCommand cmd;
            cmd.set_type(okidia::BrokerCommand::STOP);
            okidia::Envelope env;
            env.set_type(okidia::MessageType::BROKER_CMD);
            env.set_payload(cmd.SerializeAsString());
            conn->sendEnvelope(env);
            Logger::info(QObject::tr("BrokerTray: Durdur komutu gönderildi (IPC)"));
        } else {
#if defined(Q_OS_WIN)
            PlatformService::executeElevated("sc.exe", {"start", "OkidiaBroker"});
#else
            PlatformService::executeElevated("systemctl", {"start", "okidia-broker.service"});
#endif
            Logger::info(QObject::tr("BrokerTray: Servisi başlat komutu gönderildi"));
        }
    });

    // ── Yedekten Geri Yükle ───────────────────────────────────────────────────
    QObject::connect(actRestore, &QAction::triggered, [conn]() {
        QString defaultDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/OKIDIA-Yedekler";
        QString path = OkidiaUtils::getOpenFileName(
            nullptr, QObject::tr("OKIDIA Sistem Yedeği Seç"), "broker_backup",
            QObject::tr("OKIDIA Yedek (*.okbak);;Tüm Dosyalar (*)"),
            nullptr, QFileDialog::Options(), defaultDir);
        if (path.isEmpty()) return;

        if (!BackupPackage::verifyHeaderFile(path)) {
            QMessageBox::critical(nullptr, QObject::tr("Geçersiz Dosya"), QObject::tr("Seçilen dosya geçerli bir OKIDIA (.okbak) yedek paketi değil."));
            return;
        }

        bool ok = false;
        QString pwd = QInputDialog::getText(
            nullptr, QObject::tr("Yedek Parolası"),
            QObject::tr("Yedekleme dosyasının şifresini girin:"),
            QLineEdit::Password, QString(), &ok);
        if (!ok || pwd.isEmpty()) return;

        BackupContent content;
        QString err;
        if (!BackupPackage::unpackFromFile(path, pwd, content, &err)) {
            QMessageBox::critical(nullptr, QObject::tr("Şifre Çözme Hatası"), err);
            return;
        }

        int ret = QMessageBox::warning(nullptr, QObject::tr("Sistem Yedeğini Geri Yükle"),
            QObject::tr("Bu işlem sunucudaki mevcut veritabanını ve güvenlik sertifikalarını bu yedekle değiştirecektir.\n\n"
                       "Yedek Tarihi: %1\n"
                       "Oluşturan: %2\n"
                       "Sertifika Sayısı: %3\n\n"
                       "Devam etmek istiyor musunuz?").arg(
                           content.meta.timestamp > 0 ? QDateTime::fromSecsSinceEpoch(content.meta.timestamp).toString("dd.MM.yyyy HH:mm") : QObject::tr("Bilinmiyor"),
                           content.meta.creator.isEmpty() ? QObject::tr("Belirtilmemiş") : content.meta.creator,
                           QString::number(content.certFiles.size())),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

        if (ret != QMessageBox::Yes) return;

        // Veritabanını geri yükle
        const QString dbDir = OkidiaConfig::dbDir();
        QDir().mkpath(dbDir);
        QString dbPath = dbDir + "/okidia.db";
        QFile::remove(dbPath + "-wal");
        QFile::remove(dbPath + "-shm");

        QFile dbFile(dbPath);
        if (dbFile.open(QIODevice::WriteOnly)) {
            dbFile.write(content.databaseData);
            dbFile.close();
            Logger::info(QObject::tr("BrokerTray: okidia.db geri yüklendi"));
        }

        if (!content.auditData.isEmpty()) {
            QString auditPath = dbDir + "/okidia_audit.db";
            QFile::remove(auditPath + "-wal");
            QFile::remove(auditPath + "-shm");
            QFile af(auditPath);
            if (af.open(QIODevice::WriteOnly)) {
                af.write(content.auditData);
                af.close();
                Logger::info(QObject::tr("BrokerTray: okidia_audit.db geri yüklendi"));
            }
        }

        if (!content.chatData.isEmpty()) {
            QString chatPath = dbDir + "/okidia_chat.db";
            QFile::remove(chatPath + "-wal");
            QFile::remove(chatPath + "-shm");
            QFile cf(chatPath);
            if (cf.open(QIODevice::WriteOnly)) {
                cf.write(content.chatData);
                cf.close();
                Logger::info(QObject::tr("BrokerTray: okidia_chat.db geri yüklendi"));
            }
        }

        // Sertifikaları geri yükle
        const QString certDir = OkidiaConfig::brokerCertDir();
        QDir().mkpath(certDir);
        for (auto it = content.certFiles.begin(); it != content.certFiles.end(); ++it) {
            QString outCertPath = certDir + "/" + it.key();
            QFile cf(outCertPath);
            if (cf.open(QIODevice::WriteOnly)) {
                cf.write(it.value());
                cf.close();
                Logger::info(QObject::tr("BrokerTray: Sertifika geri yüklendi → ") + it.key());
            }
        }

        // Servis bağlıysa durdur / yeniden başlat
        if (conn->isConnected()) {
            okidia::BrokerCommand cmd;
            cmd.set_type(okidia::BrokerCommand::STOP);
            okidia::Envelope env;
            env.set_type(okidia::MessageType::BROKER_CMD);
            env.set_payload(cmd.SerializeAsString());
            conn->sendEnvelope(env);
            Logger::info(QObject::tr("BrokerTray: Geri yükleme sonrası Broker STOP komutu gönderildi"));
        }

        QMessageBox::information(nullptr, QObject::tr("Geri Yükleme Tamamlandı"),
            QObject::tr("Sistem veritabanı ve Kök CA sertifikaları başarıyla geri yüklendi.\nSunucu servisi yeni verilerle başlatılacaktır."));
    });

    // ── Başlat ────────────────────────────────────────────────────────────────
    conn->connectToServer();
    reconnectTimer->start();

    return app.exec();
}
