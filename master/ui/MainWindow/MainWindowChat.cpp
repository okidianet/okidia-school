// master/ui/MainWindow/MainWindowChat.cpp
#include "MainWindow.h"
#include "ui_MainWindow.h"
#include "session/SessionManager.h"
#include "chat/RoomManager.h"
#include "chat/PrivateChatManager.h"
#include "ui/ChatWindow/ChatWindow.h"
#include "../../../common/ui/ThemeManager.h"
#include <QMessageBox>
#include "../../common/utils/Logger.h"

// ============================================================================
// Faz 5 — Sohbet / El kaldırma slot'ları
// ============================================================================

void MainWindow::onChatMessageReceived(const QString& clientId,
                                        const QString& roomId,
                                        const QString& from,
                                        const QString& content,
                                        qint64         timestamp,
                                        int            senderRole,
                                        bool           isSystem) {
    Q_UNUSED(clientId);
    const QString targetRoom = roomId.isEmpty() ? RoomManager::kAdminRoomId : roomId;
    const QString fromHost = from.contains('@') ? from.section('@', 1) : from;
    const QString fromDisplay = from.contains('@') ? from.section('@', 0, 0) : from;

    m_roomMgr->onIncomingMessage(targetRoom, fromHost, fromDisplay,
                                  senderRole, content, timestamp, isSystem);
}

void MainWindow::onHandRaiseReceived(const QString& clientId,
                                      const QString& message) {
    const ClientState s = m_session ? m_session->clientState(clientId) : ClientState{};

    // Yoksayılan kullanıcıdan gelen yardım taleplerini sessizce engelle
    if (m_privateChatMgr && m_privateChatMgr->isUserIgnored(clientId, s.username)) {
        return;
    }

    if (m_session) m_session->setClientHandRaised(clientId, true, message);
    const QString name = RoomManager::formatStudentDisplayName(s.userDisplayName, s.username, s.hostname, clientId);
    const QString label = tr("Yardım Talebi: ") + name
                          + (message.isEmpty() ? "" : " — \"" + message + "\"");
    showCommandStatus(true, label);

    if (isScreenLocked()) {
        return;
    }

    // Zaten açık özel sohbet varsa tekrar onay penceresi açma; notu doğrudan oturuma ekle
    if (m_privateChatMgr) {
        const QString existingSessionId = m_privateChatMgr->sessionIdForPeer(clientId);
        if (!existingSessionId.isEmpty()) {
            const QString noteText = message.isEmpty()
                ? tr("Öğrenci tekrar yardım talebinde bulundu.")
                : tr("Öğrencinin Yardım Notu: \"%1\"").arg(message);
            m_privateChatMgr->appendSystemMessage(existingSessionId, noteText);
            if (m_chatWindow) {
                m_chatWindow->switchToTab(existingSessionId);
                m_chatWindow->showAndRaise();
            }
            return;
        }
    }

    if (!m_chatWindow || !m_chatWindow->isVisible()) {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("Öğrenci Yardım Talebi"));
        QString text = QString(tr("<b>%1</b> yardım talebinde bulundu!")).arg(name.toHtmlEscaped());
        if (!message.isEmpty()) {
            text += QString("<br><i>%1: \"%2\"</i>").arg(tr("Mesaj"), message.toHtmlEscaped());
        }
        text += tr("<br><br>Bu öğrenci ile özel sohbet başlatmak ister misiniz?");
        msgBox.setText(text);
        msgBox.setIcon(QMessageBox::Information);
        QPushButton* btnChat = msgBox.addButton(tr("Özel Sohbet Başlat"), QMessageBox::AcceptRole);
        QPushButton* btnOpenChat = msgBox.addButton(tr("Sohbet Penceresini Aç"), QMessageBox::ActionRole);
        QPushButton* btnIgnore = msgBox.addButton(tr("Yoksay (Engelle)"), QMessageBox::DestructiveRole);
        msgBox.addButton(tr("Kapat"), QMessageBox::RejectRole);
        msgBox.setDefaultButton(btnChat);
        msgBox.exec();

        if (msgBox.clickedButton() == btnChat) {
            if (m_privateChatMgr) {
                m_privateChatMgr->openDirect(clientId, name, s.hostname, s.username);
                const QString sessionId = m_privateChatMgr->sessionIdForPeer(clientId);
                if (!sessionId.isEmpty() && !message.isEmpty()) {
                    m_privateChatMgr->appendSystemMessage(sessionId, QString(tr("Öğrencinin İlettiği Not: \"%1\"")).arg(message));
                }
            }
            if (m_chatWindow) m_chatWindow->showAndRaise();
        } else if (msgBox.clickedButton() == btnOpenChat) {
            if (m_chatWindow) m_chatWindow->showAndRaise();
        } else if (msgBox.clickedButton() == btnIgnore) {
            if (m_privateChatMgr) {
                m_privateChatMgr->ignoreUser(clientId, s.username);
            }
        }
    }
}

void MainWindow::onHandLowerReceived(const QString& clientId) {
    if (m_session) m_session->setClientHandRaised(clientId, false);
}

void MainWindow::onShowChat() {
    if (isScreenLocked()) return;
    if (m_chatWindow) m_chatWindow->showAndRaise();
}
