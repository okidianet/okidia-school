// master/ui/Widgets/MasterLockOverlay.cpp
#include "MasterLockOverlay.h"
#include "../../session/SessionManager.h"
#include "../../../common/ui/ThemeManager.h"
#include "../../../common/utils/OkidiaUtils.h"
#include "../../../common/db/OkidiaDatabase.h"
#include "../../audit/AuditLog.h"
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QGraphicsDropShadowEffect>

MasterLockOverlay::MasterLockOverlay(SessionManager* session, QWidget* parent)
    : QWidget(parent)
    , m_session(session) {
    
    // Tüm alanı kapla ve olayları yakala
    setAttribute(Qt::WA_NoSystemBackground, false);
    setAttribute(Qt::WA_TransparentForMouseEvents, false);
    setFocusPolicy(Qt::StrongFocus);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setAlignment(Qt::AlignCenter);

    m_cardFrame = new QFrame(this);
    m_cardFrame->setObjectName("lockCardFrame");
    m_cardFrame->setFixedWidth(380);
    m_cardFrame->setStyleSheet(
        "QFrame#lockCardFrame {"
        "  background-color: palette(window);"
        "  border: 1px solid palette(mid);"
        "  border-radius: 12px;"
        "  padding: 24px;"
        "}"
    );

    auto* shadow = new QGraphicsDropShadowEffect(this);
    shadow->setBlurRadius(30);
    shadow->setColor(QColor(0, 0, 0, 160));
    shadow->setOffset(0, 8);
    m_cardFrame->setGraphicsEffect(shadow);

    auto* cardLayout = new QVBoxLayout(m_cardFrame);
    cardLayout->setSpacing(14);
    cardLayout->setContentsMargins(16, 16, 16, 16);

    // 1. İkon
    m_iconLabel = new QLabel(m_cardFrame);
    m_iconLabel->setAlignment(Qt::AlignCenter);
    m_iconLabel->setPixmap(ThemeManager::icon(":/ui/lock.svg").pixmap(56, 56));
    cardLayout->addWidget(m_iconLabel);

    // 2. Başlık
    m_titleLabel = new QLabel(tr("Oturum Kilitlendi"), m_cardFrame);
    m_titleLabel->setAlignment(Qt::AlignCenter);
    QFont titleFont = m_titleLabel->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    m_titleLabel->setFont(titleFont);
    cardLayout->addWidget(m_titleLabel);

    // 3. Kullanıcı Bilgisi
    m_userLabel = new QLabel(m_cardFrame);
    m_userLabel->setAlignment(Qt::AlignCenter);
    m_userLabel->setStyleSheet("color: palette(text); font-size: 13px; font-weight: 500;");
    cardLayout->addWidget(m_userLabel);

    // 4. Parola Girişi
    m_passwordEdit = new QLineEdit(m_cardFrame);
    m_passwordEdit->setEchoMode(QLineEdit::Password);
    m_passwordEdit->setPlaceholderText(tr("Parolanızı girin"));
    m_passwordEdit->setMinimumHeight(38);
    m_passwordEdit->setStyleSheet("font-size: 14px; padding: 4px 10px; border-radius: 6px;");
    connect(m_passwordEdit, &QLineEdit::returnPressed, this, &MasterLockOverlay::onUnlockClicked);
    cardLayout->addWidget(m_passwordEdit);

    // 5. Hata Etiketi
    m_errorLabel = new QLabel(m_cardFrame);
    m_errorLabel->setAlignment(Qt::AlignCenter);
    m_errorLabel->setWordWrap(true);
    m_errorLabel->setStyleSheet("color: #ef4444; font-size: 12px; font-weight: bold;");
    m_errorLabel->hide();
    cardLayout->addWidget(m_errorLabel);

    // 6. Butonlar
    m_unlockButton = new QPushButton(tr("Kilidi Aç"), m_cardFrame);
    m_unlockButton->setMinimumHeight(38);
    m_unlockButton->setProperty("class", "primary");
    m_unlockButton->setStyleSheet("font-size: 14px; font-weight: bold;");
    connect(m_unlockButton, &QPushButton::clicked, this, &MasterLockOverlay::onUnlockClicked);
    cardLayout->addWidget(m_unlockButton);

    m_logoutButton = new QPushButton(tr("Farklı Kullanıcıyla Giriş Yap"), m_cardFrame);
    m_logoutButton->setMinimumHeight(32);
    m_logoutButton->setFlat(true);
    m_logoutButton->setStyleSheet("color: palette(placeholderText); font-size: 12px; text-decoration: underline;");
    connect(m_logoutButton, &QPushButton::clicked, this, &MasterLockOverlay::onLogoutClicked);
    cardLayout->addWidget(m_logoutButton);

    rootLayout->addWidget(m_cardFrame);
}

void MasterLockOverlay::setUser(const UserRecord& user) {
    m_user = user;
    QString disp = m_user.displayName.isEmpty() ? m_user.username : m_user.displayName;
    if (!m_user.role.isEmpty()) {
        disp += QString(" (%1)").arg(m_user.role);
    }
    m_userLabel->setText(disp);
}

void MasterLockOverlay::resetState() {
    m_passwordEdit->clear();
    m_errorLabel->clear();
    m_errorLabel->hide();
    m_passwordEdit->setFocus();
}

void MasterLockOverlay::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    // Yarı saydam koyu arka plan overlay
    QColor overlayColor = (ThemeManager::instance().currentMode() == ThemeManager::ThemeMode::Dark)
                          ? QColor(10, 15, 25, 215)
                          : QColor(240, 243, 246, 215);
    painter.fillRect(rect(), overlayColor);
}

void MasterLockOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        event->accept();
        return; // Kilit ekranından Escape ile çıkılamaz
    }
    QWidget::keyPressEvent(event);
}

void MasterLockOverlay::mousePressEvent(QMouseEvent* event) {
    event->accept(); // Tıklamaların arkadaki arayüze geçmesini engelle
}

void MasterLockOverlay::onUnlockClicked() {
    const QString password = m_passwordEdit->text();
    if (password.isEmpty()) {
        m_errorLabel->setText(tr("Lütfen parolanızı girin."));
        m_errorLabel->show();
        return;
    }

    const QString hash = OkidiaUtils::sha256(password.toUtf8());

    // 1. Bellek üzerinden aktif kimlik hash kontrolü (En hızlı ve güvenilir)
    bool isMatch = false;
    if (m_session) {
        if (!m_session->activeUser().passwordHash.isEmpty() && m_session->activeUser().passwordHash == hash) {
            isMatch = true;
        } else if (!m_session->passwordHash().isEmpty() && m_session->passwordHash() == hash) {
            isMatch = true;
        }
    }
    if (!isMatch && !m_user.passwordHash.isEmpty() && m_user.passwordHash == hash) {
        isMatch = true;
    }

    // 2. Bellekte bulunamazsa yerel veritabanı doğrulaması
    if (!isMatch && OkidiaDatabase::instance().isOpen()) {
        UserRecord dbUser = OkidiaDatabase::instance().userByUsername(m_user.username);
        if (dbUser.id.isEmpty() && !m_user.id.isEmpty()) {
            dbUser = OkidiaDatabase::instance().userById(m_user.id);
        }
        if (!dbUser.id.isEmpty() && dbUser.passwordHash == hash) {
            isMatch = true;
        }
    }

    if (isMatch) {
        m_errorLabel->hide();
        m_passwordEdit->clear();
        AuditLog::instance().recordAuth(m_user.username, tr("Master kilit ekranı açıldı"));
        emit unlocked();
    } else {
        // 3. Eğer yerel olarak doğrulanamadıysa, sunucuya sor (şifre değişmiş olabilir)
        if (m_session && (m_session->isConnected() || m_session->isConnecting())) {
            m_waitingBroker = true;
            m_errorLabel->setText(tr("Sunucuda doğrulanıyor..."));
            m_errorLabel->setStyleSheet("color: palette(text); font-size: 12px; font-weight: normal;");
            m_errorLabel->show();
            m_passwordEdit->setEnabled(false);
            m_unlockButton->setEnabled(false);
            
            // Eğer daha önce bağlanılmamışsa, loginResult sinyaline bağlan
            connect(m_session, &SessionManager::loginResult, this, &MasterLockOverlay::onLoginResult, Qt::UniqueConnection);
            m_session->requestLogin(m_user.username, hash);
        } else {
            m_errorLabel->setText(tr("Hatalı parola! Lütfen tekrar deneyin."));
            m_errorLabel->setStyleSheet("color: #ef4444; font-size: 12px; font-weight: bold;");
            m_errorLabel->show();
            m_passwordEdit->selectAll();
            m_passwordEdit->setFocus();
            AuditLog::instance().recordAuth(m_user.username, tr("Master kilit ekranında hatalı parola denemesi"));
        }
    }
}

void MasterLockOverlay::onLoginResult(bool success, const QString& userId, const QString& role, const QString& displayName, const QString& errorMsg, bool mustChangePw) {
    if (!m_waitingBroker) return;
    m_waitingBroker = false;
    
    m_passwordEdit->setEnabled(true);
    m_unlockButton->setEnabled(true);
    
    if (success) {
        const QString enteredPw = m_passwordEdit->text();
        const QString hash = OkidiaUtils::sha256(enteredPw.toUtf8());
        m_errorLabel->hide();
        m_passwordEdit->clear();
        AuditLog::instance().recordAuth(m_user.username, tr("Master kilit ekranı açıldı (Sunucu onayıyla)"));
        
        if (m_session && !enteredPw.isEmpty()) {
            m_session->setPendingCredentials(m_user.username, hash);
            m_session->updateCachedPassword(hash);
        }
        emit unlocked();
    } else {
        m_errorLabel->setText(tr("Hatalı parola veya sunucu hatası!"));
        m_errorLabel->setStyleSheet("color: #ef4444; font-size: 12px; font-weight: bold;");
        m_passwordEdit->selectAll();
        m_passwordEdit->setFocus();
        AuditLog::instance().recordAuth(m_user.username, tr("Master kilit ekranında hatalı parola (Sunucu reddi)"));
    }
}

void MasterLockOverlay::onLogoutClicked() {
    emit logoutRequested();
}
