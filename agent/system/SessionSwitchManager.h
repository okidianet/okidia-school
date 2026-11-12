// agent/system/SessionSwitchManager.h
#pragma once

#include <QObject>
#include <QString>

class SessionSwitchManager : public QObject {
    Q_OBJECT

public:
    static SessionSwitchManager& instance();

    // Linux üzerinde yerel sistem kullanıcısının varlığını kontrol et, yoksa oluştur
    bool ensureSystemUser(const QString& username, const QString& displayName = QString(),
                          const QString& password = QString());

    // Öğretmen oturumuna geçiş yap (Display Manager / loginctl)
    bool switchToUserSession(const QString& username);

    // Açık olan öğretmen oturumunu askıya al/kilitle (Oturum kapatılmaz, askıda tutulur)
    bool terminateCurrentSession();

    // Farklı bir öğretmen giriş yaptığında önceki kullanıcının oturumunu zorla sonlandır
    bool forceLogoutUser(const QString& username);

    // Aktif öğretmen kullanıcı adı
    QString activeUser() const { return m_activeUser; }
    void setActiveUser(const QString& user) { m_activeUser = user; }

    // Geçici LightDM autologin dosyasını temizler (sistem yeniden başladığında kalıcı autologin'i engeller)
    void cleanupAutologinConfig();

    // Sistemde o an açık olan aktif GUI (X11/Display Manager) kullanıcısını tespit eder
    QString getActiveGuiUser() const;

private:
    explicit SessionSwitchManager(QObject* parent = nullptr);
    ~SessionSwitchManager() override = default;

    SessionSwitchManager(const SessionSwitchManager&) = delete;
    SessionSwitchManager& operator=(const SessionSwitchManager&) = delete;

    QString m_activeUser;
};
