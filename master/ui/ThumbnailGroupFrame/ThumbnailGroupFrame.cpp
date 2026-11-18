// master/ui/ThumbnailGroupFrame/ThumbnailGroupFrame.cpp
#include "ThumbnailGroupFrame.h"
#include "../ClientThumbnail/ClientThumbnail.h"
#include "../../common/utils/Logger.h"
#include "../../common/utils/OkidiaUtils.h"
#include <QStyle>

#include <QVBoxLayout>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QRegularExpression>
#include <QCollator>

ThumbnailGroupFrame::ThumbnailGroupFrame(const QString& nodeId,
                                          const QString& groupName,
                                          bool isUnassigned,
                                          QWidget* parent)
    : QWidget(parent)
    , m_nodeId(nodeId)
    , m_groupName(groupName)
    , m_isUnassigned(isUnassigned)
{
    auto* mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(0, 0, 0, 8);
    mainLay->setSpacing(0);

    // ── Grup başlığı ──────────────────────────────────────────────────────
    m_header = new QLabel(this);
    m_header->setFixedHeight(26);
    m_header->setContentsMargins(8, 0, 8, 0);
    m_header->setCursor(Qt::PointingHandCursor);
    setGroupName(groupName);  // metin ve stil
    mainLay->addWidget(m_header);

    // ── Thumbnail alanı ───────────────────────────────────────────────────
    m_thumbArea = new QWidget(this);
    m_grid = new QGridLayout(m_thumbArea);
    m_grid->setContentsMargins(8, 6, 8, 6);
    m_grid->setSpacing(8);
    mainLay->addWidget(m_thumbArea);


}

void ThumbnailGroupFrame::setGroupName(const QString& name)
{
    m_groupName = name;
    const QString icon = m_isUnassigned ? "" : "";
    m_header->setText(QString("  %1  %2  (%3)")
        .arg(icon).arg(name).arg(m_thumbs.size()));

    m_header->setProperty("class", "groupHeader");
    m_header->setProperty("isUnassigned", m_isUnassigned);
    m_header->style()->unpolish(m_header);
    m_header->style()->polish(m_header);
}

void ThumbnailGroupFrame::addThumbnail(ClientThumbnail* thumb)
{
    if (!thumb) return;
    m_thumbs[thumb->clientId()] = thumb;
    // setParent + hide: layout'a eklenmeden önce ayrı pencere olarak gösterilmesin
    thumb->setParent(m_thumbArea);
    thumb->hide();
    // Sayacı güncelle
    m_header->setText(m_header->text().replace(
        QRegularExpression(R"(\(\d+\))"),
        QString("(%1)").arg(m_thumbs.size())));
}

void ThumbnailGroupFrame::removeThumbnail(const QString& clientId)
{
    auto* t = m_thumbs.take(clientId);
    if (t) {
        m_grid->removeWidget(t);
        t->hide();
        t->setParent(nullptr);  // parent'ı sıfırla, başka frame'e eklenebilir
    }
    m_header->setText(m_header->text().replace(
        QRegularExpression(R"(\(\d+\))"),
        QString("(%1)").arg(m_thumbs.size())));
}

ClientThumbnail* ThumbnailGroupFrame::thumbnail(const QString& clientId) const
{
    return m_thumbs.value(clientId, nullptr);
}

QList<ClientThumbnail*> ThumbnailGroupFrame::sortedThumbnails() const
{
    QList<ClientThumbnail*> list = m_thumbs.values();
    std::sort(list.begin(), list.end(), [](ClientThumbnail* a, ClientThumbnail* b) {
        return OkidiaUtils::naturalCompare(a->state().hostname, b->state().hostname) < 0;
    });
    return list;
}

void ThumbnailGroupFrame::relayout(int thumbW, int thumbH, int containerWidth)
{
    m_thumbW = thumbW;
    m_thumbH = thumbH;

    // Grid'i temizle (widget'ları silmeden)
    while (m_grid->count() > 0) {
        QLayoutItem* item = m_grid->takeAt(0);
        delete item;
    }

    if (m_thumbs.isEmpty()) return;

    const int spacing  = 8;
    const int margins  = 16;  // sol+sağ
    const int avail    = containerWidth - margins;
    int cols = qMax(1, avail / (thumbW + spacing));

    QList<ClientThumbnail*> list = sortedThumbnails();

    for (int i = 0; i < list.size(); ++i) {
        list[i]->setFixedSize(thumbW, thumbH);
        m_grid->addWidget(list[i], i / cols, i % cols);
        list[i]->show();  // layout'a eklendikten sonra göster
    }
    m_grid->setColumnStretch(cols, 1);
    m_grid->setRowStretch((list.size() - 1) / cols + 1, 1);

    // Başlık sayısını güncelle
    m_header->setText(m_header->text().replace(
        QRegularExpression(R"(\(\d+\))"),
        QString("(%1)").arg(m_thumbs.size())));
}

void ThumbnailGroupFrame::setAllSelected(bool selected)
{
    for (auto* t : m_thumbs)
        t->setSelected(selected);
}

bool ThumbnailGroupFrame::isHeaderAt(const QPoint& localPos) const
{
    return m_header->geometry().contains(localPos);
}

// Başlık tıklaması → headerClicked sinyali
// Bunun için mousePressEvent'i override edip header alanına bakıyoruz.
// QLabel event'leri kendi yiyor — ana widget üzerine override ekliyoruz.
bool ThumbnailGroupFrame::event(QEvent* e)
{
    if (e->type() == QEvent::MouseButtonPress) {
        auto* me = static_cast<QMouseEvent*>(e);
        if (me->button() == Qt::LeftButton && isHeaderAt(me->pos()))
            emit headerClicked(m_nodeId);
    }
    if (e->type() == QEvent::ContextMenu) {
        auto* ce = static_cast<QContextMenuEvent*>(e);
        if (isHeaderAt(ce->pos()))
            emit headerRightClicked(m_nodeId, ce->globalPos());
    }
    return QWidget::event(e);
}
