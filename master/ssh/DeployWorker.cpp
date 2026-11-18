// master/ssh/DeployWorker.cpp
#include "DeployWorker.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include <QFileInfo>
#include <QDir>
#include <QThread>
#include <QSemaphore>

static QSemaphore s_uploadSemaphore(2); // Zayıf Wi-Fi ağlarını tıkamamak için azami 2 eşzamanlı dosya aktarımı

DeployWorker::DeployWorker(const TargetInfo& target, QObject* parent)
    : QObject(parent)
    , m_target(target)
{
    setAutoDelete(true);
}

void DeployWorker::run() {
    emit stepChanged(m_target.ip, static_cast<int>(Step::Connecting), tr("SSH Bağlantısı Kuruluyor..."));
    emit logMessage(m_target.ip, tr("SSH bağlantısı başlatılıyor (%1:22, Kullanıcı: %2)...")
                                .arg(m_target.ip, m_target.sshConfig.username));

    SshClient client;
    bool connOk = false;
    for (int connAttempt = 0; connAttempt < 2 && !connOk; ++connAttempt) {
        if (connAttempt > 0) {
            emit logMessage(m_target.ip, tr("İlk SSH bağlantı denemesi zaman aşımına uğradı, yeniden deneniyor..."));
            QThread::msleep(1500);
        }
        client.connectToHost(m_target.sshConfig);
        connOk = client.isConnected();
    }

    if (!connOk) {
        QString err = client.lastError().isEmpty() ? tr("SSH bağlantısı başarısız oldu.") : client.lastError();
        emit stepChanged(m_target.ip, static_cast<int>(Step::Failed), tr("Bağlantı Hatası"));
        emit logMessage(m_target.ip, tr("HATA: %1").arg(err));
        emit finished(m_target.ip, false, err, err);
        return;
    }

    emit logMessage(m_target.ip, tr("SSH bağlantısı başarılı. SFTP ile paket yükleme başlıyor..."));
    emit stepChanged(m_target.ip, static_cast<int>(Step::Uploading), tr("Paket Yükleniyor (%0)..."));

    QFileInfo fi(m_target.pkgPath);
    const bool isWindowsPkg = m_target.pkgPath.endsWith(".exe", Qt::CaseInsensitive);
    QString remoteTmp = isWindowsPkg ? ("C:/Windows/Temp/" + fi.fileName()) : ("/tmp/" + fi.fileName());

    emit stepChanged(m_target.ip, static_cast<int>(Step::Uploading), tr("Yükleme Sırası Bekleniyor..."));
    s_uploadSemaphore.acquire();

    emit stepChanged(m_target.ip, static_cast<int>(Step::Uploading), tr("Paket Yükleniyor (%0)..."));
    bool uploadOk = false;
    for (int uploadAttempt = 0; uploadAttempt < 2 && !uploadOk; ++uploadAttempt) {
        if (uploadAttempt > 0) {
            emit logMessage(m_target.ip, tr("SFTP yüklemesinde paket kaybı oluştu. 1.5 saniye sonra tekrar deneniyor..."));
            QThread::msleep(1500);
        }
        uploadOk = client.uploadFile(m_target.pkgPath, remoteTmp, [this](qint64 sent, qint64 total) {
            int pct = total > 0 ? static_cast<int>((sent * 100) / total) : 0;
            emit uploadProgress(m_target.ip, sent, total, pct);
            emit stepChanged(m_target.ip, static_cast<int>(Step::Uploading), tr("Paket Yükleniyor (%%1)").arg(pct));
        });
    }

    s_uploadSemaphore.release();

    if (!uploadOk) {
        QString err = client.lastError().isEmpty() ? tr("SFTP dosya yükleme başarısız oldu.") : client.lastError();
        client.disconnectFromHost();
        emit stepChanged(m_target.ip, static_cast<int>(Step::Failed), tr("Yükleme Hatası"));
        emit logMessage(m_target.ip, tr("HATA: %1").arg(err));
        emit finished(m_target.ip, false, err, err);
        return;
    }

    emit logMessage(m_target.ip, tr("Paket yüklendi: %1. Kurulum komutu çalıştırılıyor...").arg(remoteTmp));
    emit stepChanged(m_target.ip, static_cast<int>(Step::Installing), isWindowsPkg ? tr("Kuruluyor (Inno Setup)...") : tr("Kuruluyor (dpkg / apt)..."));

    QString cmd;
    if (isWindowsPkg) {
        cmd = QString("cmd.exe /c \"\"%1\" /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /LOG=\"C:\\Windows\\Temp\\okidia_inno_setup.log\"\" > C:\\Windows\\Temp\\okidia_install.log 2>&1").arg(QDir::toNativeSeparators(remoteTmp));
    } else {
        if (m_target.sshConfig.username == "root") {
            cmd = "env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin DEBIAN_FRONTEND=noninteractive sh -c 'for i in 1 2 3 4 5; do dpkg --force-confdef --force-confold -i " + remoteTmp + "; if [ $? -eq 0 ]; then exit 0; fi; apt-get install -f -y --no-install-recommends; dpkg --force-confdef --force-confold -i " + remoteTmp + "; if [ $? -eq 0 ]; then exit 0; fi; sleep 3; done; exit 1' > /tmp/okidia_install.log 2>&1";
        } else {
            QString escPass = m_target.sshConfig.password;
            escPass.replace("'", "'\\''");
            cmd = QString("echo '%1' | sudo -S env -i PATH=/usr/sbin:/usr/bin:/sbin:/bin DEBIAN_FRONTEND=noninteractive sh -c 'for i in 1 2 3 4 5; do dpkg --force-confdef --force-confold -i %2; if [ $? -eq 0 ]; then exit 0; fi; apt-get install -f -y --no-install-recommends; dpkg --force-confdef --force-confold -i %2; if [ $? -eq 0 ]; then exit 0; fi; sleep 3; done; exit 1' > /tmp/okidia_install.log 2>&1").arg(escPass, remoteTmp);
        }
    }

    int exitCode = -1;
    client.executeSync(cmd, 300000, &exitCode);

    QString installLog = client.executeSync(
        OkidiaUtils::remoteReadLogCmd(isWindowsPkg ? "C:\\Windows\\Temp\\okidia_install.log" : "/tmp/okidia_install.log", isWindowsPkg),
        5000).trimmed();

    if (exitCode == 0) {
        // Zero-Config (Sıfır Konfigürasyon & IP Bağımsız Mimari):
        // İstemciler hiçbir zaman statik bir Broker IP'sine kilitlenmez. Broker çoklu ağ kartında
        // (multi-homed) tüm bacaklarda mDNS anonsu yapar. Hedefteki eski/artık statik config temizlenerek
        // ajanın Avahi/Bonjour mDNS üzerinden dinamik keşif yapması sağlanır.
        emit logMessage(m_target.ip, tr("Sıfır-Konfigürasyon (Zero-Config mDNS) modu doğrulanıyor..."));

        // ── L3 / VLAN ve DNS Fallback Desteği (okidia-broker) ──
        // mDNS paketleri VLAN ve L3 yönlendiricileri aşamadığı için; hedef makinenin içinden
        // Broker aday IP'leri test edilir ve ilk erişilebilen IP hedefteki hosts dosyasına
        // okidia-broker olarak eklenir. Ajan mDNS'ten 5 saniye yanıt alamazsa otomatik buraya düşer.
        if (!m_target.brokerCandidateIps.isEmpty()) {
            emit logMessage(m_target.ip, tr("L3/VLAN Fallback: Hedef makineden Broker bağlantı testi yapılıyor..."));
            if (isWindowsPkg) {
                QStringList quotedIps;
                for (const QString& ip : m_target.brokerCandidateIps) {
                    quotedIps << QString("'%1'").arg(ip);
                }
                QString psProbeCmd = QString(
                    "powershell -Command \""
                    "$port = %1; $ips = @(%2); $found = ''; "
                    "foreach ($ip in $ips) { "
                    "  try { "
                    "    $c = New-Object System.Net.Sockets.TcpClient; "
                    "    $a = $c.BeginConnect($ip, $port, $null, $null); "
                    "    if ($a.AsyncWaitHandle.WaitOne(1000, $false) -and $c.Connected) { "
                    "      $c.EndConnect($a); $c.Close(); $found = $ip; break; "
                    "    } "
                    "    $c.Close(); "
                    "  } catch {} "
                    "} "
                    "if ($found -ne '') { "
                    "  $hosts = \\\"$env:SystemRoot\\System32\\drivers\\etc\\hosts\\\"; "
                    "  $lines = (Get-Content $hosts -ErrorAction SilentlyContinue) | Where-Object { $_ -notmatch 'okidia-broker' }; "
                    "  $lines += \\\"$found`tokidia-broker\\\"; "
                    "  Set-Content -Path $hosts -Value $lines -Force; "
                    "  Write-Output $found; "
                    "}\""
                ).arg(m_target.brokerPort).arg(quotedIps.join(", "));

                QString foundIp = client.executeSync(psProbeCmd, 15000).trimmed();
                if (!foundIp.isEmpty()) {
                    emit logMessage(m_target.ip, tr("L3/VLAN Fallback: Broker IP'si (%1) doğrulandı ve hosts dosyasına (okidia-broker) eklendi.").arg(foundIp));
                } else {
                    emit logMessage(m_target.ip, tr("UYARI: Hedef cihazdan hiçbir Broker IP'sine (%1) %2 portundan erişilemedi. Okul router/güvenlik duvarı (VLAN) kurallarını kontrol edin.")
                        .arg(m_target.brokerCandidateIps.join(", ")).arg(m_target.brokerPort));
                }
            } else {
                QString escPass = m_target.sshConfig.password;
                escPass.replace("'", "'\\''");

                QString ipSpaceList = m_target.brokerCandidateIps.join(" ");
                QString probeScript = QString(
                    "for ip in %1; do "
                    "  if timeout 1 bash -c \"</dev/tcp/$ip/%2\" 2>/dev/null; then "
                    "    echo $ip; break; "
                    "  fi; "
                    "done"
                ).arg(ipSpaceList).arg(m_target.brokerPort);

                QString foundIp = client.executeSync(probeScript, 10000).trimmed();
                if (!foundIp.isEmpty()) {
                    QString updateHostsCmd;
                    if (m_target.sshConfig.username == "root") {
                        updateHostsCmd = QString("sed -i '/okidia-broker/d' /etc/hosts && echo '%1 okidia-broker' >> /etc/hosts").arg(foundIp);
                    } else {
                        updateHostsCmd = QString("echo '%1' | sudo -S sh -c \"sed -i '/okidia-broker/d' /etc/hosts && echo '%2 okidia-broker' >> /etc/hosts\"")
                            .arg(escPass, foundIp);
                    }
                    client.executeSync(updateHostsCmd, 10000);
                    emit logMessage(m_target.ip, tr("L3/VLAN Fallback: Broker IP'si (%1) doğrulandı ve /etc/hosts'a (okidia-broker) eklendi.").arg(foundIp));
                } else {
                    emit logMessage(m_target.ip, tr("UYARI: Hedef cihazdan hiçbir Broker IP'sine (%1) %2 portundan erişilemedi. Okul router/güvenlik duvarı (VLAN) kurallarını kontrol edin.")
                        .arg(m_target.brokerCandidateIps.join(", ")).arg(m_target.brokerPort));
                }
            }
        }

        if (isWindowsPkg) {
            QString cleanCmd = "powershell -Command \"if (Test-Path $env:ProgramData\\Okidia\\config) { "
                               "Remove-Item -Force $env:ProgramData\\Okidia\\config -ErrorAction SilentlyContinue }; "
                               "Restart-Service OkidiaAgent -ErrorAction SilentlyContinue\"";
            client.executeSync(cleanCmd, 15000);
        } else {
            QString escPass = m_target.sshConfig.password;
            escPass.replace("'", "'\\''");
            QString cleanCmd;
            if (m_target.sshConfig.username == "root") {
                cleanCmd = "rm -f /etc/okidia/config && systemctl daemon-reload && systemctl unmask okidia-agent.service && systemctl enable okidia-agent.service && systemctl restart okidia-agent.service";
            } else {
                cleanCmd = QString("echo '%1' | sudo -S sh -c \"rm -f /etc/okidia/config && systemctl daemon-reload && systemctl unmask okidia-agent.service && systemctl enable okidia-agent.service && systemctl restart okidia-agent.service\"").arg(escPass);
            }
            client.executeSync(cleanCmd, 15000);
        }

        emit stepChanged(m_target.ip, static_cast<int>(Step::Verifying), tr("Servis Doğrulanıyor..."));
        emit logMessage(m_target.ip, tr("Kurulum tamamlandı, servis ve sürüm doğrulanıyor..."));

        QString verStr;
        if (isWindowsPkg) {
            QString scOut = client.executeSync(OkidiaUtils::remoteServiceQueryCmd("OkidiaAgent", true), 5000).trimmed();
            verStr = scOut.contains("RUNNING") ? tr("Servis Aktif") : tr("Servis Pasif");
        } else {
            QString okidiaVer = client.executeSync("dpkg-query -W -f='${Version}' okidia-agent 2>/dev/null", 5000).trimmed();
            verStr = okidiaVer.isEmpty() ? QString() : ("v" + okidiaVer);

            QString escPass = m_target.sshConfig.password;
            escPass.replace("'", "'\\''");
            QString checkActCmd = (m_target.sshConfig.username == "root")
                ? "systemctl is-active okidia-agent.service"
                : QString("echo '%1' | sudo -S systemctl is-active okidia-agent.service").arg(escPass);
            QString isAct = client.executeSync(checkActCmd, 5000).trimmed();

            if (isAct.contains("active")) {
                verStr = verStr.isEmpty() ? tr("Servis Aktif") : (verStr + tr(" (Servis Aktif)"));
                emit logMessage(m_target.ip, tr("okidia-agent.service başarıyla çalışıyor."));
            } else {
                emit logMessage(m_target.ip, tr("UYARI: okidia-agent.service aktif değil (Durum: %1). Yeniden başlatma deneniyor...").arg(isAct));
                QString restartCmd = (m_target.sshConfig.username == "root")
                    ? "systemctl restart okidia-agent.service"
                    : QString("echo '%1' | sudo -S systemctl restart okidia-agent.service").arg(escPass);
                client.executeSync(restartCmd, 10000);

                isAct = client.executeSync(checkActCmd, 5000).trimmed();
                if (isAct.contains("active")) {
                    verStr = verStr.isEmpty() ? tr("Servis Aktif") : (verStr + tr(" (Servis Aktif)"));
                    emit logMessage(m_target.ip, tr("okidia-agent.service başarıyla kurtarıldı ve aktif edildi."));
                } else {
                    emit logMessage(m_target.ip, tr("HATA: okidia-agent.service başlatılamadı! Durum: %1").arg(isAct));
                    QString svcLog = client.executeSync(
                        (m_target.sshConfig.username == "root")
                            ? "journalctl -u okidia-agent.service -n 15 --no-pager 2>/dev/null"
                            : QString("echo '%1' | sudo -S journalctl -u okidia-agent.service -n 15 --no-pager 2>/dev/null").arg(escPass),
                        5000).trimmed();
                    if (!svcLog.isEmpty()) {
                        emit logMessage(m_target.ip, tr("Servis Günlük Detayı:\n%1").arg(svcLog));
                    }
                }
            }
        }

        client.disconnectFromHost();

        const QString statusLabel = verStr.isEmpty() ? tr("Kuruldu") : tr("Kuruldu (%1)").arg(verStr);
        emit stepChanged(m_target.ip, static_cast<int>(Step::Completed), statusLabel);
        emit logMessage(m_target.ip, tr("BAŞARILI: Okidia Agent %1.").arg(statusLabel.toLower()));
        emit finished(m_target.ip, true, verStr, installLog);
    } else {
        QString errReason = tr("Kurulum komutu hata kodu %1 ile sonlandı.").arg(exitCode);
        if (isWindowsPkg) {
            QString innoLog = client.executeSync(
                OkidiaUtils::remoteReadLogCmd("C:\\Windows\\Temp\\okidia_inno_setup.log", true),
                5000).trimmed();
            if (!innoLog.isEmpty()) {
                installLog = innoLog;
            }
        }
        if (installLog.isEmpty()) {
            installLog = errReason;
        }

        client.disconnectFromHost();

        emit stepChanged(m_target.ip, static_cast<int>(Step::Failed), tr("Kurulum Hatası"));
        emit logMessage(m_target.ip, tr("HATA: %1\nDetay: %2").arg(errReason, installLog));
        emit finished(m_target.ip, false, errReason, installLog);
    }
}
