#pragma once

#include <QTableWidget>
#include <QString>
#include <QColor>
#include "SshClient.h"

class SessionManager;
class SshCredSelectorWidget;

enum class DeployStatusRank : int {
  Ready = 1,        // Hazır
  Installed = 2,    // Kuruldu
  Installing = 3,   // Kuruluyor...
  SshTesting = 4,   // SSH Deneniyor / Taranıyor
  AwaitingCred = 5, // Erişim Bekleniyor
  SshMissing = 6,   // SSH Servisi Kapalı / Yüklü Değil (Muhtemel PC)
  Failed = 7,       // Başarısız / Kurulum Hatası
  Conflict = 8,     // Master/Broker Kurulu
  Unsupported = 9,  // Desteklenmiyor (Akıllı Cihaz / PC Değil)
  Unknown = 10      // Bilinmiyor / -
};

void setRowStatus(QTableWidget *table, int row,
                  const QString &statusText, DeployStatusRank rank,
                  const QColor &fgColor = QColor(),
                  const QString &tooltip = QString());

bool isRowEligibleForAgentDeploy(QTableWidget *table, int row);
void clearRowPackageCell(QTableWidget *table, int row);

// IP adresine göre tablodaki güncel satır numarasını döndürür.
// Tablo sıralandıktan sonra eski satır indeksi geçersiz kalabileceğinden
// dialog exec() veya async callback sonrasında bu fonksiyon kullanılmalıdır.
// Bulunamazsa -1 döner.
int rowForIp(QTableWidget *table, const QString &ip);

SshClient::Config buildConfigForTarget(SessionManager *session, const QString &ip,
                                       const QString &hostname,
                                       SshCredSelectorWidget *fallbackSelector);
