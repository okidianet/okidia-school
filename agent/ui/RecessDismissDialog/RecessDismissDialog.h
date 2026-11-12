// agent/ui/RecessDismissDialog/RecessDismissDialog.h
#pragma once

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QTimer>

/**
 * RecessDismissDialog — Teneffüs Nezaket Penceresi (Grace Period Dialog)
 *
 * Ders bitiş zili çaldığında ekranı pat diye kapatmak yerine öğretmene
 * sözünü veya dersini bitirmesi için geri sayımlı bir nezaket süresi tanır.
 *
 * Özellikler:
 * - 30 saniyelik görsel geri sayım barı (Progress Bar)
 * - [🔒 Şimdi Kilitle]: Hemen beklemeden kilitler
 * - [⏱️ +1 Dk Uzat]: 60 saniye ek süre verir
 * - Sahipsiz tahta güvenliği: Süre bittiğinde tahta kendiliğinden kilitlenir
 */
class RecessDismissDialog : public QDialog {
    Q_OBJECT

public:
    static RecessDismissDialog& instance();

    void showGracePeriod(int seconds = 30, const QString& message = QString(), bool isLogout = false);
    void dismiss();
    bool isRunning() const;

signals:
    void lockNowRequested();
    void logoutNowRequested();

private slots:
    void onTick();
    void onLockNow();
    void onExtendOneMinute();
    void onEjectUsb();

private:
    explicit RecessDismissDialog(QWidget* parent = nullptr);
    ~RecessDismissDialog() override = default;

    RecessDismissDialog(const RecessDismissDialog&) = delete;
    RecessDismissDialog& operator=(const RecessDismissDialog&) = delete;

    void setupUi();
    void updateUiTime();
    void positionToBottomRight();

    QLabel*       m_lblTitle    = nullptr;
    QLabel*       m_lblMessage  = nullptr;
    QProgressBar* m_progressBar = nullptr;
    QPushButton*  m_btnLockNow  = nullptr;
    QPushButton*  m_btnExtend   = nullptr;
    QPushButton*  m_btnEjectUsb = nullptr;

    QTimer*       m_timer       = nullptr;
    int           m_remainingSec = 30;
    int           m_totalSec     = 30;
    bool          m_extended     = false;
    bool          m_isLogout     = false;
};
