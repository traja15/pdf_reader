#pragma once

#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QVector>
#include <atomic>

class DocumentStore;

/// A passage of a PDF, the unit of smart search.
struct Chunk {
    int page = 0;
    double pagePos = 0; ///< where on the page it starts (0..1), to scroll close to it
    QString text;
};

struct SearchHit {
    QString docKey;
    int page = 0;
    double pagePos = 0;
    QString text;
    double score = 0;
    bool byMeaning = false; ///< found by the embedding (semantic) search
    bool byKeyword = false; ///< found by the keyword (full-text) search
};

/**
 * Passage index for smart search, stored in SQLite next to the library:
 *  - chunks: passages (~160 words, never crossing pages) with their embedding vector
 *  - chunks_fts: SQLite FTS5 keyword index over the same passages
 * Search combines meaning (cosine similarity of embeddings) and keywords (BM25) with
 * reciprocal-rank fusion, so both paraphrased questions and exact names work.
 * Thread-safe; heavy calls run on worker threads.
 */
class SearchIndex {
public:
    SearchIndex();

    /// Splits page texts into overlapping passages.
    static QVector<Chunk> chunkPages(const QStringList &pageTexts);

    QSet<QString> indexedDocuments() const;
    void replaceDocument(const QString &docKey, const QVector<Chunk> &chunks);
    void removeDocument(const QString &docKey);
    /// Drops the whole index (Settings → Rebuild).
    void clear();

    /// Passages without an embedding from [model] (new, or made by another model).
    QVector<std::pair<qint64, QString>> chunksNeedingEmbedding(const QString &model, int limit) const;
    int countChunks() const;
    int countEmbedded(const QString &model) const;
    void storeEmbeddings(const QVector<qint64> &ids, const QVector<QVector<float>> &vectors, const QString &model);

    /// Best passages for a query. [queryVector] may be empty (keyword-only). [allowedDocs] empty = all.
    QVector<SearchHit> search(const QString &query, const QVector<float> &queryVector, const QString &model,
                              const QSet<QString> &allowedDocs, int limit) const;

    bool hasKeywordIndex() const { return m_fts; }

private:
    void loadVectors(const QString &model) const;

    bool m_fts = false;
    mutable QMutex m_cacheLock;
    mutable bool m_cacheValid = false;
    mutable QString m_cacheModel;
    mutable int m_dim = 0;
    mutable QVector<qint64> m_ids;
    mutable QVector<QString> m_docs;
    mutable QVector<float> m_vectors; ///< m_ids.size() × m_dim, L2-normalised
};

/**
 * Keeps the index up to date in the background: reads new PDFs (keyword search works right away),
 * then asks Ollama to embed passages for meaning-based search.
 */
class Indexer : public QObject {
    Q_OBJECT
public:
    Indexer(DocumentStore *docs, SearchIndex *index, QObject *parent = nullptr);

    /// Runs now (GUI thread). A run already in progress is repeated once it finishes.
    void start();
    /// Runs in a few seconds, coalescing bursts of library changes.
    void requestSoon();
    /// Asks a running pass to stop at the next passage batch.
    void stop() { m_stop = true; }
    bool isRunning() const { return m_running; }
    QString status() const { return m_status; }

signals:
    void statusChanged(const QString &text, bool busy);
    /// New passages or embeddings are searchable.
    void indexChanged();

private:
    void run(); // worker thread
    void report(const QString &text, bool busy);

    DocumentStore *m_docs;
    SearchIndex *m_index;
    QTimer m_debounce;
    std::atomic_bool m_running = false;
    std::atomic_bool m_again = false;
    std::atomic_bool m_stop = false;
    QString m_status;
};
