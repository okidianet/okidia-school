// master/ui/ThumbnailGroupFrame/ThumbnailGroupFrame.h
#pragma once

#include <QWidget>
#include <QLabel>
#include <QGridLayout>
#include <QMap>
#include <QString>
#include <QStringList>

class ClientThumbnail;

class ThumbnailGroupFrame : public QWidget {
    Q_OBJECT
public:
    explicit ThumbnailGroupFrame(const QString& nodeId,
                                  const QString& groupName,
                                  bool isUnassigned = false,
                                  QWidget* parent   = nullptr);

    QString     nodeId()      const { return m_nodeId; }
    QString     groupName()   const { return m_groupName; }
    QStringList clientIds()   const { return m_thumbs.keys(); }
    int         count()       const { return m_thumbs.size(); }
    bool        isUnassigned()const { return m_isUnassigned; }

    void addThumbnail(ClientThumbnail* thumb);
    void removeThumbnail(const QString& clientId);
    ClientThumbnail* thumbnail(const QString& clientId) const;
    QList<ClientThumbnail*> sortedThumbnails() const;
    void relayout(int thumbW, int thumbH, int containerWidth);
    void setGroupName(const QString& name);
    void setAllSelected(bool selected);
    QWidget* thumbArea() const { return m_thumbArea; }
    bool isHeaderAt(const QPoint& localPos) const;

signals:
    void headerClicked(const QString& nodeId);
    void headerRightClicked(const QString& nodeId, const QPoint& globalPos);

protected:
    bool event(QEvent* e) override;

private:
    QString      m_nodeId;
    QString      m_groupName;
    bool         m_isUnassigned;

    QLabel*      m_header   = nullptr;
    QWidget*     m_thumbArea = nullptr;
    QGridLayout* m_grid     = nullptr;

    QMap<QString, ClientThumbnail*> m_thumbs;
    int m_thumbW = 160;
    int m_thumbH = 120;
};
