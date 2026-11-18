// master/exam/ExamDialog.cpp
#include "ExamDialog.h"
#include "ui_ExamDialog.h"
#include "../../common/utils/OkidiaUtils.h"

#include <QUuid>

ExamDialog::ExamDialog(QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::ExamDialog) {
    ui->setupUi(this);
    setFixedSize(sizeHint());
}

ExamDialog::~ExamDialog() {
    delete ui;
}

ExamConfig ExamDialog::config() const {
    ExamConfig cfg;
    cfg.examId          = OkidiaUtils::generateUuid().left(8);
    cfg.title           = ui->titleEdit->text().trimmed();
    if (cfg.title.isEmpty()) cfg.title = tr("Sınav");
    cfg.durationMinutes = ui->durationSpin->value();
    cfg.lockScreen      = ui->chkLockScreen->isChecked();
    cfg.lockInput       = ui->chkLockInput->isChecked();
    cfg.blockWeb        = ui->chkBlockWeb->isChecked();
    return cfg;
}
