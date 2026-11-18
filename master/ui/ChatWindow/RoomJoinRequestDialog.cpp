// master/ui/ChatWindow/RoomJoinRequestDialog.cpp
#include "RoomJoinRequestDialog.h"
#include "../../../common/ui/ThemeManager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>

RoomJoinRequestDialog::RoomJoinRequestDialog(const QString& requestId,
                                             const QString& roomId,
                                             const QString& roomName,
                                             const QString& fromUsername,
                                             const QString& fromDisplay,
                                             QWidget* parent)
    : QDialog(parent)
    , m_requestId(requestId)
    , m_roomId(roomId)
    , m_roomName(roomName)
    , m_fromUsername(fromUsername)
    , m_fromDisplay(fromDisplay.isEmpty() ? fromUsername : fromDisplay)
{
    setWindowTitle(tr("Sohbet Katılım İsteği"));
    setWindowIcon(ThemeManager::icon(":/ui/chat.svg"));
    setModal(true);
    setFixedSize(420, 230);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(20, 20, 20, 20);
    mainLayout->setSpacing(14);

    // Başlık ve İkon
    auto* headerLayout = new QHBoxLayout();
    auto* iconLabel = new QLabel(this);
    iconLabel->setPixmap(ThemeManager::icon(":/ui/school.svg").pixmap(36, 36));
    headerLayout->addWidget(iconLabel);

    auto* textLayout = new QVBoxLayout();
    m_titleLabel = new QLabel(QString("<b>%1</b> (%2)").arg(m_fromDisplay, m_fromUsername), this);
    m_titleLabel->setStyleSheet("font-size: 15px;");
    m_messageLabel = new QLabel(tr("<b>%1</b> sınıfının sohbet odasına katılmak istiyor.").arg(m_roomName), this);
    m_messageLabel->setWordWrap(true);
    m_messageLabel->setStyleSheet("color: #64748b; font-size: 13px;");
    textLayout->addWidget(m_titleLabel);
    textLayout->addWidget(m_messageLabel);
    headerLayout->addLayout(textLayout, 1);
    mainLayout->addLayout(headerLayout);

    // Geri sayım çubuğu ve yazı
    auto* timerLayout = new QHBoxLayout();
    m_timerLabel = new QLabel(tr("%1 saniye kaldı").arg(m_timeLeft), this);
    m_timerLabel->setStyleSheet("font-size: 12px; font-weight: bold; color: #f59e0b;");
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 30);
    m_progressBar->setValue(30);
    m_progressBar->setTextVisible(false);
    m_progressBar->setFixedHeight(6);
    m_progressBar->setStyleSheet(
        "QProgressBar { background-color: #334155; border-radius: 3px; }"
        "QProgressBar::chunk { background-color: #38bdf8; border-radius: 3px; }"
    );
    timerLayout->addWidget(m_timerLabel);
    mainLayout->addLayout(timerLayout);
    mainLayout->addWidget(m_progressBar);

    // Butonlar
    auto* btnLayout = new QHBoxLayout();
    btnLayout->setSpacing(12);

    m_rejectBtn = new QPushButton(tr("Reddet"), this);
    m_rejectBtn->setFixedHeight(36);
    m_rejectBtn->setCursor(Qt::PointingHandCursor);
    m_rejectBtn->setStyleSheet(
        "QPushButton { background-color: #ef4444; color: white; border: none; border-radius: 6px; font-weight: bold; padding: 0 16px; }"
        "QPushButton:hover { background-color: #dc2626; }"
    );
    connect(m_rejectBtn, &QPushButton::clicked, this, &RoomJoinRequestDialog::onReject);

    m_acceptBtn = new QPushButton(tr("Kabul Et"), this);
    m_acceptBtn->setFixedHeight(36);
    m_acceptBtn->setCursor(Qt::PointingHandCursor);
    m_acceptBtn->setStyleSheet(
        "QPushButton { background-color: #10b981; color: white; border: none; border-radius: 6px; font-weight: bold; padding: 0 16px; }"
        "QPushButton:hover { background-color: #059669; }"
    );
    connect(m_acceptBtn, &QPushButton::clicked, this, &RoomJoinRequestDialog::onAccept);

    btnLayout->addWidget(m_rejectBtn);
    btnLayout->addWidget(m_acceptBtn);
    mainLayout->addLayout(btnLayout);

    // Zamanlayıcı
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &RoomJoinRequestDialog::onTick);
    m_timer->start(1000);
}

void RoomJoinRequestDialog::onTick() {
    --m_timeLeft;
    m_progressBar->setValue(m_timeLeft);
    m_timerLabel->setText(tr("%1 saniye kaldı").arg(m_timeLeft));
    if (m_timeLeft <= 0) {
        m_timer->stop();
        reject();
    }
}

void RoomJoinRequestDialog::onAccept() {
    if (m_timer) m_timer->stop();
    accept();
}

void RoomJoinRequestDialog::onReject() {
    if (m_timer) m_timer->stop();
    reject();
}
