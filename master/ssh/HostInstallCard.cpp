// master/ssh/HostInstallCard.cpp
#include "HostInstallCard.h"
#include "../../common/ui/ThemeManager.h"
#include <QFileInfo>
#include <QDialog>
#include <QTextEdit>
#include <QDialogButtonBox>
#include <QApplication>
#include <QJsonArray>
#include <QDateTime>

HostInstallCard::HostInstallCard(const QString& ip,
                                 const QString& hostname,
                                 const QString& osInfo,
                                 const QString& pkgPath,
                                 QWidget* parent)
    : QFrame(parent)
    , m_ip(ip)
    , m_hostname(hostname.isEmpty() ? ip : hostname)
    , m_osInfo(osInfo)
    , m_pkgPath(pkgPath)
{
    setupUi();
    updateCardStyle();
}

void HostInstallCard::setupUi() {
    setObjectName("hostInstallCard");
    setFrameShape(QFrame::StyledPanel);
    setMinimumWidth(240);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(12, 10, 12, 10);
    mainLay->setSpacing(6);

    // ── Üst Başlık Satırı (İkon + Hostname + IP) ───────────────────
    auto* topRow = new QHBoxLayout();
    topRow->setSpacing(8);

    m_iconLabel = new QLabel(this);
    m_iconLabel->setPixmap(ThemeManager::icon(":/ui/monitor.svg").pixmap(20, 20));
    m_iconLabel->setFixedSize(20, 20);
    topRow->addWidget(m_iconLabel);

    auto* titleLay = new QVBoxLayout();
    titleLay->setSpacing(1);

    m_hostnameLabel = new QLabel(m_hostname, this);
    QFont hFont = m_hostnameLabel->font();
    hFont.setBold(true);
    hFont.setPointSize(10);
    m_hostnameLabel->setFont(hFont);
    m_hostnameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titleLay->addWidget(m_hostnameLabel);

    m_ipLabel = new QLabel(m_ip, this);
    QFont ipFont = m_ipLabel->font();
    ipFont.setPointSize(8);
    m_ipLabel->setFont(ipFont);
    m_ipLabel->setProperty("status", "muted");
    m_ipLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    titleLay->addWidget(m_ipLabel);

    topRow->addLayout(titleLay, 1);
    mainLay->addLayout(topRow);

    // ── İşletim Sistemi / Paket Etiketi ───────────────────────────
    QFileInfo fi(m_pkgPath);
    QString pkgName = fi.fileName();
    QString osAndPkg = m_osInfo.isEmpty() ? pkgName : QString("%1 — %2").arg(m_osInfo, pkgName);

    m_osLabel = new QLabel(osAndPkg, this);
    QFont osFont = m_osLabel->font();
    osFont.setPointSize(8);
    m_osLabel->setFont(osFont);
    m_osLabel->setProperty("status", "muted");
    m_osLabel->setWordWrap(true);
    mainLay->addWidget(m_osLabel);

    // ── Durum Satırı (İkon + Metin) ───────────────────────────────
    auto* statusRow = new QHBoxLayout();
    statusRow->setSpacing(6);

    m_statusIconLabel = new QLabel(this);
    m_statusIconLabel->setPixmap(ThemeManager::icon(":/ui/clock.svg").pixmap(16, 16));
    m_statusIconLabel->setFixedSize(16, 16);
    statusRow->addWidget(m_statusIconLabel);

    m_statusLabel = new QLabel(tr("Sırada Bekliyor..."), this);
    QFont stFont = m_statusLabel->font();
    stFont.setBold(true);
    stFont.setPointSize(9);
    m_statusLabel->setFont(stFont);
    statusRow->addWidget(m_statusLabel, 1);

    mainLay->addLayout(statusRow);

    // ── İlerleme Çubuğu ───────────────────────────────────────────
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setFixedHeight(8);
    m_progressBar->setTextVisible(false);
    mainLay->addWidget(m_progressBar);

    // ── Alt Buton Satırı (Günlük / Log İncele) ─────────────────────
    auto* botRow = new QHBoxLayout();
    botRow->setContentsMargins(0, 2, 0, 0);

    m_logBtn = new QPushButton(tr("Günlük İncele"), this);
    m_logBtn->setIcon(ThemeManager::icon(":/ui/file.svg"));
    m_logBtn->setProperty("class", "secondary");
    m_logBtn->setFixedHeight(24);
    m_logBtn->setCursor(Qt::PointingHandCursor);
    m_logBtn->setVisible(false); // Başlangıçta gizli, log dolunca veya hata olunca görünür
    botRow->addStretch();
    botRow->addWidget(m_logBtn);

    mainLay->addLayout(botRow);

    connect(m_logBtn, &QPushButton::clicked, this, &HostInstallCard::showLogDialog);
}

void HostInstallCard::updateCardStyle() {
    // Tema renklerine göre kart kenarlıklarını güncelle
    QString borderStyle;
    switch (m_step) {
        case DeployWorker::Step::Completed:
            borderStyle = "HostInstallCard#hostInstallCard { border: 1.5px solid #2ecc71; border-radius: 6px; }";
            break;
        case DeployWorker::Step::Failed:
            borderStyle = "HostInstallCard#hostInstallCard { border: 1.5px solid #e74c3c; border-radius: 6px; }";
            break;
        case DeployWorker::Step::QueuedOffline:
            borderStyle = "HostInstallCard#hostInstallCard { border: 1.5px solid #f39c12; border-radius: 6px; }";
            break;
        case DeployWorker::Step::Uploading:
        case DeployWorker::Step::Installing:
        case DeployWorker::Step::Verifying:
            borderStyle = "HostInstallCard#hostInstallCard { border: 1.5px solid #3498db; border-radius: 6px; }";
            break;
        default:
            borderStyle = "HostInstallCard#hostInstallCard { border: 1px solid palette(mid); border-radius: 6px; }";
            break;
    }
    setStyleSheet(borderStyle);
}

bool HostInstallCard::matchesTarget(const QString& target) const {
    if (target.isEmpty()) return true;
    if (target == m_ip) return true;
    if (!m_machineId.isEmpty() && target == m_machineId) return true;
    if (!m_hostname.isEmpty() && target.toLower() == m_hostname.toLower()) return true;
    return false;
}

void HostInstallCard::onStepChanged(const QString& ip, int step, const QString& statusText) {
    if (!matchesTarget(ip)) return;

    m_step = static_cast<DeployWorker::Step>(step);
    m_statusLabel->setText(statusText);

    switch (m_step) {
        case DeployWorker::Step::Connecting:
            m_statusIconLabel->setPixmap(ThemeManager::icon(":/ui/discovery.svg").pixmap(16, 16));
            m_progressBar->setRange(0, 0); // Indeterminate
            break;
        case DeployWorker::Step::Uploading:
            m_statusIconLabel->setPixmap(ThemeManager::icon(":/ui/hourglass.svg").pixmap(16, 16));
            m_progressBar->setRange(0, 100);
            break;
        case DeployWorker::Step::Installing:
            m_statusIconLabel->setPixmap(ThemeManager::icon(":/ui/root_drive.svg").pixmap(16, 16));
            m_progressBar->setRange(0, 0); // Indeterminate
            break;
        case DeployWorker::Step::Verifying:
            m_statusIconLabel->setPixmap(ThemeManager::icon(":/ui/refresh.svg").pixmap(16, 16));
            m_progressBar->setRange(0, 0);
            break;
        case DeployWorker::Step::QueuedOffline:
            m_statusIconLabel->setPixmap(ThemeManager::icon(":/ui/clock.svg").pixmap(16, 16));
            m_progressBar->setRange(0, 100);
            m_progressBar->setValue(0);
            m_logBtn->setText(tr("Günlük İncele"));
            m_logBtn->setProperty("class", "secondary");
            m_logBtn->setVisible(true);
            break;
        case DeployWorker::Step::Completed:
            m_statusIconLabel->setPixmap(ThemeManager::icon(":/ui/check_circle.svg").pixmap(16, 16));
            m_progressBar->setRange(0, 100);
            m_progressBar->setValue(100);
            m_logBtn->setText(tr("Günlük İncele"));
            m_logBtn->setProperty("class", "secondary");
            m_logBtn->setVisible(true);
            break;
        case DeployWorker::Step::Failed:
            m_statusIconLabel->setPixmap(ThemeManager::icon(":/ui/error_circle.svg").pixmap(16, 16));
            m_progressBar->setRange(0, 100);
            m_progressBar->setValue(100);
            m_logBtn->setText(tr("Hata Detayı"));
            m_logBtn->setProperty("class", "danger");
            m_logBtn->setVisible(true);
            break;
        default:
            m_statusIconLabel->setPixmap(ThemeManager::icon(":/ui/clock.svg").pixmap(16, 16));
            m_progressBar->setRange(0, 100);
            m_progressBar->setValue(0);
            break;
    }

    updateCardStyle();
    emit stateUpdated();
}

void HostInstallCard::onUploadProgress(const QString& ip, qint64 bytesSent, qint64 bytesTotal, int percent) {
    Q_UNUSED(bytesSent);
    Q_UNUSED(bytesTotal);
    if (!matchesTarget(ip)) return;

    if (m_step == DeployWorker::Step::Uploading) {
        m_progressBar->setRange(0, 100);
        m_progressBar->setValue(percent);
    }
}

void HostInstallCard::onLogMessage(const QString& ip, const QString& msg) {
    if (!matchesTarget(ip)) return;
    QString stamped = msg.startsWith("[")
        ? msg
        : QString("[%1] %2").arg(QDateTime::currentDateTime().toString("dd.MM.yyyy HH:mm:ss"), msg);
    m_logs.append(stamped);
    m_logBtn->setVisible(true);
    emit stateUpdated();
}

void HostInstallCard::onFinished(const QString& ip, bool success, const QString& versionOrError, const QString& fullLog) {
    if (!matchesTarget(ip)) return;

    m_fullLog = fullLog;
    if (success) {
        const QString label = versionOrError.isEmpty()
            ? tr("Kuruldu")
            : (versionOrError.startsWith(tr("Kuruldu")) ? versionOrError : tr("Kuruldu (%1)").arg(versionOrError));
        onStepChanged(ip, static_cast<int>(DeployWorker::Step::Completed), label);
    } else {
        onStepChanged(ip, static_cast<int>(DeployWorker::Step::Failed),
                      tr("Hata: %1").arg(versionOrError));
    }
}

QJsonObject HostInstallCard::toJson() const {
    QJsonObject obj;
    obj["ip"] = m_ip;
    obj["hostname"] = m_hostname;
    obj["osInfo"] = m_osInfo;
    obj["pkgPath"] = m_pkgPath;
    obj["machineId"] = m_machineId;
    obj["targetVersion"] = m_targetVersion;
    obj["step"] = static_cast<int>(m_step);
    obj["statusText"] = m_statusLabel ? m_statusLabel->text() : QString();
    obj["fullLog"] = m_fullLog;
    QJsonArray logArr;
    for (const QString& l : m_logs) {
        logArr.append(l);
    }
    obj["logs"] = logArr;
    return obj;
}

HostInstallCard* HostInstallCard::fromJson(const QJsonObject& obj, QWidget* parent) {
    QString ip = obj["ip"].toString();
    QString hostname = obj["hostname"].toString();
    QString osInfo = obj["osInfo"].toString();
    QString pkgPath = obj["pkgPath"].toString();

    auto* card = new HostInstallCard(ip, hostname, osInfo, pkgPath, parent);
    card->setMachineId(obj["machineId"].toString());
    card->setTargetVersion(obj["targetVersion"].toString());
    card->m_fullLog = obj["fullLog"].toString();

    QJsonArray logArr = obj["logs"].toArray();
    for (const QJsonValue& val : logArr) {
        card->m_logs.append(val.toString());
    }

    int savedStep = obj["step"].toInt(static_cast<int>(DeployWorker::Step::Pending));
    QString savedStatus = obj["statusText"].toString();

    // Master kapatıldığında yarıda kalmış aktif adımlar varsa (Uploading/Installing/Verifying),
    // yeniden açıldığında anlaşılır şekilde durumunu yansıt
    auto stepEnum = static_cast<DeployWorker::Step>(savedStep);
    if (stepEnum == DeployWorker::Step::Connecting ||
        stepEnum == DeployWorker::Step::Uploading ||
        stepEnum == DeployWorker::Step::Installing ||
        stepEnum == DeployWorker::Step::Verifying) {
        if (savedStatus.isEmpty()) {
            savedStatus = tr("İşlem Bekleniyor / Sürüm Kontrol Ediliyor...");
        }
    }

    card->onStepChanged(ip, savedStep, savedStatus);
    if (!card->m_logs.isEmpty() || !card->m_fullLog.isEmpty()) {
        card->m_logBtn->setVisible(true);
    }
    return card;
}

void HostInstallCard::showLogDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle(QString("%1 (%2) — %3").arg(m_hostname, m_ip, tr("Kurulum Günlüğü")));
    dlg.setWindowIcon(ThemeManager::icon(":/ui/file.svg"));
    dlg.resize(660, 440);

    auto* lay = new QVBoxLayout(&dlg);
    lay->setContentsMargins(12, 12, 12, 12);
    lay->setSpacing(8);

    auto* logEdit = new QTextEdit(&dlg);
    logEdit->setReadOnly(true);
    logEdit->setFont(QFont("Monospace", 9));

    QString combinedLog = m_logs.join("\n");
    if (!m_fullLog.isEmpty()) {
        combinedLog += "\n\n--- [" + tr("Sistem Kurulum / Durum Çıktısı") + "] ---\n" + m_fullLog;
    }
    if (combinedLog.trimmed().isEmpty()) {
        combinedLog = tr("Kayıtlı günlük mesajı bulunamadı.");
    }
    logEdit->setPlainText(combinedLog);
    lay->addWidget(logEdit);

    auto* btnRow = new QHBoxLayout();
    if (!m_machineId.isEmpty()) {
        auto* remoteLogBtn = new QPushButton(ThemeManager::icon(":/ui/monitor.svg"),
                                             tr("İstemci Sistem Günlüğünü Aç (Canlı)"), &dlg);
        remoteLogBtn->setProperty("class", "secondary");
        remoteLogBtn->setToolTip(tr("İstemci çevrimiçiyse doğrudan cihaz üzerindeki okidia-agent.log dosyasını canlı inceler."));
        connect(remoteLogBtn, &QPushButton::clicked, this, [this, &dlg]() {
            dlg.accept();
            emit remoteLogRequested(m_machineId, m_ip, m_hostname);
        });
        btnRow->addWidget(remoteLogBtn);
    }
    btnRow->addStretch();

    auto* btnBox = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
    connect(btnBox, &QDialogButtonBox::rejected, &dlg, &QDialog::accept);
    btnRow->addWidget(btnBox);
    lay->addLayout(btnRow);

    dlg.exec();
}
