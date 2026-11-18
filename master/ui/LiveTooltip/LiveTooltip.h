#pragma once

#include <QWidget>
#include <QLabel>
#include <QVBoxLayout>
#include <QString>
#include "../../session/SessionManager.h" // For ClientState access or we can pass ClientState

struct ClientState; // Forward declare if we just pass struct

class LiveTooltip : public QWidget {
    Q_OBJECT

public:
    explicit LiveTooltip(QWidget* parent = nullptr);
    
    // Updates tooltip with latest client state
    void updateData(const ClientState& state);
    
    // Show tooltip near the target widget
    void showAt(QWidget* target);

    void startDismissTimer(int timeoutMs = 7000);
    void stopDismissTimer();

signals:
    void tooltipDismissed();

protected:
    void hideEvent(QHideEvent* event) override;

private:
    QLabel* m_contentLabel = nullptr;
    class QTimer* m_dismissTimer = nullptr;
};
