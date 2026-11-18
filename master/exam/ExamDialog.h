// master/exam/ExamDialog.h
#pragma once

#include <QDialog>
#include <QDateTime>

QT_BEGIN_NAMESPACE
namespace Ui { class ExamDialog; }
QT_END_NAMESPACE

struct ExamConfig {
    QString  examId;
    QString  title;
    int      durationMinutes = 40;  // 0 = süresiz
    bool     lockScreen     = false;
    bool     lockInput      = false;
    bool     blockWeb       = false;
};

/**
 * Sınav / ödev başlatma diyaloğu.
 * Öğretmen başlık, süre ve kısıtlamaları girer.
 */
class ExamDialog : public QDialog {
    Q_OBJECT

public:
    explicit ExamDialog(QWidget* parent = nullptr);
    ~ExamDialog();

    ExamConfig config() const;

private:
    Ui::ExamDialog* ui;
};
