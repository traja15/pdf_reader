#pragma once

#include <QDateTime>
#include <QMutex>
#include <QObject>
#include <QTimer>
#include <atomic>

class GoogleAuth;
class DocumentStore;
class VocabStore;

/**
 * Two-way sync with the Drive appDataFolder, in the same format as the Android app:
 *   documents.json      list of documents (title, folder, reading position, deletion tombstones)
 *   folders.json        catalogue folders (name, parent, deletion tombstones)
 *   pdf_<docKey>.pdf    the PDF itself, uploaded the first time it's opened on any device
 *   ink_<docKey>.json   pages of strokes for one document
 *   vocab.json          every saved word, including tombstones for deletions
 * Merging is last-write-wins per document, per word and per page.
 */
class SyncManager : public QObject {
    Q_OBJECT
public:
    enum class Status { Idle, Running, Done, Failed, SignedOut };
    Q_ENUM(Status)

    SyncManager(GoogleAuth *auth, DocumentStore *docs, VocabStore *vocab, QObject *parent = nullptr);

    /// Starts a background sync now (GUI thread). Does nothing if signed out.
    void syncNow();
    /// Schedules a sync a few seconds from now, coalescing bursts of changes.
    void requestSync();

    Status status() const { return m_status; }
    QString message() const { return m_message; }
    QDateTime lastSync() const { return m_lastSync; }

signals:
    void statusChanged();
    /// Emitted on the GUI thread after every sync attempt.
    void finished(bool ok);

private:
    void run();                 // worker thread
    void syncBlocking();        // worker thread; throws on failure
    void setStatus(Status s, const QString &message = {});

    GoogleAuth *m_auth;
    DocumentStore *m_docs;
    VocabStore *m_vocab;
    QTimer m_debounce;
    QMutex m_runLock;
    std::atomic_bool m_running = false;
    std::atomic_bool m_again = false;

    Status m_status = Status::Idle;
    QString m_message;
    QDateTime m_lastSync;
};
