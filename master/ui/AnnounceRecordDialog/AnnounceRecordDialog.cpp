#include "AnnounceRecordDialog.h"
#include "../SmartboardConfirmDialog/SmartboardConfirmDialog.h"
#include "../../session/SessionManager.h"
#include "../../common/utils/Logger.h"
#include "../../common/ui/ThemeManager.h"
#include "../../audit/AuditLog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QCheckBox>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QStyle>
#include <QTemporaryFile>
#include <QDir>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QProcess>
#include <QToolButton>

QString AnnounceRecordDialog::friendlyDeviceName(const QAudioDevice& dev) const {
    QString desc = dev.description().trimmed();
    QString idStr = QString::fromUtf8(dev.id()).toLower();

    QString label;
    if (desc.contains("Digital", Qt::CaseInsensitive) || idStr.contains("dmic") || idStr.contains("acp")) {
        label = tr("Dahili Dijital Mikrofon");
    } else if (desc.contains("Headphone", Qt::CaseInsensitive) || desc.contains("Kulaklık", Qt::CaseInsensitive)) {
        label = tr("Kulaklık Mikrofonu");
    } else if (desc.contains("Internal", Qt::CaseInsensitive) || desc.contains("Dahili", Qt::CaseInsensitive)) {
        label = tr("Dahili Mikrofon");
    } else if (desc.contains("USB", Qt::CaseInsensitive)) {
        label = tr("USB Mikrofon");
    } else if (desc.contains("Bluetooth", Qt::CaseInsensitive)) {
        label = tr("Bluetooth Mikrofon");
    }

    if (!label.isEmpty()) {
        return QString("%1 (%2)").arg(label, desc);
    }
    return desc.isEmpty() ? tr("Ses Giriş Aygıtı") : desc;
}

void AnnounceRecordDialog::populateMicrophones() {
    if (!m_micCombo) return;

    QByteArray currentId = m_micCombo->currentData().toByteArray();
    m_micCombo->clear();

    const QList<QAudioDevice> inputs = QMediaDevices::audioInputs();
    int bestIndex = -1;

    for (int i = 0; i < inputs.size(); ++i) {
        const auto& dev = inputs[i];
        QString desc = friendlyDeviceName(dev);
        QString idStr = QString::fromUtf8(dev.id());
        m_micCombo->addItem(ThemeManager::icon(":/ui/volume.svg"), desc, dev.id());
        m_micCombo->setItemData(i, QString("%1\n[%2]").arg(dev.description(), idStr), Qt::ToolTipRole);

        if (!currentId.isEmpty() && dev.id() == currentId) {
            bestIndex = i;
        } else if (bestIndex < 0 && dev.isDefault()) {
            bestIndex = i;
        }
    }

    if (m_micCombo->count() == 0) {
        // Doğrudan aygıt listelenemediyse sistem varsayılanı fallback'i ekle
        QAudioDevice defDev = QMediaDevices::defaultAudioInput();
        if (!defDev.isNull()) {
            QString desc = friendlyDeviceName(defDev);
            m_micCombo->addItem(ThemeManager::icon(":/ui/volume.svg"),
                                tr("Sistem Varsayılanı: %1").arg(desc),
                                defDev.id());
            bestIndex = 0;
        } else {
            m_micCombo->addItem(ThemeManager::icon(":/ui/volume.svg"),
                                tr("Sistem Varsayılan Mikrofonu"),
                                QByteArray("default"));
            bestIndex = 0;
        }
    }

    if (bestIndex >= 0 && bestIndex < m_micCombo->count()) {
        m_micCombo->setCurrentIndex(bestIndex);
    } else if (m_micCombo->count() > 0) {
        m_micCombo->setCurrentIndex(0);
    }
}

AnnounceRecordDialog::AnnounceRecordDialog(SessionManager* session,
                                           const QStringList& targets,
                                           const QString& targetDescription,
                                           const QString& initialFilePath,
                                           QWidget* parent)
    : QDialog(parent)
    , m_session(session)
    , m_targets(targets)
    , m_targetDescription(targetDescription)
    , m_initialFilePath(initialFilePath)
{
    const bool isFileMode = !m_initialFilePath.isEmpty();
    setWindowTitle(isFileMode ? tr("Ses Yayını (Dosyadan)") : tr("Anons Kaydet"));
    setFixedSize(480, 360);
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    auto* lay = new QVBoxLayout(this);
    lay->setSpacing(8);
    lay->setContentsMargins(18, 12, 18, 12);

    // --- Hedef Kapsamı ve Cihaz Filtreleri Grubu ---
    m_targetGroup = new QGroupBox(tr("Yayın Hedefleri"), this);
    auto* targetGroupLay = new QVBoxLayout(m_targetGroup);
    targetGroupLay->setSpacing(6);
    targetGroupLay->setContentsMargins(12, 10, 12, 10);

    QString scopeText = m_targetDescription.trimmed().isEmpty() ? tr("Tüm İstemciler") : m_targetDescription.trimmed();
    m_targetScopeLbl = new QLabel(QString("<b>%1:</b> %2").arg(tr("Hedef Kapsamı"), scopeText), m_targetGroup);
    m_targetScopeLbl->setWordWrap(true);
    targetGroupLay->addWidget(m_targetScopeLbl);

    auto* chkLay = new QHBoxLayout();
    chkLay->setSpacing(16);

    m_chkTargetStudents = new QCheckBox(tr("Öğrenci Bilgisayarları"), m_targetGroup);
    m_chkTargetStudents->setIcon(ThemeManager::icon(":/ui/student.svg"));

    m_chkTargetSmartboards = new QCheckBox(tr("Akıllı Tahtalar"), m_targetGroup);
    m_chkTargetSmartboards->setIcon(ThemeManager::icon(":/ui/smartboard.svg"));
    m_chkTargetSmartboards->setToolTip(tr("Ders işlenen akıllı tahtaların bölünmemesi için varsayılan olarak kapalıdır."));

    // Akıllı varsayılan seçim kontrolü:
    int studentCount = 0;
    int boardCount = 0;
    for (const QString& id : m_targets) {
        int aType = 0;
        if (m_session) {
            aType = m_session->clientState(id).agentType;
        }
        if (aType == 3) {
            boardCount++;
        } else if (aType != 2) {
            studentCount++;
        }
    }

    if (boardCount > 0 && studentCount == 0) {
        m_chkTargetStudents->setChecked(false);
        m_chkTargetSmartboards->setChecked(true);
    } else {
        m_chkTargetStudents->setChecked(true);
        m_chkTargetSmartboards->setChecked(false);
    }

    chkLay->addWidget(m_chkTargetStudents);
    chkLay->addWidget(m_chkTargetSmartboards);
    chkLay->addStretch();
    targetGroupLay->addLayout(chkLay);

    lay->addWidget(m_targetGroup);

    // --- Durum Etiketi ---
    m_statusLbl = new QLabel(isFileMode ? tr("Seçilen Ses Dosyası: %1").arg(QFileInfo(m_initialFilePath).fileName())
                                        : tr("Mikrofona konuşmak için Kaydet'e basın."), this);
    m_statusLbl->setAlignment(Qt::AlignCenter);
    m_statusLbl->setWordWrap(true);
    m_statusLbl->setProperty("status", "info");
    lay->addWidget(m_statusLbl);

    // --- Mikrofon Seçimi (Yalnızca canlı anons kaydında görünür) ---
    m_micWidget = new QWidget(this);
    auto* micLay = new QHBoxLayout(m_micWidget);
    micLay->setContentsMargins(0, 0, 0, 0);

    m_micLbl = new QLabel(tr("Mikrofon:"), m_micWidget);
    m_micCombo = new QComboBox(m_micWidget);
    m_micCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_btnRefreshMics = new QToolButton(m_micWidget);
    m_btnRefreshMics->setIcon(ThemeManager::icon(":/ui/refresh.svg"));
    m_btnRefreshMics->setToolTip(tr("Mikrofon Listesini Yenile"));
    connect(m_btnRefreshMics, &QToolButton::clicked, this, [this]() {
        populateMicrophones();
        m_statusLbl->setText(tr("Mikrofon listesi güncellendi."));
    });

    m_mediaDevices = new QMediaDevices(this);
    connect(m_mediaDevices, &QMediaDevices::audioInputsChanged,
            this, &AnnounceRecordDialog::populateMicrophones);

    populateMicrophones();

    micLay->addWidget(m_micLbl);
    micLay->addWidget(m_micCombo, 1);
    micLay->addWidget(m_btnRefreshMics);
    lay->addWidget(m_micWidget);

    if (isFileMode) {
        m_micWidget->setVisible(false);
    }

    m_timeLbl = new QLabel("00:00", this);
    m_timeLbl->setAlignment(Qt::AlignCenter);
    QFont f = m_timeLbl->font();
    f.setPointSize(28);
    f.setBold(true);
    m_timeLbl->setFont(f);
    m_timeLbl->setProperty("status", "info");
    m_timeLbl->setVisible(false);
    lay->addWidget(m_timeLbl);

    lay->addStretch();

    auto* row1 = new QHBoxLayout();
    m_btnKaydet = new QPushButton(tr("Kaydet"),           this);
    m_btnKaydet->setIcon(ThemeManager::icon(":/ui/record.svg"));
    
    m_btnDurdur = new QPushButton(tr("Kaydı Durdur"),     this);
    m_btnDurdur->setIcon(ThemeManager::icon(":/ui/stop.svg"));
    
    m_btnDinle  = new QPushButton(tr("Dinle"),             this);
    m_btnDinle->setIcon(ThemeManager::icon(":/ui/play.svg"));
    
    m_btnDStop  = new QPushButton(tr("Dinlemeyi Durdur"), this);
    m_btnDStop->setIcon(ThemeManager::icon(":/ui/stop.svg"));
    
    row1->addWidget(m_btnKaydet);
    row1->addWidget(m_btnDurdur);
    row1->addWidget(m_btnDinle);
    row1->addWidget(m_btnDStop);
    lay->addLayout(row1);

    auto* row2 = new QHBoxLayout();
    m_btnTekrar  = new QPushButton(tr("Tekrar Kaydet"), this);
    m_btnYayinla = new QPushButton(tr("Yayınla"),       this);
    m_btnIptal   = new QPushButton(tr("İptal"),              this);
    m_repeatCombo = new QComboBox(this);
    m_repeatCombo->addItems({"1", "2", "3"});
    m_repeatCombo->setFixedWidth(42);
    m_repeatCombo->setToolTip(tr("Tekrar sayısı"));
    row2->addWidget(m_btnTekrar);
    row2->addWidget(m_btnYayinla);
    row2->addWidget(m_repeatCombo);
    row2->addStretch();
    row2->addWidget(m_btnIptal);
    lay->addLayout(row2);

    m_btnKaydet ->setProperty("class", "primary");
    m_btnDinle  ->setProperty("class", "primary");
    m_btnYayinla->setProperty("class", "primary");

    m_recordTimer = new QTimer(this);
    m_recordTimer->setInterval(1000);
    connect(m_recordTimer, &QTimer::timeout, this, &AnnounceRecordDialog::onRecordTick);

    connect(m_btnKaydet,  &QPushButton::clicked, this, &AnnounceRecordDialog::onKaydetClicked);
    connect(m_btnDurdur,  &QPushButton::clicked, this, &AnnounceRecordDialog::onDurdurClicked);
    connect(m_btnDinle,   &QPushButton::clicked, this, &AnnounceRecordDialog::onDinleClicked);
    connect(m_btnDStop,   &QPushButton::clicked, this, &AnnounceRecordDialog::onDinleDurdurClicked);
    connect(m_btnTekrar,  &QPushButton::clicked, this, &AnnounceRecordDialog::onTekrarKaydetClicked);
    connect(m_btnYayinla, &QPushButton::clicked, this, &AnnounceRecordDialog::onYayinlaClicked);
    connect(m_btnIptal,   &QPushButton::clicked, this, &QDialog::reject);

    initPlayer();

    if (isFileMode) {
        m_wavPath = m_initialFilePath;
        setState(State::Recorded);
    } else {
        QString lastAnnounce = QDir::tempPath() + "/okidia_last_announce.wav";
        Logger::info(QObject::tr("AnnounceRecordDialog: Son anons dosyası kontrol ediliyor -> ") + lastAnnounce);
        if (QFile::exists(lastAnnounce)) {
            QTemporaryFile tmp(QDir::tempPath() + "/okidia_anons_XXXXXX.wav");
            tmp.setAutoRemove(false);
            if (tmp.open()) {
                QString tempPath = tmp.fileName();
                tmp.close();
                QFile::remove(tempPath); // Hedef dosya zaten mevcut olduğunda QFile::copy hata verdiği için siliyoruz
                if (QFile::copy(lastAnnounce, tempPath)) {
                    m_wavPath = tempPath;
                    setState(State::Recorded);
                    m_statusLbl->setText(tr("Son ses kaydı otomatik yüklendi."));
                    Logger::info(QObject::tr("AnnounceRecordDialog: Son anons başarıyla yüklendi -> ") + tempPath);
                } else {
                    Logger::warning(QObject::tr("AnnounceRecordDialog: Son anons kopyalanamadı!"));
                    setState(State::Idle);
                }
            } else {
                Logger::warning(QObject::tr("AnnounceRecordDialog: Geçici dosya oluşturulamadı!"));
                setState(State::Idle);
            }
        } else {
            Logger::info(QObject::tr("AnnounceRecordDialog: Son anons dosyası bulunamadı, boş pencere açılıyor."));
            setState(State::Idle);
        }
    }
}

AnnounceRecordDialog::~AnnounceRecordDialog() {
    if (m_session && m_session->isRecording())
        m_session->stopRecording();
    cleanupTempFile();
}

void AnnounceRecordDialog::setState(State s) {
    m_state = s;
    m_elapsed = 0;
    m_timeLbl->setText("00:00");
    updateUI();
}

void AnnounceRecordDialog::updateUI() {
    m_btnKaydet  ->setVisible(false);
    m_btnDurdur  ->setVisible(false);
    m_btnDinle   ->setVisible(false);
    m_btnDStop   ->setVisible(false);
    m_btnTekrar  ->setVisible(false);
    m_btnYayinla ->setVisible(false);
    m_repeatCombo->setVisible(false);
    m_timeLbl    ->setVisible(false);

    if (m_micCombo) {
        m_micCombo->setEnabled(m_state == State::Idle || m_state == State::Recorded);
    }

    const bool isFileMode = !m_initialFilePath.isEmpty();

    switch (m_state) {
    case State::Idle:
        if (isFileMode) {
            m_statusLbl->setText(tr("Ses dosyası hazır değil."));
        } else {
            m_statusLbl->setText(tr("Mikrofona konuşmak için Kaydet'e basın."));
            m_btnKaydet->setVisible(true);
        }
        break;
    case State::Recording:
        m_btnDurdur->setEnabled(true);
        m_statusLbl->setText(tr("Kayıt yapılıyor..."));
        m_btnDurdur ->setVisible(true);
        m_timeLbl   ->setVisible(true);
        m_timeLbl->setProperty("status", "error");
        m_timeLbl->style()->unpolish(m_timeLbl);
        m_timeLbl->style()->polish(m_timeLbl);
        break;
    case State::Recorded:
        if (isFileMode) {
            m_statusLbl->setText(tr("Ses dosyası hazır: %1").arg(QFileInfo(m_initialFilePath).fileName()));
        } else {
            m_statusLbl->setText(tr("Kayıt tamamlandı."));
            m_btnTekrar->setVisible(true);
        }
        m_btnDinle  ->setVisible(true);
        m_btnYayinla->setVisible(true);
        m_repeatCombo->setVisible(true);
        break;
    case State::Playing:
        m_statusLbl->setText(tr("Dinleniyor..."));
        m_btnDStop  ->setVisible(true);
        m_timeLbl   ->setVisible(true);
        m_timeLbl->setProperty("status", "success");
        m_timeLbl->style()->unpolish(m_timeLbl);
        m_timeLbl->style()->polish(m_timeLbl);
        break;
    }
}

void AnnounceRecordDialog::onKaydetClicked() {
    if (!m_session) return;
    cleanupTempFile();

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

    if (!m_session->startRecording(selectedDev)) {
        m_statusLbl->setText(tr("Mikrofon başlatılamadı."));
        return;
    }
    setState(State::Recording);
    m_recordTimer->start();
}

void AnnounceRecordDialog::onDurdurClicked() {
    m_recordTimer->stop();
    if (!m_session) return;
    m_statusLbl->setText(tr("Tamamlanıyor..."));
    m_btnDurdur->setEnabled(false);
    m_wavPath = m_session->stopRecording();
    if (m_wavPath.isEmpty() || !QFile::exists(m_wavPath)
            || QFileInfo(m_wavPath).size() <= 44) {
        m_statusLbl->setText(tr("Kayıt başarısız."));
        setState(State::Idle);
        return;
    }
    setState(State::Recorded);
}

void AnnounceRecordDialog::onDinleClicked() {
    if (m_wavPath.isEmpty() || !QFile::exists(m_wavPath)) {
        m_statusLbl->setText(tr("Oynatılacak ses dosyası bulunamadı veya boş."));
        return;
    }
    if (m_initialFilePath.isEmpty() && QFileInfo(m_wavPath).size() <= 44) {
        m_statusLbl->setText(tr("Oynatılacak ses dosyası boş."));
        return;
    }
    initPlayer();
    m_player->setSource(QUrl::fromLocalFile(m_wavPath));
    m_player->play();
    setState(State::Playing);
}

void AnnounceRecordDialog::onDinleDurdurClicked() {
    m_recordTimer->stop();
    if (m_player) m_player->stop();
    setState(State::Recorded);
}

void AnnounceRecordDialog::onTekrarKaydetClicked() {
    if (m_player) m_player->stop();
    m_recordTimer->stop();
    cleanupTempFile();
    setState(State::Idle);
}

void AnnounceRecordDialog::onYayinlaClicked() {
    if (!m_session || m_wavPath.isEmpty() || !QFile::exists(m_wavPath)) {
        m_statusLbl->setText(tr("Yayınlanacak geçerli bir ses dosyası yok."));
        return;
    }
    if (m_initialFilePath.isEmpty() && QFileInfo(m_wavPath).size() <= 44) {
        m_statusLbl->setText(tr("Yayınlanacak ses kaydı boş."));
        return;
    }
    if (m_player) m_player->stop();
    m_recordTimer->stop();

    const bool targetStudents = m_chkTargetStudents ? m_chkTargetStudents->isChecked() : true;
    const bool targetSmartboards = m_chkTargetSmartboards ? m_chkTargetSmartboards->isChecked() : false;

    if (!targetStudents && !targetSmartboards) {
        m_statusLbl->setText(tr("Lütfen en az bir hedef aygıt türü (Öğrenci veya Akıllı Tahta) seçin."));
        return;
    }

    QStringList finalTargets;
    QMap<QString, QString> targetedSmartboards; // clientId -> display name

    for (const QString& cid : m_targets) {
        int aType = 0;
        QString name = cid;
        if (m_session) {
            const ClientState cs = m_session->clientState(cid);
            aType = cs.agentType;
            name = cs.hostname.isEmpty() ? cid : cs.hostname;
            if (!cs.username.isEmpty()) name += " (" + cs.username + ")";
        }
        if (aType == 3) {
            if (targetSmartboards) {
                targetedSmartboards.insert(cid, name);
                finalTargets << cid;
            }
        } else if (aType == 2) {
            // İdari PC anonstan tamamen muaf
            continue;
        } else {
            // Öğrenci veya standart PC
            if (targetStudents) {
                finalTargets << cid;
            }
        }
    }

    // Eğer Akıllı Tahta hedeflenmişse ve tahta varsa, öğretmeni uyar ve teyit al
    if (targetSmartboards && !targetedSmartboards.isEmpty()) {
        SmartboardConfirmDialog confirmDlg(targetedSmartboards, this);
        if (confirmDlg.exec() != QDialog::Accepted) {
            return;
        }
        QStringList confirmedBoards = confirmDlg.selectedClientIds();
        for (const QString& boardId : targetedSmartboards.keys()) {
            if (!confirmedBoards.contains(boardId)) {
                finalTargets.removeAll(boardId);
            }
        }
    }

    if (finalTargets.isEmpty()) {
        m_statusLbl->setText(tr("Seçilen hedef kriterlerine uygun çevrimiçi istemci bulunamadı."));
        return;
    }

    const int repeat = m_repeatCombo ? m_repeatCombo->currentText().toInt() : 1;
    m_session->sendAnons(m_wavPath, finalTargets, repeat);

    QString auditDesc = m_initialFilePath.isEmpty()
        ? tr("Sesli anons yayınlandı (Tekrar: %1)").arg(repeat)
        : tr("Ses dosyası yayınlandı: %1 (Tekrar: %2)").arg(QFileInfo(m_wavPath).fileName()).arg(repeat);
    AuditLog::instance().recordFile({}, finalTargets.join(", "), auditDesc);

    // Son ses kaydını /tmp/okidia_last_announce.wav olarak yedekle (yalnızca canlı mikrofon kaydıysa)
    if (m_initialFilePath.isEmpty()) {
        QString lastAnnounce = QDir::tempPath() + "/okidia_last_announce.wav";
        QFile::remove(lastAnnounce);
        if (QFile::copy(m_wavPath, lastAnnounce)) {
            Logger::info(QObject::tr("AnnounceRecordDialog: Son anons önbelleğe kaydedildi -> ") + lastAnnounce);
        } else {
            Logger::warning(QObject::tr("AnnounceRecordDialog: Son anons önbelleğe kopyalanamadı!"));
        }
    }

    m_targets = finalTargets;
    m_published = true;
    accept();
}

void AnnounceRecordDialog::onRecordTick() {
    ++m_elapsed;
    m_timeLbl->setText(QString("%1:%2")
        .arg(m_elapsed / 60, 2, 10, QChar('0'))
        .arg(m_elapsed % 60, 2, 10, QChar('0')));
}

void AnnounceRecordDialog::cleanupTempFile() {
    if (!m_wavPath.isEmpty()) {
        if (m_initialFilePath.isEmpty() || m_wavPath != m_initialFilePath) {
            QFile::remove(m_wavPath);
        }
        m_wavPath.clear();
    }
}

void AnnounceRecordDialog::initPlayer() {
    if (m_player) return;
    m_player = new QMediaPlayer(this);
    m_audio  = new QAudioOutput(this);
    m_audio->setDevice(QMediaDevices::defaultAudioOutput());
    m_audio->setVolume(1.0f);
    m_audio->setMuted(false);
    m_player->setAudioOutput(m_audio);

    connect(m_player, &QMediaPlayer::playbackStateChanged,
            this, [this](QMediaPlayer::PlaybackState s) {
        if (s == QMediaPlayer::StoppedState && m_state == State::Playing) {
            m_recordTimer->stop();
            setState(State::Recorded);
        }
    });

    connect(m_player, &QMediaPlayer::positionChanged, this, [this](qint64 posMs) {
        if (m_state == State::Playing) {
            int secs = static_cast<int>(posMs / 1000);
            m_timeLbl->setText(QString("%1:%2")
                .arg(secs / 60, 2, 10, QChar('0'))
                .arg(secs % 60, 2, 10, QChar('0')));
        }
    });

    connect(m_player, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString& errStr) {
        Logger::error(QObject::tr("AnnounceRecordDialog: Oynatma hatası: ") + errStr);
        m_statusLbl->setText(tr("Oynatma hatası: ") + errStr);
        m_recordTimer->stop();
        setState(State::Recorded);
    });
}
