// agent/ui/PopupMessage/PopupMessage.cpp
#include "PopupMessage.h"
#include "ui_PopupMessage.h"

#include <QTimer>

// ---------------------------------------------------------------------------
// Statik fabrika
// ---------------------------------------------------------------------------
void PopupMessage::show(const QString& message, const QString& sender) {
    PopupMessage* dlg = new PopupMessage(message, sender);

    // 60 saniye sonra otomatik kapat
    QTimer::singleShot(60000, dlg, &QDialog::accept);

    dlg->setWindowFlag(Qt::WindowStaysOnTopHint, true);
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    dlg->setModal(false);
    dlg->QDialog::show();
    dlg->raise();
    dlg->activateWindow();
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
PopupMessage::PopupMessage(const QString& message,
                           const QString& sender,
                           QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::PopupMessage)
{
    ui->setupUi(this);
    ui->messageLabel->setText(message);

    // sender formatı: "ROLE|Header"  (ör. "YM|Onur Tamer Ekmenci (Yönetici)")
    // Eski formatlar (ör. "YÖNETİCİ|hostname" veya sadece isim) da tam desteklenir
    if (sender.contains('|')) {
        const int sep      = sender.indexOf('|');
        const QString role = sender.left(sep).trimmed().toUpper();
        const QString header = sender.mid(sep + 1).trimmed();

        // Pencere başlığı: "YÖNETİCİ MESAJI" / "ÖĞRETMEN MESAJI"
        if (role == "YM" || role == tr("YÖNETİCİ") || role == tr("YÖNETICI") || role == "ADMIN")
            setWindowTitle(tr("YÖNETİCİ MESAJI"));
        else
            setWindowTitle(tr("ÖĞRETMEN MESAJI"));

        // Header label: Yöneticinin tam adı ve unvanı
        ui->headerLabel->setText(header);
    } else if (!sender.isEmpty()) {
        // Eski format — başlık değişmesin, sadece header güncelle
        ui->headerLabel->setText(sender);
    }
}

PopupMessage::~PopupMessage() {
    delete ui;
}
