// master/ui/AnnounceRecordDialog/AnnounceRecordDialog.h
#pragma once

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QComboBox>
#include <QCheckBox>
#include <QGroupBox>
#include <QTimer>
#include <QProgressBar>
#include <QMediaPlayer>
#include <QAudioOutput>
#include <QStringList>

class SessionManager;

/**
 * Anons & Ses Yayın Dialog'u.
 * Canlı mikrofon kaydı veya hazır ses dosyasını (wav/mp3 vb.)
 * seçili hedef kapsamına ve aygıt türlerine (Öğrenci PC / Akıllı Tahta) yayınlar.
 */
class AnnounceRecordDialog : public QDialog {
    Q_OBJECT

public:
    explicit AnnounceRecordDialog(SessionManager* session,
                                  const QStringList& targets,
                                  const QString& targetDescription = QString(),
                                  const QString& initialFilePath = QString(),
                                  QWidget* parent = nullptr);
    ~AnnounceRecordDialog();

    bool wasPublished() const { return m_published; }
    const QStringList& targets() const { return m_targets; }
    QString wavPath() const { return m_wavPath; }

private slots:
    void onKaydetClicked();
    void onDurdurClicked();
    void onDinleClicked();
    void onDinleDurdurClicked();
    void onTekrarKaydetClicked();
    void onYayinlaClicked();
    void onRecordTick();

private:
    enum class State { Idle, Recording, Recorded, Playing };
    void setState(State s);
    void updateUI();
    void cleanupTempFile();
    void initPlayer();
    void populateMicrophones();
    QString friendlyDeviceName(const class QAudioDevice& dev) const;

    SessionManager* m_session  = nullptr;
    QStringList     m_targets;
    QString         m_targetDescription;
    QString         m_initialFilePath;
    QString         m_wavPath;
    bool            m_published = false;
    State           m_state     = State::Idle;
    int             m_elapsed   = 0;

    // Hedef Kapsamı ve Cihaz Filtreleri
    QGroupBox*    m_targetGroup        = nullptr;
    QLabel*       m_targetScopeLbl     = nullptr;
    QCheckBox*    m_chkTargetStudents   = nullptr;
    QCheckBox*    m_chkTargetSmartboards = nullptr;

    // Mikrofon ve Durum
    QWidget*      m_micWidget          = nullptr;
    QLabel*       m_statusLbl          = nullptr;
    QLabel*       m_micLbl             = nullptr;
    QComboBox*    m_micCombo           = nullptr;
    class QToolButton* m_btnRefreshMics = nullptr;
    class QMediaDevices* m_mediaDevices = nullptr;
    QLabel*       m_timeLbl            = nullptr;
    QProgressBar* m_levelBar           = nullptr;

    // Butonlar
    QPushButton* m_btnKaydet  = nullptr;
    QPushButton* m_btnDurdur  = nullptr;
    QPushButton* m_btnDinle   = nullptr;
    QPushButton* m_btnDStop   = nullptr;
    QPushButton* m_btnTekrar  = nullptr;
    QPushButton* m_btnYayinla = nullptr;
    QPushButton* m_btnIptal   = nullptr;
    QComboBox*   m_repeatCombo = nullptr;

    QTimer*       m_recordTimer = nullptr;
    QMediaPlayer* m_player      = nullptr;
    QAudioOutput* m_audio       = nullptr;
    class QAudioSink* m_previewSink = nullptr;
    class QBuffer*    m_previewBuf  = nullptr;
};
