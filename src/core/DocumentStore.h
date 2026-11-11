#pragma once

#include "core/Models.h"

#include <QMap>
#include <QObject>
#include <optional>

/// Library, reading progress and ink, persisted in SQLite. Safe to call from any thread.
class DocumentStore : public QObject {
    Q_OBJECT
public:
    explicit DocumentStore(QString docsDir, QObject *parent = nullptr);

    /// Non-deleted documents, most recently opened first.
    QVector<Document> library() const;
    /// Every document including tombstones (for sync).
    QVector<Document> all() const;
    std::optional<Document> get(const QString &docKey) const;
    void upsert(const Document &doc);

    /// Copies a PDF into app storage (keyed by content hash) and records it. A new PDF goes into
    /// [folderId]; one already in the library keeps its folder. Throws on failure.
    Document importFile(const QString &path, const QString &driveFileId = {}, const QString &folderId = {});
    void markOpened(const QString &docKey);
    /// Renames a PDF in the library (synced to all devices; the file itself is unchanged).
    void renameDocument(const QString &docKey, const QString &title);
    void saveProgress(const QString &docKey, int page, double offset);
    /// Frees space on this device; the PDF and notes stay in the cloud.
    void removeFromDevice(const QString &docKey);
    /// Deletes the PDF and its notes on every device (propagated as a tombstone).
    void deleteEverywhere(const QString &docKey);

    QMap<int, QVector<Stroke>> loadInk(const QString &docKey) const;
    void saveInk(const QString &docKey, int page, const QVector<Stroke> &strokes);
    QVector<PageInkRow> inkRows(const QString &docKey) const;
    void upsertInkRow(const QString &docKey, const PageInkRow &row);
    void deleteInk(const QString &docKey);

    QString docsDir() const { return m_docsDir; }

    // ---- Catalogue folders ----
    /// Live folders, sorted by name.
    QVector<Folder> folders() const;
    QVector<Folder> allFolders() const; ///< including tombstones (for sync)
    std::optional<Folder> folder(const QString &id) const;
    /// True if [id] names a live folder (documents in missing/deleted folders count as Unfiled).
    bool folderExists(const QString &id) const;
    /// "Books / Physics"
    QString folderPath(const QString &id) const;
    Folder createFolder(const QString &name, const QString &parentId = {});
    void renameFolder(const QString &id, const QString &name);
    /// Moves a folder under another (or to the top level). Refuses to move it inside itself.
    bool moveFolder(const QString &id, const QString &newParentId);
    /// Deletes a folder; its PDFs and subfolders move up to its parent.
    void deleteFolder(const QString &id);
    void moveDocument(const QString &docKey, const QString &folderId);
    void upsertFolder(const Folder &f); ///< as-is (sync)

signals:
    /// Emitted after any change to the library list (may be emitted from a worker thread).
    void libraryChanged();
    /// Emitted after changes other devices should see (new PDFs, deletions).
    void needsSync();

private:
    QString m_docsDir;
};
