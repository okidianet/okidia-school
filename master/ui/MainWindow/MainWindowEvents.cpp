// master/ui/MainWindow/MainWindowEvents.cpp
// Sistem bildirim pencereleri, donanım/saat uyarıları, OTA diyalogları ve pencere olayları.

#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "../Widgets/MasterUpdateDialog.h"
#include "../../session/SessionManager.h"
#include "../../ssh/OtaUpdatePanel.h"
#include "../../ssh/SshManagerPanel.h"
#include "../../common/utils/OkidiaUtils.h"
#include "../../../common/ui/ThemeManager.h"
#include <utils/Logger.h>

#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QDockWidget>
#include <QTabWidget>
#include <QApplication>

void MainWindow::onOpenSshTerminalForClient(const QString& clientId) {
    if (!m_sshManagerPanel) return;

    showSystemManagementWindow(m_sshManagerPanel);
    m_sshManagerPanel->openTerminalForClient(clientId);
}

void MainWindow::onMasterUpdateAvailable(const QString& version, const QString& filename, quint64 size, const QString& osTag, const QString& osArch) {
    Q_UNUSED(filename);
    Q_UNUSED(size);
    if (!m_masterUpdateBtn) return;

    // İşletim sistemi ve mimari uyumluluğu kontrolü
    if (!osTag.isEmpty()) {
        const QString myOsTag = OkidiaUtils::detectLocalOsTag();
        if (!myOsTag.isEmpty() && !OkidiaUtils::isOsPackageCompatible(osTag, myOsTag)) {
            Logger::info(QObject::tr("MainWindow: Uyumsuz bir OS için Master güncellemesi bildirildi (%1 != %2), yoksayılıyor.").arg(osTag, myOsTag));
            return;
        }
    }
    if (!osArch.isEmpty()) {
        const QString myArch = OkidiaUtils::detectLocalOsArch();
        if (!myArch.isEmpty() && myArch != OkidiaUtils::normalizeArch(osArch)) {
            Logger::info(QObject::tr("MainWindow: Farklı bir mimari için Master güncellemesi bildirildi (%1 != %2), yoksayılıyor.").arg(osArch, myArch));
            return;
        }
    }

    QString curVer = Okidia::Version::string();

    // Aynı veya daha düşük sürüm için güncelleme uyarısı gösterme
    if (OkidiaUtils::compareVersions(version, curVer) <= 0) {
        m_masterUpdateBtn->setVisible(false);
        Logger::info(QObject::tr("MainWindow: Mevcut sürüm (v%1) yeni sürüme (v%2) eşit veya daha yüksek, güncelleme butonu gizlendi.")
                     .arg(curVer, version));
        return;
    }

    m_masterUpdateBtn->setIcon(ThemeManager::icon(":/ui/refresh.svg"));
    m_masterUpdateBtn->setText(tr("Yeni Sürüm Mevcut (v%1) — Güncelle").arg(version));
    m_masterUpdateBtn->setVisible(true);
}

void MainWindow::onMasterUpdateBtnClicked() {
    if (!m_session) return;
    auto info = m_session->availableMasterUpdate();
    if (!info.available) return;

    MasterUpdateDialog dlg(m_session, info.version, info.filename, info.size, info.osTag, info.osArch, this);
    dlg.exec();
}

void MainWindow::onBrokerOutdatedWarning(const QString& brokerVer, const QString& masterVer) {
    QTimer::singleShot(600, this, [this, brokerVer, masterVer]() {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("Sunucu Güncellemesi Gerekli"));
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setText(tr("<h3>⚠️ Sunucu (Broker) Sürümünüz Güncel Değil!</h3>"));
        msgBox.setInformativeText(
            QString(tr("Bağlandığınız Broker sunucusu (<b>v%1</b>), bu Master uygulamasından (<b>v%2</b>) daha eski bir sürümde çalışıyor.<br><br>"
                       "• Veritabanı ve protokol uyumluluğunun korunması için <b>lütfen sunucuyu güncelleyin</b>.<br>"
                       "• <i>(Sunucu güncellenene kadar diğer öğretmenlerin ve gözlemcilerin giriş yapması engellenecektir.)</i><br><br>"
                       "Sunucuyu şimdi 'Sistem Güncelleme' panelinden veya SSH üzerinden güncelleyebilirsiniz."))
            .arg(brokerVer, masterVer)
        );
        QPushButton* updatePanelBtn = msgBox.addButton(tr("Sistem Güncelleme Panelini Aç"), QMessageBox::ActionRole);
        msgBox.addButton(tr("Daha Sonra"), QMessageBox::RejectRole);
        msgBox.exec();

        if (msgBox.clickedButton() == updatePanelBtn) {
            showSystemManagementWindow(m_otaUpdatePanel);
        }
    });
}

void MainWindow::onBrokerTimeWarning(bool isDesync, int remainingLogins, const QString& warningMsg, bool isLocked) {
    QTimer::singleShot(800, this, [this, isDesync, remainingLogins, warningMsg, isLocked]() {
        if (!isDesync) return;

        if (isLocked) {
            QMessageBox::critical(this, tr("Kritik Güvenlik Kilidi: Sunucu Saati Hatası"),
                warningMsg + "\n\n" +
                tr("Sunucu saati düzeltilene veya anakart pili değiştirilene kadar sisteme erişim durdurulmuştur."));
            emit logoutRequested();
        } else {
            QMessageBox::warning(this, tr("Sunucu Saat / BIOS Pili Uyarısı"),
                warningMsg + "\n\n" +
                tr("Kalan Oturum Açma Hakkı: %1/3\n"
                   "Lütfen en kısa sürede Broker sunucusunun saat ayarını yapın veya anakart BIOS pilini yenileyin.")
                .arg(remainingLogins));
        }
    });
}

void MainWindow::onClientHardwareAlert(const QString& machineId, const QString& hostname, const QString& alertType, int count, const QString& details) {
    QTimer::singleShot(1200, this, [this, machineId, hostname, alertType, count, details]() {
        QString displayName = hostname.isEmpty() ? machineId : hostname;

        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("Donanım Arıza Bildirimi"));
        msgBox.setIcon(QMessageBox::Warning);
        msgBox.setText(tr("<h3>🔋 Akıllı Tahta Donanım Arızası: %1</h3>").arg(displayName));
        msgBox.setInformativeText(
            tr("<b>Cihaz:</b> %1 (%2)<br>"
               "<b>Arıza Tipi:</b> %3 (Anakart BIOS Pili)<br>"
               "<b>Hata Tekrarı:</b> %4 kez üst üste<br><br>"
               "%5<br><br>"
               "<i>Not: Zilsiz Okul zil/anons senkronizasyonunun ve sertifika güvenliğinin aksamaması için lütfen cihazın BIOS pilini yenileyin.</i>")
            .arg(displayName, machineId, alertType, QString::number(count), details)
        );
        msgBox.addButton(tr("Anladım"), QMessageBox::AcceptRole);
        msgBox.exec();
    });
}

void MainWindow::onUserRoleChanged(const QString& newRole, const QString& displayName, const QString& message) {
    Q_UNUSED(displayName);
    QString roleTitle;
    if (newRole == "YM") {
        roleTitle = tr("Yönetici (YM)");
    } else if (newRole == "OM") {
        roleTitle = tr("Öğretmen (OM)");
    } else if (newRole == "GM") {
        roleTitle = tr("Gözlemci (GM)");
    } else {
        roleTitle = newRole;
    }

    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("Kullanıcı Yetkisi Güncellendi"));
    msgBox.setIcon(QMessageBox::Information);
    msgBox.setText(tr("<h3>Kullanıcı Yetkiniz Güncellendi</h3>"));
    QString info = tr("Hesap yetkiniz <b>%1</b> olarak değiştirildi.<br><br>").arg(roleTitle);
    if (!message.isEmpty()) {
        info += message + "<br><br>";
    }
    info += tr("Sınıf atamaları, oda yöneticilikleri ve arayüz yetkilerinin eksiksiz uygulanabilmesi için lütfen oturumu kapatıp yeniden giriş yapınız.");
    msgBox.setInformativeText(info);

    QPushButton* btnRelogin = msgBox.addButton(tr("Şimdi Yeniden Giriş Yap"), QMessageBox::AcceptRole);
    msgBox.addButton(tr("Daha Sonra"), QMessageBox::RejectRole);

    msgBox.exec();

    if (msgBox.clickedButton() == btnRelogin) {
        emit logoutRequested();
    }
}

void MainWindow::onBrokerStorageWarning(quint64 availableBytes, quint64 requiredBytes, const QString& warningMsg) {
    Q_UNUSED(availableBytes);
    Q_UNUSED(requiredBytes);
    QTimer::singleShot(500, this, [this, warningMsg]() {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("Sunucu Depolama Uyarısı"));
        msgBox.setIcon(QMessageBox::Critical);
        msgBox.setText(tr("<h3>Broker Sunucusunda Yetersiz Disk Alanı!</h3>"));
        msgBox.setInformativeText(
            tr("Sunucu diski kritik doluluk eşiğine ulaştığı için <b>otomatik sistem yedeklemesi yapılamadı</b>.<br><br>"
               "<b>Detay:</b> %1<br><br>"
               "<i>Sistem bütünlüğü ve veri kaybı riskini önlemek için lütfen sunucuda gereksiz dosyaları silin veya eski yedekleri harici diske aktarınız.</i>")
            .arg(warningMsg)
        );
        msgBox.addButton(tr("Anladım"), QMessageBox::AcceptRole);
        msgBox.exec();
    });
}

void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ActivationChange) {
        if (!isActiveWindow()) {
            ClientThumbnail::hideLiveTooltip();
        }
    } else if (event->type() == QEvent::WindowStateChange) {
        if (isMinimized()) {
            ClientThumbnail::hideLiveTooltip();
            // Master simge durumuna küçültüldü: arka planda boşuna thumbnail çekip ağı meşgul etmesin
            if (m_session) {
                Logger::info(QObject::tr("MainWindow: Simge durumuna küçüldü. Canlı thumbnail akışı duraklatılıyor (SCREEN_STREAM_PAUSE)."));
                m_session->setVisibleClients({});
            }
        } else {
            // Tekrar açıldı/öne geldi: mevcut layout'u bozmadan görünür liste üzerinden thumbnail akışını yeniden başlat
            if (m_session) {
                Logger::info(QObject::tr("MainWindow: Simge durumundan çıkarıldı. Görünür istemciler için akış yenileniyor."));
                QStringList visibleList;
                for (auto* frame : m_groupFrames) {
                    if (frame && frame->isVisible()) {
                        visibleList.append(frame->clientIds());
                    }
                }
                if (visibleList.isEmpty()) {
                    visibleList = m_thumbnails.keys();
                }
                updateStatusBar(visibleList);
            }
        }
    }
}

