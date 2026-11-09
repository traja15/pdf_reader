#include "net/SyncManager.h"

#include "core/DocumentStore.h"
#include "core/VocabStore.h"
#include "net/DriveApi.h"
#include "net/GoogleAuth.h"
#include "net/Http.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtConcurrent>
#include <cmath>

namespace {

const QString kDocuments = "documents.json";
const QString kVocab = "vocab.json";
const QString kFolders = "folders.json";
QString pdfName(const QString &docKey) { return "pdf_" + docKey + ".pdf"; }
QString inkName(const QString &docKey) { return "ink_" + docKey + ".json"; }
qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }

// Android's kotlinx.serialization omits fields equal to their defaults, so every read must
// tolerate missing keys; writes include everything (Android ignores nothing it knows).

QJsonObject docToJson(const Document &d)
{
    QJsonObject o{{"docKey", d.docKey}, {"title", d.title}, {"pageCount", d.pageCount}, {"lastPage", d.lastPage},
                  {"lastOffset", d.lastOffset}, {"updatedAt", double(d.updatedAt)}, {"deleted", d.deleted}};
    if (!d.driveFileId.isEmpty())
        o["driveFileId"] = d.driveFileId;
    if (!d.folderId.isEmpty())
        o["folderId"] = d.folderId;
    return o;
}

Document docFromJson(const QJsonObject &o)
{
    Document d;
    d.docKey = o["docKey"].toString();
    d.title = o["title"].toString();
    d.driveFileId = o["driveFileId"].toString();
    d.folderId = o["folderId"].toString();
    d.pageCount = o["pageCount"].toInt();
    d.lastPage = o["lastPage"].toInt();
    d.lastOffset = o["lastOffset"].toDouble();
    d.updatedAt = qint64(o["updatedAt"].toDouble());
    d.deleted = o["deleted"].toBool();
    return d;
}

QJsonObject wordToJson(const VocabWord &w)
{
    QJsonObject o{{"word", w.word}, {"definition", w.definition}, {"addedAt", double(w.addedAt)},
                  {"updatedAt", double(w.updatedAt)}, {"deleted", w.deleted}};
    auto opt = [&](const char *key, const QString &v) { if (!v.isEmpty()) o[key] = v; };
    opt("phonetic", w.phonetic);
    opt("partOfSpeech", w.partOfSpeech);
    opt("tamil", w.tamil);
    opt("contextSentence", w.contextSentence);
    opt("sourceDocKey", w.sourceDocKey);
    opt("sourceTitle", w.sourceTitle);
    if (w.sourcePage >= 0)
        o["sourcePage"] = w.sourcePage;
    return o;
}

VocabWord wordFromJson(const QJsonObject &o)
{
    VocabWord w;
    w.word = o["word"].toString();
    w.phonetic = o["phonetic"].toString();
    w.partOfSpeech = o["partOfSpeech"].toString();
    w.definition = o["definition"].toString();
    w.tamil = o["tamil"].toString();
    w.contextSentence = o["contextSentence"].toString();
    w.sourceDocKey = o["sourceDocKey"].toString();
    w.sourceTitle = o["sourceTitle"].toString();
    w.sourcePage = o["sourcePage"].isDouble() ? o["sourcePage"].toInt() : -1;
    w.addedAt = qint64(o["addedAt"].toDouble());
    w.updatedAt = qint64(o["updatedAt"].toDouble());
    w.deleted = o["deleted"].toBool();
    return w;
}

QJsonArray readArray(const QString &token, const QHash<QString, DriveFile> &files, const QString &name)
{
    const auto it = files.find(name);
    if (it == files.end())
        return {};
    return QJsonDocument::fromJson(DriveApi::readFile(token, it->id)).array();
}

void writeArray(const QString &token, const QHash<QString, DriveFile> &files, const QString &name, const QJsonArray &arr)
{
    const auto it = files.find(name);
    DriveApi::writeJson(token, name, QJsonDocument(arr).toJson(QJsonDocument::Compact),
                        it == files.end() ? QString() : it->id);
}

bool sameDoc(const Document &a, const Document &b)
{
    return a.title == b.title && a.driveFileId == b.driveFileId && a.folderId == b.folderId && a.pageCount == b.pageCount &&
           a.lastPage == b.lastPage && std::abs(a.lastOffset - b.lastOffset) < 1e-4 &&
           a.updatedAt == b.updatedAt && a.deleted == b.deleted;
}

} // namespace

SyncManager::SyncManager(GoogleAuth *auth, DocumentStore *docs, VocabStore *vocab, QObject *parent)
    : QObject(parent), m_auth(auth), m_docs(docs), m_vocab(vocab)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(3000);
    connect(&m_debounce, &QTimer::timeout, this, &SyncManager::syncNow);
    if (!m_auth->isSignedIn())
        m_status = Status::SignedOut;
}

void SyncManager::requestSync()
{
    if (m_auth->isSignedIn())
        m_debounce.start();
}

void SyncManager::syncNow()
{
    if (!m_auth->isSignedIn()) {
        setStatus(Status::SignedOut);
        return;
    }
    if (m_running.exchange(true)) {
        m_again = true; // run once more after the current sync, to pick up new changes
        return;
    }
    setStatus(Status::Running);
    (void)QtConcurrent::run([this] { run(); });
}

void SyncManager::run()
{
    bool ok = false;
    QString error;
    Status result = Status::Failed;
    do {
        m_again = false;
        try {
            QMutexLocker lock(&m_runLock);
            syncBlocking();
            ok = true;
            result = Status::Done;
        } catch (const NeedsSignIn &) {
            result = Status::SignedOut;
            error = "Sign in with Google to sync";
            break;
        } catch (const HttpError &e) {
            error = e.status == 0 ? "Offline — will sync when you're back online" : QString::fromStdString(e.what());
        } catch (const std::exception &e) {
            error = QString::fromStdString(e.what());
        }
    } while (ok && m_again);

    m_running = false;
    QMetaObject::invokeMethod(this, [this, ok, result, error] {
        if (ok)
            m_lastSync = QDateTime::currentDateTime();
        setStatus(result, error);
        emit finished(ok);
    }, Qt::QueuedConnection);
}

void SyncManager::setStatus(Status s, const QString &message)
{
    m_status = s;
    m_message = message;
    emit statusChanged();
}

void SyncManager::syncBlocking()
{
    const QString token = m_auth->accessToken();
    QHash<QString, DriveFile> files;
    for (const DriveFile &f : DriveApi::listAppData(token))
        files.insert(f.name, f);

    // ---- Folders (catalogue): last write wins per folder ----
    QHash<QString, Folder> remoteFolders;
    for (const QJsonValue &v : readArray(token, files, kFolders)) {
        const QJsonObject o = v.toObject();
        const Folder f{o["id"].toString(), o["name"].toString(), o["parentId"].toString(),
                       qint64(o["updatedAt"].toDouble()), o["deleted"].toBool()};
        if (!f.id.isEmpty()) remoteFolders.insert(f.id, f);
    }
    QHash<QString, Folder> localFolders;
    for (const Folder &f : m_docs->allFolders())
        localFolders.insert(f.id, f);
    for (const Folder &r : std::as_const(remoteFolders)) {
        const auto l = localFolders.find(r.id);
        if (l == localFolders.end() || l->updatedAt < r.updatedAt)
            m_docs->upsertFolder(r);
    }
    QJsonArray mergedFolders;
    bool foldersChanged = false;
    const QVector<Folder> allFolders = m_docs->allFolders();
    for (const Folder &f : allFolders) {
        QJsonObject o{{"id", f.id}, {"name", f.name}, {"updatedAt", double(f.updatedAt)}, {"deleted", f.deleted}};
        if (!f.parentId.isEmpty()) o["parentId"] = f.parentId;
        mergedFolders.append(o);
        const auto r = remoteFolders.find(f.id);
        if (r == remoteFolders.end() || r->updatedAt != f.updatedAt) foldersChanged = true;
    }
    if (foldersChanged || allFolders.size() != remoteFolders.size())
        writeArray(token, files, kFolders, mergedFolders);

    // ---- Documents: merge metadata, applying deletions from other devices ----
    QHash<QString, Document> remoteDocs;
    for (const QJsonValue &v : readArray(token, files, kDocuments)) {
        const Document d = docFromJson(v.toObject());
        if (!d.docKey.isEmpty())
            remoteDocs.insert(d.docKey, d);
    }
    QHash<QString, Document> local;
    for (const Document &d : m_docs->all())
        local.insert(d.docKey, d);

    for (const Document &r : std::as_const(remoteDocs)) {
        const auto l = local.find(r.docKey);
        if (l == local.end()) {
            Document d = r; // opened on another device; downloaded here when first opened
            d.localPath.clear();
            d.lastOpenedAt = 0;
            m_docs->upsert(d);
        } else if (r.updatedAt > l->updatedAt) {
            Document d = *l;
            if (r.deleted && !l->deleted) {
                if (!d.localPath.isEmpty()) QFile::remove(d.localPath);
                m_docs->deleteInk(d.docKey);
                d.localPath.clear();
            }
            d.title = r.title;
            d.folderId = r.folderId;
            if (!r.driveFileId.isEmpty()) d.driveFileId = r.driveFileId;
            d.lastPage = r.lastPage;
            d.lastOffset = r.lastOffset;
            d.updatedAt = r.updatedAt;
            d.deleted = r.deleted;
            m_docs->upsert(d);
        }
    }

    // ---- PDFs: upload new ones, remove cloud copies of deleted ones ----
    for (Document d : m_docs->all()) {
        if (d.deleted) {
            for (const QString &name : {pdfName(d.docKey), inkName(d.docKey)})
                if (files.contains(name)) DriveApi::remove(token, files[name].id);
            continue;
        }
        QString cloudId = files.value(pdfName(d.docKey)).id;
        if (cloudId.isEmpty() && d.onDevice())
            cloudId = DriveApi::uploadFile(token, pdfName(d.docKey), d.localPath, "application/pdf");
        if (!cloudId.isEmpty() && cloudId != d.driveFileId) {
            d.driveFileId = cloudId;
            d.updatedAt = now();
            m_docs->upsert(d);
        }
    }

    // ---- documents.json ----
    QJsonArray mergedDocs;
    bool docsChanged = false;
    const QVector<Document> allDocs = m_docs->all();
    for (const Document &d : allDocs) {
        mergedDocs.append(docToJson(d));
        const auto r = remoteDocs.find(d.docKey);
        if (r == remoteDocs.end() || !sameDoc(*r, d)) docsChanged = true;
    }
    if (docsChanged || allDocs.size() != remoteDocs.size())
        writeArray(token, files, kDocuments, mergedDocs);

    // ---- vocab.json ----
    QHash<QString, VocabWord> remoteWords;
    for (const QJsonValue &v : readArray(token, files, kVocab)) {
        const VocabWord w = wordFromJson(v.toObject());
        if (!w.word.isEmpty()) remoteWords.insert(w.word, w);
    }
    QHash<QString, VocabWord> localWords;
    for (const VocabWord &w : m_vocab->allIncludingDeleted())
        localWords.insert(w.word, w);
    for (const VocabWord &r : std::as_const(remoteWords)) {
        const auto l = localWords.find(r.word);
        if (l == localWords.end() || l->updatedAt < r.updatedAt)
            m_vocab->upsertRaw(r);
    }
    QJsonArray mergedWords;
    bool wordsChanged = false;
    const QVector<VocabWord> allWords = m_vocab->allIncludingDeleted();
    for (const VocabWord &w : allWords) {
        mergedWords.append(wordToJson(w));
        const auto r = remoteWords.find(w.word);
        if (r == remoteWords.end() || r->updatedAt != w.updatedAt || r->deleted != w.deleted) wordsChanged = true;
    }
    if (wordsChanged || allWords.size() != remoteWords.size())
        writeArray(token, files, kVocab, mergedWords);

    // ---- ink_<docKey>.json per document ----
    for (const Document &d : allDocs) {
        if (d.deleted)
            continue;
        QHash<int, PageInkRow> remotePages;
        for (const QJsonValue &v : readArray(token, files, inkName(d.docKey))) {
            const QJsonObject o = v.toObject();
            remotePages.insert(o["page"].toInt(), {o["page"].toInt(), o["strokesJson"].toString(), qint64(o["updatedAt"].toDouble())});
        }
        QHash<int, PageInkRow> localPages;
        for (const PageInkRow &row : m_docs->inkRows(d.docKey))
            localPages.insert(row.page, row);
        for (const PageInkRow &r : std::as_const(remotePages)) {
            const auto l = localPages.find(r.page);
            if (l == localPages.end() || l->updatedAt < r.updatedAt)
                m_docs->upsertInkRow(d.docKey, r);
        }
        QJsonArray merged;
        bool changed = false;
        const QVector<PageInkRow> rows = m_docs->inkRows(d.docKey);
        for (const PageInkRow &row : rows) {
            merged.append(QJsonObject{{"page", row.page}, {"strokesJson", row.strokesJson}, {"updatedAt", double(row.updatedAt)}});
            const auto r = remotePages.find(row.page);
            if (r == remotePages.end() || r->updatedAt != row.updatedAt) changed = true;
        }
        if (!rows.isEmpty() && (changed || rows.size() != remotePages.size()))
            writeArray(token, files, inkName(d.docKey), merged);
    }
}
