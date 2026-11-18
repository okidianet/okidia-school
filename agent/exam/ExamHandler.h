// agent/exam/ExamHandler.h
#pragma once

#include <QObject>
#include "../../common/network/Connection.h"

class ExamOverlay;

/**
 * Sınav/ödev yönetimi — agent tarafı.
 *
 * EXAM_START  → Ekranın köşesinde "Sınavım Bitti" butonu göster
 * EXAM_COLLECT→ Öğrenciye teslim onayı sor, onaylarsa sinyal gönder
 * EXAM_END    → Butonu gizle
 */
class ExamHandler : public QObject {
    Q_OBJECT

public:
    explicit ExamHandler(QObject* parent = nullptr);

    void setConnection(Connection* conn) { m_conn = conn; }
    void handleEnvelope(const okidia::Envelope& envelope);

    bool isExamActive() const { return m_examActive; }

signals:
    void examSubmitted(const QString& examId);

private slots:
    void onSubmitClicked();

private:
    Connection*   m_conn       = nullptr;
    bool          m_examActive = false;
    QString       m_examId;
    QString       m_examTitle;
    ExamOverlay*  m_overlay    = nullptr;

    void startExam(const okidia::ExamCommand& cmd);
    void collectExam();
    void endExam();
    void cleanupExamFiles();
};
