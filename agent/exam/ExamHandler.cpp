// agent/exam/ExamHandler.cpp
#include "ExamHandler.h"
#include "../../common/utils/Logger.h"

#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QDateTime>
#include <QMessageBox>
#include <QApplication>
#include <QScreen>
#include <QDir>
#include <QFile>

// ---------------------------------------------------------------------------
// ExamOverlay — ekranın sağ alt köşesinde duran küçük panel
// ---------------------------------------------------------------------------
class ExamOverlay : public QWidget {
    Q_OBJECT
public:
    explicit ExamOverlay(const QString& title, QWidget* parent = nullptr)
        : QWidget(parent,
                  Qt::Tool
                | Qt::FramelessWindowHint
                | Qt::WindowStaysOnTopHint) {

        setAttribute(Qt::WA_DeleteOnClose, false);
        setStyleSheet(
            "QWidget { background: #1e1e2e; border-radius: 8px; "
            "          border: 2px solid #89b4fa; }"
        );

        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(12, 10, 12, 10);
        layout->setSpacing(8);

        auto* titleLabel = new QLabel("" + title, this);
        titleLabel->setStyleSheet(
            "color: #cdd6f4; font-size: 12px; font-weight: bold;"
            "border: none;");
        layout->addWidget(titleLabel);

        m_timerLabel = new QLabel("00:00", this);
        m_timerLabel->setStyleSheet(
            "color: #f9e2af; font-size: 18px; font-weight: bold;"
            "text-align: center; border: none;");
        m_timerLabel->setAlignment(Qt::AlignCenter);
        layout->addWidget(m_timerLabel);

        m_submitBtn = new QPushButton(tr("Sınavım Bitti"), this);
        m_submitBtn->setStyleSheet(
            "QPushButton { background: #a6e3a1; color: #1e1e2e; "
            "border-radius: 6px; padding: 6px 16px; font-weight: bold; "
            "font-size: 12px; border: none; }"
            "QPushButton:hover { background: #94e2d5; }");
        layout->addWidget(m_submitBtn);

        connect(m_submitBtn, &QPushButton::clicked,
                this, &ExamOverlay::submitClicked);

        // Geçen süreyi say
        m_elapsed = 0;
        m_clock = new QTimer(this);
        m_clock->setInterval(1000);
        connect(m_clock, &QTimer::timeout, [this]() {
            ++m_elapsed;
            int h = m_elapsed / 3600;
            int m = (m_elapsed % 3600) / 60;
            int s = m_elapsed % 60;
            m_timerLabel->setText(
                h > 0
                ? QString("%1:%2:%3")
                    .arg(h, 2, 10, QChar('0'))
                    .arg(m, 2, 10, QChar('0'))
                    .arg(s, 2, 10, QChar('0'))
                : QString("%1:%2")
                    .arg(m, 2, 10, QChar('0'))
                    .arg(s, 2, 10, QChar('0')));
        });
        m_clock->start();

        adjustSize();
        positionBottomRight();
    }

    void positionBottomRight() {
        QRect screen = QApplication::primaryScreen()->availableGeometry();
        move(screen.right()  - width()  - 16,
             screen.bottom() - height() - 48);
    }

signals:
    void submitClicked();

private:
    QLabel*     m_timerLabel;
    QPushButton* m_submitBtn;
    QTimer*     m_clock;
    int         m_elapsed;
};

#include "ExamHandler.moc"

// ---------------------------------------------------------------------------
// ExamHandler
// ---------------------------------------------------------------------------
ExamHandler::ExamHandler(QObject* parent)
    : QObject(parent) {}

void ExamHandler::handleEnvelope(const okidia::Envelope& envelope) {
    switch (envelope.type()) {
    case okidia::MessageType::EXAM_START: {
        okidia::ExamCommand cmd;
        if (cmd.ParseFromString(envelope.payload()))
            startExam(cmd);
        break;
    }
    case okidia::MessageType::EXAM_COLLECT:
        collectExam();
        break;
    case okidia::MessageType::EXAM_END:
        endExam();
        break;
    default:
        break;
    }
}

void ExamHandler::startExam(const okidia::ExamCommand& cmd) {
    if (m_examActive) endExam();

    m_examId    = QString::fromStdString(cmd.exam_id());
    m_examTitle = QString::fromStdString(cmd.title());
    m_examActive = true;

    Logger::info(QObject::tr("ExamHandler: Sınav başladı → ") + m_examTitle);

    m_overlay = new ExamOverlay(m_examTitle);
    connect(m_overlay, SIGNAL(submitClicked()),
            this, SLOT(onSubmitClicked()));
    m_overlay->show();
    m_overlay->raise();
}

void ExamHandler::collectExam() {
    // Öğretmen EXAM_COLLECT gönderdi: öğrenciye onay sor
    Logger::info(QObject::tr("ExamHandler: Teslim isteği alındı"));
    QMessageBox::information(
        nullptr,
        tr("Sınav Teslimi"),
        tr("Öğretmeniniz sınavı toplamaya başladı.\n"
           "Hazır olduğunuzda 'Sınavım Bitti' butonuna tıklayın."));
}

void ExamHandler::onSubmitClicked() {
    int ret = QMessageBox::question(
        nullptr,
        tr("Sınavı Teslim Et"),
        tr("Sınavınızı teslim etmek istediğinizden emin misiniz?"),
        QMessageBox::Yes | QMessageBox::No);

    if (ret != QMessageBox::Yes) return;

    Logger::info(QObject::tr("ExamHandler: Sınav teslim edildi → ") + m_examId);

    if (m_conn) {
        okidia::ExamCommand result;
        result.set_exam_id(m_examId.toStdString());

        okidia::Envelope env;
        env.set_type(okidia::MessageType::EXAM_COLLECT);
        env.set_payload(result.SerializeAsString());
        m_conn->sendEnvelope(env);
    }

    emit examSubmitted(m_examId);
    endExam();
}

void ExamHandler::cleanupExamFiles() {
    if (m_examId.isEmpty()) return;

    // Sınav esnasında temp dizininde tutulmuş olabilecek tüm geçici materyal veya taslakları temizle (KVKK veri minimizasyonu)
    QDir tempDir(QDir::tempPath());
    QString examPrefix = QString("okidia_exam_%1").arg(m_examId);
    QStringList filters;
    filters << QString("%1*").arg(examPrefix);
    
    QFileInfoList files = tempDir.entryInfoList(filters, QDir::Files | QDir::NoDotAndDotDot);
    for (const QFileInfo& fi : files) {
        if (QFile::remove(fi.absoluteFilePath())) {
            Logger::info(QObject::tr("ExamHandler: Geçici sınav dosyası temizlendi → ") + fi.fileName());
        }
    }
}

void ExamHandler::endExam() {
    if (!m_examActive) return;
    m_examActive = false;

    if (m_overlay) {
        m_overlay->hide();
        m_overlay->deleteLater();
        m_overlay = nullptr;
    }

    cleanupExamFiles();

    Logger::info(QObject::tr("ExamHandler: Sınav bitti → ") + m_examId);
    m_examId.clear();
    m_examTitle.clear();
}
