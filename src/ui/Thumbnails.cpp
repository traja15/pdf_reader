#include "ui/Thumbnails.h"

#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QPainter>
#include <QPdfDocument>
#include <QStandardPaths>
#include <QtConcurrent>

namespace {
constexpr int kSize = 300; // pixels; tiles show it at ~150 px, so it stays sharp on HiDPI screens

/// The top of the page, scaled to fill a square tile (like a book cover), with a thin border.
QImage renderSquare(const QString &path, int page)
{
    QPdfDocument pdf;
    if (pdf.load(path) != QPdfDocument::Error::None || pdf.pageCount() == 0)
        return {};
    page = qBound(0, page, pdf.pageCount() - 1);
    const QSizeF pts = pdf.pagePointSize(page);
    const int height = pts.width() > 0 ? qRound(kSize * pts.height() / pts.width()) : kSize;
    const QImage pageImage = pdf.render(page, QSize(kSize, height));

    QImage square(kSize, kSize, QImage::Format_RGB32);
    square.fill(Qt::white);
    QPainter p(&square);
    p.drawImage(QPoint(0, 0), pageImage); // top-aligned; anything below the square is cropped
    p.setPen(QColor(0, 0, 0, 40));
    p.drawRect(square.rect().adjusted(0, 0, -1, -1));
    return square;
}
} // namespace

Thumbnails::Thumbnails(QObject *parent)
    : QObject(parent), m_dir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/thumbs")
{
    QDir().mkpath(m_dir);
}

QString Thumbnails::key(const Document &doc) { return doc.docKey + "_" + QString::number(doc.lastPage); }

QIcon Thumbnails::icon(const Document &doc)
{
    if (!doc.onDevice())
        return QIcon(":/resources/icons/cloud.svg");
    const QString k = key(doc);
    if (const auto it = m_cache.find(k); it != m_cache.end())
        return QIcon(*it);

    const QString file = m_dir + "/" + k + ".png";
    if (QPixmap disk; disk.load(file)) {
        m_cache.insert(k, disk);
        return QIcon(disk);
    }
    if (!m_pending.contains(k)) {
        m_pending.insert(k);
        auto *watcher = new QFutureWatcher<QImage>(this);
        const QString docKey = doc.docKey;
        connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, k, file, docKey] {
            watcher->deleteLater();
            m_pending.remove(k);
            const QImage image = watcher->result();
            if (image.isNull()) return;
            // Drop thumbnails of pages this PDF is no longer open at.
            for (const QString &old : QDir(m_dir).entryList({docKey + "_*.png"}))
                QFile::remove(m_dir + "/" + old);
            image.save(file);
            m_cache.insert(k, QPixmap::fromImage(image));
            emit ready(docKey);
        });
        watcher->setFuture(QtConcurrent::run(renderSquare, doc.localPath, doc.lastPage));
    }
    return QIcon(":/resources/icons/pdf.svg");
}
