#pragma once

#include <QList>
#include <QObject>
#include <QRectF>
#include <QThread>
#include <atomic>
#include <memory>

/// One match: the page and its highlight rectangles in PDF points.
struct TextHit {
    int page = 0;
    QList<QRectF> rects;
};

class TextSearchWorker;

/**
 * Fast find-in-document. Runs on its own thread with its own copy of the PDF, so the reader never
 * waits for it. Page text is extracted once and cached, so later searches in the same PDF are
 * nearly instant. Results stream in page by page, starting at the page you are on and wrapping round.
 * Case-insensitive; spaces and line breaks match each other, and words hyphenated across a line
 * break are found.
 */
class TextSearch : public QObject
{
    Q_OBJECT
public:
    explicit TextSearch(const QString &pdfPath, QObject *parent = nullptr);
    ~TextSearch() override;

    /// Starts a new search (cancelling any running one). Returns its id, sent with every result.
    quint64 search(const QString &text, int startPage);
    void cancel();

    /// Normalisation shared by page text and queries (exposed for tests).
    static QString normalize(const QString &text, QList<int> *map = nullptr);

signals:
    /// Matches found on one page (sorted top to bottom).
    void pageHits(quint64 id, int page, const QList<TextHit> &hits);
    /// How many matches the whole PDF has (sent once the text is scanned; boxes may still be coming).
    void counted(quint64 id, int total);
    /// Every page has been searched.
    void finished(quint64 id, int total);

private:
    QThread m_thread;
    TextSearchWorker *m_worker;
    std::shared_ptr<std::atomic<quint64>> m_current;
    quint64 m_next = 0;
};
