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
    if (m_session) m_session->setClientHandRaised(clientId, true, message);
    const ClientState s = m_session ? m_session->clientState(clientId) : ClientState{};
    const QString name = s.hostname.isEmpty() ? clientId : (s.username.isEmpty() ? s.hostname : (s.hostname + " (" + s.username + ")"));
    const QString label = tr("Yardım Talebi: ") + name
                          + (message.isEmpty() ? "" : " — \"" + message + "\"");
    showCommandStatus(true, label);

    if (isScreenLocked()) {
        return;
    }

    if (!m_chatWindow || !m_chatWindow->isVisible()) {
        QMessageBox msgBox(this);
        msgBox.setWindowTitle(tr("Öğrenci Yardım Talebi"));
        QString text = QString(tr("<b>%1</b> yardım talebinde bulundu!")).arg(name);
        if (!message.isEmpty()) {
            text += QString("<br><i>Mesaj: \"%1\"</i>").arg(message);
        }
        text += tr("<br><br>Bu öğrenci ile özel sohbet başlatmak ister misiniz?");
        msgBox.setText(text);
        msgBox.setIcon(QMessageBox::Information);
        QPushButton* btnChat = msgBox.addButton(tr("Özel Sohbet Başlat"), QMessageBox::AcceptRole);
        QPushButton* btnOpenChat = msgBox.addButton(tr("Sohbet Penceresini Aç"), QMessageBox::ActionRole);
        msgBox.addButton("Kapat", QMessageBox::RejectRole);
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
