#include "core/SearchIndex.h"

#include "core/Database.h"
#include "core/DocumentStore.h"
#include "net/Http.h"
#include "net/Ollama.h"

#include <QHash>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>
#include <QtConcurrent>
#include <algorithm>

namespace {

constexpr int kChunkWords = 160;
constexpr int kOverlapWords = 40;
constexpr int kMinWords = 8;
constexpr int kRrfK = 60;
constexpr int kEmbedBatch = 16;

QByteArray toBlob(const QVector<float> &v)
{
    return QByteArray(reinterpret_cast<const char *>(v.constData()), qsizetype(v.size() * sizeof(float)));
}

} // namespace

// ---------------------------------------------------------------- SearchIndex

SearchIndex::SearchIndex()
{
    QSqlQuery q(Database::db());
    q.exec("CREATE TABLE IF NOT EXISTS chunks (id INTEGER PRIMARY KEY, docKey TEXT NOT NULL, page INTEGER NOT NULL, "
           "pagePos REAL NOT NULL DEFAULT 0, text TEXT NOT NULL, embedding BLOB, model TEXT)");
    q.exec("CREATE INDEX IF NOT EXISTS chunks_doc ON chunks(docKey)");
    q.exec("CREATE TABLE IF NOT EXISTS search_docs (docKey TEXT PRIMARY KEY NOT NULL, chunkCount INTEGER NOT NULL, "
           "indexedAt INTEGER NOT NULL)");
    // Keyword index (FTS5). If this SQLite build lacks FTS5, smart search still works by meaning.
    m_fts = q.exec("CREATE VIRTUAL TABLE IF NOT EXISTS chunks_fts USING fts5(text, content='chunks', "
                   "content_rowid='id', tokenize='porter unicode61')");
    if (m_fts) {
        q.exec("CREATE TRIGGER IF NOT EXISTS chunks_ai AFTER INSERT ON chunks BEGIN "
               "INSERT INTO chunks_fts(rowid, text) VALUES (new.id, new.text); END");
        q.exec("CREATE TRIGGER IF NOT EXISTS chunks_ad AFTER DELETE ON chunks BEGIN "
               "INSERT INTO chunks_fts(chunks_fts, rowid, text) VALUES ('delete', old.id, old.text); END");
    }
}

QVector<Chunk> SearchIndex::chunkPages(const QStringList &pageTexts)
{
    static const QRegularExpression space("\\s+");
    QVector<Chunk> out;
    for (int page = 0; page < pageTexts.size(); ++page) {
        const QStringList words = pageTexts[page].split(space, Qt::SkipEmptyParts);
        if (words.size() < kMinWords)
            continue;
        for (int start = 0; start < words.size(); start += kChunkWords - kOverlapWords) {
            const int end = std::min<int>(start + kChunkWords, int(words.size()));
            if (end - start >= kMinWords || start == 0)
                out.append({page, double(start) / words.size(), words.mid(start, end - start).join(' ')});
            if (end == words.size())
                break;
        }
    }
    return out;
}

QSet<QString> SearchIndex::indexedDocuments() const
{
    QSet<QString> out;
    QSqlQuery q(Database::db());
    q.exec("SELECT docKey FROM search_docs");
    while (q.next())
        out.insert(q.value(0).toString());
    return out;
}

void SearchIndex::replaceDocument(const QString &docKey, const QVector<Chunk> &chunks)
{
    QSqlDatabase db = Database::db();
    db.transaction();
    QSqlQuery q(db);
    q.prepare("DELETE FROM chunks WHERE docKey = ?");
    q.addBindValue(docKey);
    q.exec();
    q.prepare("INSERT INTO chunks (docKey, page, pagePos, text) VALUES (?,?,?,?)");
    for (const Chunk &c : chunks) {
        q.addBindValue(docKey);
        q.addBindValue(c.page);
        q.addBindValue(c.pagePos);
        q.addBindValue(c.text);
        q.exec();
    }
    q.prepare("INSERT OR REPLACE INTO search_docs (docKey, chunkCount, indexedAt) VALUES (?,?,?)");
    q.addBindValue(docKey);
    q.addBindValue(int(chunks.size()));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.exec();
    db.commit();
    QMutexLocker lock(&m_cacheLock);
    m_cacheValid = false;
}

void SearchIndex::removeDocument(const QString &docKey)
{
    QSqlQuery q(Database::db());
    q.prepare("DELETE FROM chunks WHERE docKey = ?");
    q.addBindValue(docKey);
    q.exec();
    q.prepare("DELETE FROM search_docs WHERE docKey = ?");
    q.addBindValue(docKey);
    q.exec();
    QMutexLocker lock(&m_cacheLock);
    m_cacheValid = false;
}

void SearchIndex::clear()
{
    QSqlQuery q(Database::db());
    q.exec("DELETE FROM chunks");
    q.exec("DELETE FROM search_docs");
    QMutexLocker lock(&m_cacheLock);
    m_cacheValid = false;
}

QVector<std::pair<qint64, QString>> SearchIndex::chunksNeedingEmbedding(const QString &model, int limit) const
{
    QVector<std::pair<qint64, QString>> out;
    QSqlQuery q(Database::db());
    q.prepare("SELECT id, text FROM chunks WHERE embedding IS NULL OR model IS NULL OR model != ? LIMIT ?");
    q.addBindValue(model);
    q.addBindValue(limit);
    q.exec();
    while (q.next())
        out.append({q.value(0).toLongLong(), q.value(1).toString()});
    return out;
}

int SearchIndex::countChunks() const
{
    QSqlQuery q(Database::db());
    q.exec("SELECT COUNT(*) FROM chunks");
    return q.next() ? q.value(0).toInt() : 0;
}

int SearchIndex::countEmbedded(const QString &model) const
{
    QSqlQuery q(Database::db());
    q.prepare("SELECT COUNT(*) FROM chunks WHERE embedding IS NOT NULL AND model = ?");
    q.addBindValue(model);
    q.exec();
    return q.next() ? q.value(0).toInt() : 0;
}

void SearchIndex::storeEmbeddings(const QVector<qint64> &ids, const QVector<QVector<float>> &vectors, const QString &model)
{
    QSqlDatabase db = Database::db();
    db.transaction();
    QSqlQuery q(db);
    q.prepare("UPDATE chunks SET embedding = ?, model = ? WHERE id = ?");
    for (int i = 0; i < ids.size() && i < vectors.size(); ++i) {
        q.addBindValue(toBlob(vectors[i]));
        q.addBindValue(model);
        q.addBindValue(ids[i]);
        q.exec();
    }
    db.commit();
    QMutexLocker lock(&m_cacheLock);
    m_cacheValid = false;
}

void SearchIndex::loadVectors(const QString &model) const
{
    // Caller holds m_cacheLock.
    m_ids.clear();
    m_docs.clear();
    m_vectors.clear();
    m_dim = 0;
    QSqlQuery q(Database::db());
    q.prepare("SELECT id, docKey, embedding FROM chunks WHERE embedding IS NOT NULL AND model = ?");
    q.addBindValue(model);
    q.exec();
    while (q.next()) {
        const QByteArray blob = q.value(2).toByteArray();
        const int dim = int(blob.size() / sizeof(float));
        if (dim == 0 || (m_dim && dim != m_dim))
            continue;
        m_dim = dim;
        m_ids.append(q.value(0).toLongLong());
        m_docs.append(q.value(1).toString());
        const auto *f = reinterpret_cast<const float *>(blob.constData());
        m_vectors.append(QVector<float>(f, f + dim));
    }
    m_cacheModel = model;
    m_cacheValid = true;
}

QVector<SearchHit> SearchIndex::search(const QString &query, const QVector<float> &queryVector, const QString &model,
                                       const QSet<QString> &allowedDocs, int limit) const
{
    QHash<qint64, double> fused;
    QSet<qint64> byMeaning, byKeyword;

    // Meaning: cosine similarity against every passage (vectors are normalised).
    if (!queryVector.isEmpty()) {
        QMutexLocker lock(&m_cacheLock);
        if (!m_cacheValid || m_cacheModel != model)
            loadVectors(model);
        if (m_dim == queryVector.size()) {
            QVector<std::pair<float, int>> scored;
            scored.reserve(m_ids.size());
            for (int i = 0; i < m_ids.size(); ++i) {
                if (!allowedDocs.isEmpty() && !allowedDocs.contains(m_docs[i])) continue;
                const float *v = m_vectors.constData() + qsizetype(i) * m_dim;
                float dot = 0;
                for (int d = 0; d < m_dim; ++d) dot += v[d] * queryVector[d];
                scored.append({dot, i});
            }
            const int keep = std::min<int>(100, int(scored.size()));
            std::partial_sort(scored.begin(), scored.begin() + keep, scored.end(),
                              [](const auto &a, const auto &b) { return a.first > b.first; });
            for (int r = 0; r < keep; ++r) {
                const qint64 id = m_ids[scored[r].second];
                fused[id] += 1.0 / (kRrfK + r + 1);
                byMeaning.insert(id);
            }
        }
    }

    // Keywords: BM25 over the FTS5 index (any of the query's words).
    if (m_fts) {
        static const QRegularExpression wordRe("[\\p{L}\\p{N}]{2,}");
        QStringList terms;
        for (auto it = wordRe.globalMatch(query); it.hasNext() && terms.size() < 12;) {
            const QString t = it.next().captured().toLower();
            if (!terms.contains('"' + t + '"')) terms << '"' + t + '"';
        }
        if (!terms.isEmpty()) {
            QSqlQuery q(Database::db());
            q.prepare("SELECT c.id, c.docKey FROM chunks_fts JOIN chunks c ON c.id = chunks_fts.rowid "
                      "WHERE chunks_fts MATCH ? ORDER BY bm25(chunks_fts) LIMIT 150");
            q.addBindValue(terms.join(" OR "));
            q.exec();
            int rank = 0;
            while (q.next()) {
                if (!allowedDocs.isEmpty() && !allowedDocs.contains(q.value(1).toString())) continue;
                const qint64 id = q.value(0).toLongLong();
                fused[id] += 1.0 / (kRrfK + rank + 1);
                byKeyword.insert(id);
                ++rank;
            }
        }
    }

    QVector<std::pair<double, qint64>> ranked;
    for (auto it = fused.begin(); it != fused.end(); ++it)
        ranked.append({it.value(), it.key()});
    std::sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first > b.first; });

    // At most a few passages per page, so one dense page doesn't crowd out everything else.
    constexpr int kPerPage = 3;
    QVector<SearchHit> hits;
    QHash<QString, int> perPage;
    QSqlQuery q(Database::db());
    q.prepare("SELECT docKey, page, pagePos, text FROM chunks WHERE id = ?");
    for (const auto &[score, id] : ranked) {
        if (hits.size() >= limit) break;
        q.addBindValue(id);
        if (!q.exec() || !q.next()) continue;
        const QString docKey = q.value(0).toString();
        const int page = q.value(1).toInt();
        const QString pageKey = docKey + ':' + QString::number(page);
        if (perPage[pageKey]++ >= kPerPage) continue;
        hits.append({docKey, page, q.value(2).toDouble(), q.value(3).toString(), score, byMeaning.contains(id),
                     byKeyword.contains(id)});
    }
    return hits;
}

// ---------------------------------------------------------------- Indexer

Indexer::Indexer(DocumentStore *docs, SearchIndex *index, QObject *parent)
    : QObject(parent), m_docs(docs), m_index(index)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(3000);
    connect(&m_debounce, &QTimer::timeout, this, &Indexer::start);
}

void Indexer::requestSoon() { m_debounce.start(); }

void Indexer::start()
{
    if (m_running.exchange(true)) {
        m_again = true;
        return;
    }
    m_stop = false;
    (void)QtConcurrent::run([this] { run(); });
}

void Indexer::report(const QString &text, bool busy)
{
    QMetaObject::invokeMethod(this, [this, text, busy] {
        m_status = text;
        emit statusChanged(text, busy);
    }, Qt::QueuedConnection);
}

void Indexer::run()
{
    do {
        m_again = false;
        // Phase 1: read new PDFs into passages (keyword search works as soon as this is done).
        const QVector<Document> library = m_docs->library();
        QSet<QString> inLibrary;
        for (const Document &d : library) inLibrary.insert(d.docKey);
        QSet<QString> indexed = m_index->indexedDocuments();
        for (const QString &key : indexed)
            if (!inLibrary.contains(key)) m_index->removeDocument(key); // deleted everywhere

        QVector<Document> todo;
        for (const Document &d : library)
            if (d.onDevice() && !indexed.contains(d.docKey)) todo.append(d);
        for (int i = 0; i < todo.size() && !m_stop; ++i) {
            const Document &d = todo[i];
            report(QStringLiteral("Reading “%1” (%2 of %3)…").arg(d.title).arg(i + 1).arg(todo.size()), true);
            QPdfDocument pdf;
            if (pdf.load(d.localPath) != QPdfDocument::Error::None) continue;
            QStringList pages;
            for (int p = 0; p < pdf.pageCount(); ++p)
                pages << pdf.getAllText(p).text();
            m_index->replaceDocument(d.docKey, SearchIndex::chunkPages(pages));
            QMetaObject::invokeMethod(this, &Indexer::indexChanged, Qt::QueuedConnection);
        }

        // Phase 2: embeddings for meaning-based search (needs Ollama and the embedding model).
        const QString model = AiPrefs::embeddingModel();
        const int total = m_index->countChunks();
        bool embeddingsReady = false;
        QString problem;
        try {
            if (!Ollama::hasModel(model)) {
                problem = QStringLiteral("Keyword search ready · download “%1” in Settings → Smart search for "
                                         "meaning-based search").arg(model);
            } else {
                int done = m_index->countEmbedded(model);
                while (!m_stop) {
                    const auto batch = m_index->chunksNeedingEmbedding(model, kEmbedBatch);
                    if (batch.isEmpty()) break;
                    QVector<qint64> ids;
                    QStringList texts;
                    for (const auto &[id, text] : batch) { ids << id; texts << text; }
                    m_index->storeEmbeddings(ids, Ollama::embed(texts, false), model);
                    done += int(batch.size());
                    report(QStringLiteral("Understanding passages… %1 of %2").arg(done).arg(total), true);
                    if (done % (kEmbedBatch * 8) == 0)
                        QMetaObject::invokeMethod(this, &Indexer::indexChanged, Qt::QueuedConnection);
                }
                embeddingsReady = true;
            }
        } catch (const HttpError &e) {
            problem = e.status == 0 ? QString("Keyword search ready · start Ollama (“ollama serve”) for meaning-based search")
                                    : QString("Smart search: ") + e.what();
        }
        QMetaObject::invokeMethod(this, &Indexer::indexChanged, Qt::QueuedConnection);

        const int docs = int(m_index->indexedDocuments().size());
        if (m_stop) report("Indexing paused", false);
        else if (!embeddingsReady) report(problem, false);
        else report(QStringLiteral("Smart search ready · %1 PDFs, %2 passages").arg(docs).arg(total), false);
    } while (m_again && !m_stop);
    m_running = false;
}
