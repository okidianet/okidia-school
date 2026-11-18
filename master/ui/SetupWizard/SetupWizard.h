// master/ui/SetupWizard/SetupWizard.h
// İlk kurulum sihirbazı — broker keşfi, kurulum kodu, YM hesabı, sertifika alma
#pragma once

#include <QWizard>
#include <QWizardPage>
#include <QListWidget>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QTimer>
#include <QSslSocket>

#include "../../../common/network/MdnsManager.h"
#include "../../../common/network/TlsClient.h"

// ─────────────────────────────────────────────────────────────────────────────
// Bulunan broker bilgisi
struct BrokerEntry {
    QString name;
    QString host;
    quint16 port  = 0;
    QString role;
    QStringList allHosts;
};

// ─────────────────────────────────────────────────────────────────────────────
// Sayfa 1: Hoşgeldiniz
class WelcomePage : public QWizardPage {
    Q_OBJECT
public:
    explicit WelcomePage(QWidget* parent = nullptr);
};

// ─────────────────────────────────────────────────────────────────────────────
// Mod Seçim Sayfası (YM, OM veya Restore)
class ChoicePage : public QWizardPage {
    Q_OBJECT
    Q_PROPERTY(int modeValue READ modeValue)
public:
    explicit ChoicePage(QWidget* parent = nullptr);
    int nextId() const override;
    int modeValue() const;
private:
    QButtonGroup* m_group;
};

// ─────────────────────────────────────────────────────────────────────────────
// Sayfa 2: Broker keşfi
class BrokerDiscoveryPage : public QWizardPage {
    Q_OBJECT
public:
    explicit BrokerDiscoveryPage(QWidget* parent = nullptr);
    ~BrokerDiscoveryPage();

    void initializePage() override;
    bool isComplete() const override;
    int nextId() const override;

    BrokerEntry selectedBroker() const;

private slots:
    void onServiceFound(const QString& name,
                        const QString& host,
                        quint16 port,
                        const QString& role);
    void onServiceLost(const QString& name);
    void onScanTimeout();
    void onSelectionChanged();
    void onManualAdd();

private:
    QLabel*       m_statusLabel;
    QListWidget*  m_list;
    QPushButton*  m_rescanBtn;
    QPushButton*  m_manualAddBtn;
    QProgressBar* m_progress;
    MdnsManager*  m_mdns;
    QTimer*       m_scanTimer;
    QList<BrokerEntry> m_brokers;

    void startScan();
    void addBroker(const BrokerEntry& b);
    QString roleLabel(const QString& role) const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Sayfa 3: Kurulum kodu
class SetupCodePage : public QWizardPage {
    Q_OBJECT
public:
    explicit SetupCodePage(QWidget* parent = nullptr);
    bool isComplete() const override;
    bool validatePage() override;
    int nextId() const override;

private slots:
    void onCodeChanged(const QString& text);

private:
    QLineEdit* m_codeEdit;
    QLabel*    m_hintLabel;
};

// OM Katılım Kodu Sayfası
class JoinCodePage : public QWizardPage {
    Q_OBJECT
public:
    explicit JoinCodePage(QWidget* parent = nullptr);
    bool isComplete() const override;
    int nextId() const override;
    QString password() const;
private slots:
    void onCodeChanged(const QString& text);
    void onPasswordChanged();
private:
    QLineEdit* m_codeEdit;
    QLabel*    m_hintLabel;
    QLineEdit* m_passwordEdit;
    QLineEdit* m_password2Edit;
    QLabel*    m_pwErrorLabel;
};

// ─────────────────────────────────────────────────────────────────────────────
// Sayfa 4: YM hesabı oluştur
class AdminAccountPage : public QWizardPage {
    Q_OBJECT
public:
    explicit AdminAccountPage(QWidget* parent = nullptr);
    bool validatePage() override;
    bool isComplete() const override;
    int nextId() const override;

private slots:
    void onFieldChanged();

private:
    QLineEdit* m_schoolNameEdit;
    QLineEdit* m_usernameEdit;
    QLineEdit* m_displayNameEdit;
    QLineEdit* m_passwordEdit;
    QLineEdit* m_password2Edit;
    QLabel*    m_errorLabel;
};

// ─────────────────────────────────────────────────────────────────────────────
// Yeni Sayfa: Kurtarma Kodu
class RecoveryKeyPage : public QWizardPage {
    Q_OBJECT
    Q_PROPERTY(QString recoveryKey READ recoveryKey)
public:
    explicit RecoveryKeyPage(QWidget* parent = nullptr);
    void initializePage() override;
    int nextId() const override;
    QString recoveryKey() const { return m_key; }
private slots:
    void onSaveClicked();
private:
    QLabel*      m_keyLabel;
    QPushButton* m_saveBtn;
    QString      m_key;
};

// ─────────────────────────────────────────────────────────────────────────────
// Yeni Sayfa: RestoreSelectPage (Yedek Seçimi)
class RestoreSelectPage : public QWizardPage {
    Q_OBJECT
public:
    explicit RestoreSelectPage(QWidget* parent = nullptr);
    bool isComplete() const override;
    bool validatePage() override;
    int nextId() const override;
    QString backupPath() const;
    QString backupPassword() const;

private slots:
    void onBrowse();
    void onFieldChanged();

private:
    QLineEdit* m_pathEdit;
    QLineEdit* m_passwordEdit;
    QLabel*    m_errorLabel;
};

// ─────────────────────────────────────────────────────────────────────────────
// Sayfa 5: Sertifika alma + tamamlanma
class FinishPage : public QWizardPage {
    Q_OBJECT
public:
    explicit FinishPage(QWidget* parent = nullptr);

    void initializePage() override;
    bool isComplete() const override;
    bool hasError() const { return m_failed; }

signals:
    void setupComplete(const QString& masterCertPath,
                       const QString& masterKeyPath,
                       const QString& caCertPath);

private slots:
    void onBrokerConnected(Connection* conn);
    void onBrokerDisconnected();
    void onMessageReceived(const okidia::Envelope& env);

private:
    QLabel*       m_statusLabel;
    QProgressBar* m_progress;
    QPushButton*  m_retryBtn = nullptr;
    TlsClient*    m_tls;
    bool          m_done   = false;
    bool          m_failed = false;

    QStringList m_candidateHosts;
    quint16     m_candidatePort = 0;
    int         m_currentHostIdx = 0;
    QString     m_connectedHost;
    QString     m_generatedKeyPath;
    
    void connectToBroker();
    void tryNextHost();
    void sendSetupRequest(Connection* conn);
    void sendJoinRequest(Connection* conn);
    void sendRestoreRequest(Connection* conn);
    void completeSetup(const QString& userId = "", const QString& networkKey = "");
    bool saveCerts(const QString& certPem, const QString& caPem, const QString& userId = "", const QString& networkKey = "");
    void setStatus(const QString& msg, bool error = false);
};

// ─────────────────────────────────────────────────────────────────────────────
// Ana sihirbaz
class SetupWizard : public QWizard {
    Q_OBJECT
public:
    explicit SetupWizard(QWidget* parent = nullptr);
    void reject() override;

    // Kurulum tamamlandıktan sonra sertifika yolları
    QString masterCertPath() const { return m_masterCertPath; }
    QString masterKeyPath()  const { return m_masterKeyPath;  }
    QString caCertPath()     const { return m_caCertPath;     }

    // Sayfa ID'leri
    enum Pages {
        Page_Welcome = 0,
        Page_Choice,
        Page_Discovery,
        Page_Code,      // YM için Setup Code
        Page_JoinCode,  // OM için Join Code
        Page_Account,
        Page_RecoveryKey,
        Page_RestoreSelect,
        Page_Finish
    };

    enum SetupMode {
        Mode_NewYM = 0,
        Mode_JoinOM,
        Mode_RestoreIdentity
    };

private slots:
    void onSetupComplete(const QString& certPath,
                         const QString& keyPath,
                         const QString& caPath);
    void onPageChanged(int id);

private:
    QString m_masterCertPath;
    QString m_masterKeyPath;
    QString m_caCertPath;
};

// ─────────────────────────────────────────────────────────────────────────────
// Kurtarma Sihirbazı (Restore için Discovery + Setup Code)
class RecoveryWizard : public QWizard {
    Q_OBJECT
public:
    explicit RecoveryWizard(QWidget* parent = nullptr);

    enum Pages {
        Page_Discovery = 0,
        Page_Code
    };

    BrokerEntry selectedBroker() const;
    QString setupCode() const;
};
