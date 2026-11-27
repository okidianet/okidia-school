// master/ui/LiveAnnounceDialog/LiveAnnounceDialog.cpp
#include "LiveAnnounceDialog.h"
#include "../../session/SessionManager.h"
#include "../../../common/ui/ThemeManager.h"
#include "../../../common/utils/Logger.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QGroupBox>
#include <QProgressBar>

LiveAnnounceDialog::LiveAnnounceDialog(SessionManager* session,
                                       const QStringList& targets,
                                       const QString& targetDescription,
                                       QWidget* parent)
    : QDialog(parent)
    , m_session(session)
    , m_targets(targets)
    , m_targetDesc(targetDescription)
{
    setWindowTitle(tr("Canlı Anons (Mikrofon)"));
    setMinimumWidth(440);
    setModal(true);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setSpacing(14);
    rootLayout->setContentsMargins(18, 18, 18, 18);

    // Hedef Bilgisi
    auto* infoGroup = new QGroupBox(tr("Yayın Hedefi"), this);
    auto* infoLayout = new QVBoxLayout(infoGroup);
    auto* lblTargets = new QLabel(
        m_targetDesc.isEmpty()
            ? (m_targets.isEmpty() ? tr("Tüm Okul / İstemciler") : tr("%1 istemci seçili").arg(m_targets.size()))
            : m_targetDesc,
        infoGroup);
    lblTargets->setStyleSheet("font-weight: 500;");
    infoLayout->addWidget(lblTargets);
    rootLayout->addWidget(infoGroup);

    // Mikrofon Seçimi
    auto* micGroup = new QGroupBox(tr("Mikrofon"), this);
    auto* micLayout = new QVBoxLayout(micGroup);
    m_micCombo = new QComboBox(micGroup);
    micLayout->addWidget(m_micCombo);
    rootLayout->addWidget(micGroup);

    // Durum ve Süre
    auto* statusBox = new QVBoxLayout();
    statusBox->setAlignment(Qt::AlignCenter);

    m_timerLabel = new QLabel("00:00", this);
    m_timerLabel->setStyleSheet("font-size: 24px; font-weight: bold; font-family: monospace;");
    m_timerLabel->setAlignment(Qt::AlignCenter);
    statusBox->addWidget(m_timerLabel);

    m_statusLabel = new QLabel(tr("Canlı yayını başlatmak için aşağıdaki butona basın."), this);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setWordWrap(true);
    statusBox->addWidget(m_statusLabel);

    m_levelBar = new QProgressBar(this);
    m_levelBar->setRange(0, 100);
    m_levelBar->setValue(0);
    m_levelBar->setTextVisible(false);
    m_levelBar->setFixedHeight(10);
    m_levelBar->setToolTip(tr("Canlı mikrofon ses seviyesi"));
    statusBox->addWidget(m_levelBar);

    rootLayout->addLayout(statusBox);

    if (m_session) {
        connect(m_session, &SessionManager::audioInputLevelChanged, this, [this](int pct) {
            if (m_levelBar) {
                m_levelBar->setValue(qBound(0, pct, 100));
            }
        });
    }

    // Butonlar
    m_btnBroadcast = new QPushButton(tr("Canlı Yayını Başlat"), this);
    m_btnBroadcast->setIcon(ThemeManager::icon(":/ui/record.svg"));
    m_btnBroadcast->setFixedHeight(42);
    m_btnBroadcast->setStyleSheet("font-size: 14px; font-weight: bold;");

    m_btnClose = new QPushButton(tr("Kapat"), this);
    m_btnClose->setFixedHeight(34);

    rootLayout->addWidget(m_btnBroadcast);
    rootLayout->addWidget(m_btnClose);

    // Zamanlayıcı
    m_elapsedTimer = new QTimer(this);
    m_elapsedTimer->setInterval(1000);
    connect(m_elapsedTimer, &QTimer::timeout, this, &LiveAnnounceDialog::onTimerTick);

    connect(m_btnBroadcast, &QPushButton::clicked, this, &LiveAnnounceDialog::onToggleBroadcast);
    connect(m_btnClose, &QPushButton::clicked, this, &QDialog::accept);

    populateMicrophones();
    setLiveState(false);
}

LiveAnnounceDialog::~LiveAnnounceDialog() {
    if (m_session && m_session->isLiveAudioBroadcasting()) {
        m_session->stopLiveAudioBroadcast();
    }
}

void LiveAnnounceDialog::populateMicrophones() {
    m_micCombo->clear();
    const QList<QAudioDevice> inputs = QMediaDevices::audioInputs();
    int bestIndex = -1;

    for (int i = 0; i < inputs.size(); ++i) {
        const auto& dev = inputs[i];
        QString desc = dev.description().trimmed();
        if (desc.isEmpty()) desc = tr("Ses Giriş Aygıtı");
        m_micCombo->addItem(ThemeManager::icon(":/ui/volume.svg"), desc, dev.id());

        if (dev.isDefault() && bestIndex < 0) {
            bestIndex = i;
        }
    }

    if (m_micCombo->count() == 0) {
        m_micCombo->addItem(ThemeManager::icon(":/ui/volume.svg"), tr("Sistem Varsayılan Mikrofonu"), QByteArray("default"));
        bestIndex = 0;
    }

    if (bestIndex >= 0 && bestIndex < m_micCombo->count()) {
        m_micCombo->setCurrentIndex(bestIndex);
    }
}

void LiveAnnounceDialog::onToggleBroadcast() {
    if (!m_session) return;

    if (!m_session->isLiveAudioBroadcasting()) {
        QAudioDevice selectedDev;
        if (m_micCombo && m_micCombo->count() > 0) {
            QByteArray devId = m_micCombo->currentData().toByteArray();
            if (devId != "default" && !devId.isEmpty()) {
                for (const auto& dev : QMediaDevices::audioInputs()) {
                    if (dev.id() == devId) {
                        selectedDev = dev;
                        break;
                    }
                }
            }
        }

        if (!m_session->startLiveAudioBroadcast(m_targets, selectedDev)) {
            m_statusLabel->setText(tr("Mikrofon başlatılamadı!"));
            return;
        }

        setLiveState(true);
    } else {
        m_session->stopLiveAudioBroadcast();
        setLiveState(false);
    }
}

void LiveAnnounceDialog::setLiveState(bool isLive) {
    m_micCombo->setEnabled(!isLive);

    if (isLive) {
        m_btnBroadcast->setText(tr("Yayını Durdur"));
        m_btnBroadcast->setIcon(ThemeManager::icon(":/ui/stop.svg"));
        m_btnBroadcast->setStyleSheet("background-color: #dc2626; color: white; font-size: 14px; font-weight: bold; border-radius: 6px;");
        m_statusLabel->setText(tr("<b style='color: #ef4444;'>● CANLI YAYINDA</b><br>Mikrofona konuşun, sesiniz anlık iletiliyor..."));
        m_elapsedSeconds = 0;
        m_timerLabel->setText("00:00");
        m_elapsedTimer->start();
    } else {
        m_elapsedTimer->stop();
        if (m_levelBar) m_levelBar->setValue(0);
        m_btnBroadcast->setText(tr("Canlı Yayını Başlat"));
        m_btnBroadcast->setIcon(ThemeManager::icon(":/ui/record.svg"));
        m_btnBroadcast->setStyleSheet("font-size: 14px; font-weight: bold;");
        m_statusLabel->setText(tr("Canlı yayını başlatmak için butona basın."));
    }
}

void LiveAnnounceDialog::onTimerTick() {
    ++m_elapsedSeconds;
    int mins = m_elapsedSeconds / 60;
    int secs = m_elapsedSeconds % 60;
    m_timerLabel->setText(QString("%1:%2")
                              .arg(mins, 2, 10, QChar('0'))
                              .arg(secs, 2, 10, QChar('0')));
}
