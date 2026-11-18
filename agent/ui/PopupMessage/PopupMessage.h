// agent/ui/PopupMessage/PopupMessage.h
#pragma once

#include <QDialog>

QT_BEGIN_NAMESPACE
namespace Ui { class PopupMessage; }
QT_END_NAMESPACE

/**
 * Öğretmenden gelen MSG_POPUP komutunu ekranda gösterir.
 *
 * - Her çağrıda yeni bir dialog penceresi açılır (NonModal).
 * - Ekran kilitliyken de üstte görünür (WindowStaysOnTopHint).
 * - Kullanıcı "Tamam" dediğinde veya 60 saniye sonra otomatik kapanır.
 *
 * Kullanım:
 *   PopupMessage::show("Sınav 5 dakikaya bitiyor!");
 */
class PopupMessage : public QDialog {
    Q_OBJECT

public:
    // Yeni popup aç (non-modal, üstte)
    static void show(const QString& message,
                     const QString& sender = QString());

private:
    explicit PopupMessage(const QString& message,
                          const QString& sender,
                          QWidget* parent = nullptr);
    ~PopupMessage();

    Ui::PopupMessage* ui;
};
