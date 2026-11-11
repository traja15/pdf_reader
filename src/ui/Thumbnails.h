#pragma once

#include "core/Models.h"

#include <QHash>
#include <QIcon>
#include <QObject>
#include <QSet>

/**
 * Square thumbnails of the page each PDF is open at (the top of the page, scaled to the tile width).
 * Rendered in the background and cached in memory and on disk; a new page gives a new thumbnail.
 */
class Thumbnails : public QObject {
    Q_OBJECT
public:
    explicit Thumbnails(QObject *parent = nullptr);

    /// The thumbnail if it's ready, otherwise a placeholder (and rendering starts).
    QIcon icon(const Document &doc);

signals:
    void ready(const QString &docKey);

private:
    static QString key(const Document &doc);
    QString m_dir;
    QHash<QString, QPixmap> m_cache;
    QSet<QString> m_pending;
};
