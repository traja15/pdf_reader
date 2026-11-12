#include "core/TextSearch.h"

#include <QPdfDocument>
#include <QPdfSelection>
#include <optional>

namespace {

bool isBreak(QChar c) { return c == u'\r' || c == u'\n'; }
bool isHyphen(QChar c) { return c == u'-' || c == QChar(0x00AD) || c == QChar(0x0002) || c == QChar(0x2010); }

struct PageText {
    QString text;   ///< normalised (lower case, single spaces)
    QList<int> map; ///< index in text -> character index on the PDF page
};

} // namespace

QString TextSearch::normalize(const QString &text, QList<int> *map)
{
    QString out;
    out.reserve(text.size());
    if (map) {
        map->clear();
        map->reserve(text.size());
    }
    const int n = text.size();
    for (int i = 0; i < n; ++i) {
        const QChar c = text[i];
        // "exam-\r\nple" -> "example": hyphen at the end of a line joins the word.
        if (isHyphen(c) && i + 1 < n && isBreak(text[i + 1]) && i > 0 && text[i - 1].isLetter()) {
            int j = i + 1;
            while (j < n && (isBreak(text[j]) || text[j] == u' ')) ++j;
            if (j < n && text[j].isLetter()) {
                i = j - 1;
                continue;
            }
        }
        if (c.isSpace() || c == QChar(0x0002)) {
            if (!out.isEmpty() && out.back() != u' ') {
                out.append(u' ');
                if (map) map->append(i);
            }
            continue;
        }
        out.append(c.toLower());
        if (map) map->append(i);
    }
    return out;
}

/// Lives on the search thread.
class TextSearchWorker : public QObject
{
    Q_OBJECT
public:
    TextSearchWorker(QString path, std::shared_ptr<std::atomic<quint64>> current)
        : m_path(std::move(path)), m_current(std::move(current)) {}

    void run(quint64 id, const QString &query, int startPage, TextSearch *target)
    {
        if (!m_pdf) {
            m_pdf = std::make_unique<QPdfDocument>();
            if (m_pdf->load(m_path) != QPdfDocument::Error::None) {
                emit target->finished(id, 0);
                return;
            }
            m_pages.resize(m_pdf->pageCount());
        }
        const QString needle = TextSearch::normalize(query).trimmed();
        const int count = int(m_pages.size());
        int total = 0;
        if (needle.isEmpty() || count == 0) {
            emit target->finished(id, 0);
            return;
        }
        startPage = qBound(0, startPage, count - 1);
        // Pass 1 finds matches in the (cached) text, which is quick; pass 2 asks the PDF engine
        // where each match is on the page, which is slow, so the page you're on goes first.
        QList<std::pair<int, QList<qsizetype>>> found; // page, match offsets
        for (int k = 0; k < count; ++k) {
            if (m_current->load() != id)
                return; // a newer search replaced this one
            const int page = (startPage + k) % count;
            const PageText &pt = text(page);
            QList<qsizetype> at;
            for (qsizetype i = pt.text.indexOf(needle); i >= 0; i = pt.text.indexOf(needle, i + needle.size()))
                at.append(i);
            if (at.isEmpty())
                continue;
            total += at.size();
            found.append({page, at});
            if (page == startPage)
                emitHits(id, page, at, needle.size(), target); // the current page right away
        }
        emit target->counted(id, total);
        for (const auto &[page, at] : std::as_const(found)) {
            if (m_current->load() != id)
                return;
            if (page != startPage)
                emitHits(id, page, at, needle.size(), target);
        }
        emit target->finished(id, total);
    }

private:
    const PageText &text(int page)
    {
        if (!m_pages[page]) {
            PageText pt;
            pt.text = TextSearch::normalize(m_pdf->getAllText(page).text(), &pt.map);
            m_pages[page] = std::move(pt);
        }
        return *m_pages[page];
    }

    void emitHits(quint64 id, int page, const QList<qsizetype> &at, qsizetype length, TextSearch *target)
    {
        const PageText &pt = *m_pages[page];
        QList<TextHit> hits;
        for (const qsizetype i : at) {
            const int first = pt.map[i], last = pt.map[i + length - 1];
            TextHit hit{page, {}};
            for (const QPolygonF &poly : m_pdf->getSelectionAtIndex(page, first, last - first + 1).bounds())
                hit.rects.append(poly.boundingRect());
            if (!hit.rects.isEmpty())
                hits.append(std::move(hit));
        }
        if (!hits.isEmpty())
            emit target->pageHits(id, page, hits);
    }

    QString m_path;
    std::shared_ptr<std::atomic<quint64>> m_current;
    std::unique_ptr<QPdfDocument> m_pdf;
    std::vector<std::optional<PageText>> m_pages;
};

TextSearch::TextSearch(const QString &pdfPath, QObject *parent)
    : QObject(parent), m_current(std::make_shared<std::atomic<quint64>>(0))
{
    qRegisterMetaType<QList<TextHit>>();
    m_worker = new TextSearchWorker(pdfPath, m_current);
    m_worker->moveToThread(&m_thread);
    connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);
    m_thread.setObjectName("PenReader text search");
    m_thread.start(QThread::LowPriority);
}

TextSearch::~TextSearch()
{
    m_current->store(0);
    m_thread.quit();
    m_thread.wait();
}

quint64 TextSearch::search(const QString &text, int startPage)
{
    const quint64 id = ++m_next;
    m_current->store(id);
    QMetaObject::invokeMethod(m_worker, [w = m_worker, id, text, startPage, this] { w->run(id, text, startPage, this); },
                              Qt::QueuedConnection);
    return id;
}

void TextSearch::cancel() { m_current->store(0); }

#include "TextSearch.moc"
