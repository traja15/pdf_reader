#include "core/DocumentStore.h"

#include "core/Database.h"
#include "core/InkCodec.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QPdfDocument>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <cmath>
#include <stdexcept>

namespace {

qint64 now() { return QDateTime::currentMSecsSinceEpoch(); }

Document readDoc(const QSqlQuery &q)
{
    Document d;
    d.docKey = q.value("docKey").toString();
    d.title = q.value("title").toString();
    d.localPath = q.value("localPath").toString();
    d.driveFileId = q.value("driveFileId").toString();
    d.folderId = q.value("folderId").toString();
    d.pageCount = q.value("pageCount").toInt();
    d.lastPage = q.value("lastPage").toInt();
    d.lastOffset = q.value("lastOffset").toDouble();
    d.lastOpenedAt = q.value("lastOpenedAt").toLongLong();
    d.updatedAt = q.value("updatedAt").toLongLong();
    d.deleted = q.value("deleted").toBool();
    return d;
}

QVariant orNull(const QString &s) { return s.isEmpty() ? QVariant() : QVariant(s); }

} // namespace

DocumentStore::DocumentStore(QString docsDir, QObject *parent)
    : QObject(parent), m_docsDir(std::move(docsDir))
{
    QDir().mkpath(m_docsDir);
}

QVector<Document> DocumentStore::library() const
{
    QVector<Document> out;
    QSqlQuery q(Database::db());
    q.exec("SELECT * FROM documents WHERE deleted = 0 ORDER BY lastOpenedAt DESC");
    while (q.next())
        out.append(readDoc(q));
    return out;
}

QVector<Document> DocumentStore::all() const
{
    QVector<Document> out;
    QSqlQuery q(Database::db());
    q.exec("SELECT * FROM documents");
    while (q.next())
        out.append(readDoc(q));
    return out;
}

std::optional<Document> DocumentStore::get(const QString &docKey) const
{
    QSqlQuery q(Database::db());
    q.prepare("SELECT * FROM documents WHERE docKey = ?");
    q.addBindValue(docKey);
    if (q.exec() && q.next())
        return readDoc(q);
    return std::nullopt;
}

void DocumentStore::upsert(const Document &d)
{
    QSqlQuery q(Database::db());
    q.prepare("INSERT OR REPLACE INTO documents (docKey, title, localPath, driveFileId, folderId, pageCount, lastPage, "
              "lastOffset, lastOpenedAt, updatedAt, deleted) VALUES (?,?,?,?,?,?,?,?,?,?,?)");
    q.addBindValue(d.docKey);
    q.addBindValue(d.title);
    q.addBindValue(orNull(d.localPath));
    q.addBindValue(orNull(d.driveFileId));
    q.addBindValue(orNull(d.folderId));
    q.addBindValue(d.pageCount);
    q.addBindValue(d.lastPage);
    q.addBindValue(d.lastOffset);
    q.addBindValue(d.lastOpenedAt);
    q.addBindValue(d.updatedAt);
    q.addBindValue(d.deleted);
    q.exec();
    emit libraryChanged();
}

Document DocumentStore::importFile(const QString &path, const QString &driveFileId, const QString &folderId)
{
    QFile src(path);
    if (!src.open(QIODevice::ReadOnly))
        throw std::runtime_error(("Can't open " + path).toStdString());

    // Copy to a temp file while hashing, then move into place under the content hash.
    const QString tmpPath = m_docsDir + "/import-" + QString::number(now()) + ".tmp";
    QFile tmp(tmpPath);
    if (!tmp.open(QIODevice::WriteOnly))
        throw std::runtime_error("Can't write to the app's document folder");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!src.atEnd()) {
        const QByteArray chunk = src.read(1 << 16);
        hash.addData(chunk);
        tmp.write(chunk);
    }
    tmp.close();

    const QString docKey = QString::fromLatin1(hash.result().toHex()).left(32);
    const QString target = m_docsDir + "/" + docKey + ".pdf";
    if (QFile::exists(target))
        QFile::remove(tmpPath);
    else if (!QFile::rename(tmpPath, target))
        throw std::runtime_error("Couldn't store the PDF");

    QPdfDocument pdf;
    if (pdf.load(target) != QPdfDocument::Error::None) {
        QFile::remove(target);
        throw std::runtime_error("That file isn't a readable PDF");
    }

    const auto existing = get(docKey);
    Document d;
    d.docKey = docKey;
    d.title = existing ? existing->title : QFileInfo(path).completeBaseName();
    d.localPath = target;
    d.driveFileId = !driveFileId.isEmpty() ? driveFileId : (existing ? existing->driveFileId : QString());
    d.folderId = existing && !existing->deleted ? existing->folderId : folderId;
    d.pageCount = pdf.pageCount();
    d.lastPage = existing ? existing->lastPage : 0;
    d.lastOffset = existing ? existing->lastOffset : 0;
    d.lastOpenedAt = now();
    d.updatedAt = now();
    d.deleted = false;
    upsert(d);
    if (!existing || existing->deleted)
        emit needsSync();
    return d;
}

void DocumentStore::markOpened(const QString &docKey)
{
    if (auto d = get(docKey)) {
        d->lastOpenedAt = now();
        upsert(*d);
    }
}

void DocumentStore::saveProgress(const QString &docKey, int page, double offset)
{
    auto d = get(docKey);
    if (!d || (d->lastPage == page && std::abs(d->lastOffset - offset) <= 0.01))
        return;
    d->lastPage = page;
    d->lastOffset = offset;
    d->updatedAt = now();
    upsert(*d);
}

void DocumentStore::removeFromDevice(const QString &docKey)
{
    auto d = get(docKey);
    if (!d)
        return;
    if (!d->localPath.isEmpty())
        QFile::remove(d->localPath);
    d->localPath.clear();
    upsert(*d);
}

void DocumentStore::deleteEverywhere(const QString &docKey)
{
    auto d = get(docKey);
    if (!d)
        return;
    if (!d->localPath.isEmpty())
        QFile::remove(d->localPath);
    deleteInk(docKey);
    d->localPath.clear();
    d->deleted = true;
    d->updatedAt = now();
    upsert(*d);
    emit needsSync();
}

QMap<int, QVector<Stroke>> DocumentStore::loadInk(const QString &docKey) const
{
    QMap<int, QVector<Stroke>> out;
    for (const PageInkRow &row : inkRows(docKey))
        out.insert(row.page, InkCodec::decode(row.strokesJson));
    return out;
}

void DocumentStore::saveInk(const QString &docKey, int page, const QVector<Stroke> &strokes)
{
    upsertInkRow(docKey, {page, InkCodec::encode(strokes), now()});
}

QVector<PageInkRow> DocumentStore::inkRows(const QString &docKey) const
{
    QVector<PageInkRow> out;
    QSqlQuery q(Database::db());
    q.prepare("SELECT page, strokesJson, updatedAt FROM page_ink WHERE docKey = ?");
    q.addBindValue(docKey);
    q.exec();
    while (q.next())
        out.append({q.value(0).toInt(), q.value(1).toString(), q.value(2).toLongLong()});
    return out;
}

void DocumentStore::upsertInkRow(const QString &docKey, const PageInkRow &row)
{
    QSqlQuery q(Database::db());
    q.prepare("INSERT OR REPLACE INTO page_ink (docKey, page, strokesJson, updatedAt) VALUES (?,?,?,?)");
    q.addBindValue(docKey);
    q.addBindValue(row.page);
    q.addBindValue(row.strokesJson);
    q.addBindValue(row.updatedAt);
    q.exec();
}

void DocumentStore::deleteInk(const QString &docKey)
{
    QSqlQuery q(Database::db());
    q.prepare("DELETE FROM page_ink WHERE docKey = ?");
    q.addBindValue(docKey);
    q.exec();
}

// ---------------------------------------------------------------- catalogue folders

namespace {

Folder readFolder(const QSqlQuery &q)
{
    return {q.value("id").toString(), q.value("name").toString(), q.value("parentId").toString(),
            q.value("updatedAt").toLongLong(), q.value("deleted").toBool()};
}

} // namespace

QVector<Folder> DocumentStore::folders() const
{
    QVector<Folder> out;
    QSqlQuery q(Database::db());
    q.exec("SELECT * FROM folders WHERE deleted = 0 ORDER BY name COLLATE NOCASE");
    while (q.next())
        out.append(readFolder(q));
    return out;
}

QVector<Folder> DocumentStore::allFolders() const
{
    QVector<Folder> out;
    QSqlQuery q(Database::db());
    q.exec("SELECT * FROM folders");
    while (q.next())
        out.append(readFolder(q));
    return out;
}

std::optional<Folder> DocumentStore::folder(const QString &id) const
{
    if (id.isEmpty())
        return std::nullopt;
    QSqlQuery q(Database::db());
    q.prepare("SELECT * FROM folders WHERE id = ?");
    q.addBindValue(id);
    if (q.exec() && q.next())
        return readFolder(q);
    return std::nullopt;
}

bool DocumentStore::folderExists(const QString &id) const
{
    const auto f = folder(id);
    return f && !f->deleted;
}

QString DocumentStore::folderPath(const QString &id) const
{
    QStringList parts;
    QString current = id;
    for (int depth = 0; depth < 32 && !current.isEmpty(); ++depth) { // depth guard against bad synced data
        const auto f = folder(current);
        if (!f || f->deleted)
            break;
        parts.prepend(f->name);
        current = f->parentId;
    }
    return parts.join(" / ");
}

void DocumentStore::upsertFolder(const Folder &f)
{
    QSqlQuery q(Database::db());
    q.prepare("INSERT OR REPLACE INTO folders (id, name, parentId, updatedAt, deleted) VALUES (?,?,?,?,?)");
    q.addBindValue(f.id);
    q.addBindValue(f.name);
    q.addBindValue(orNull(f.parentId));
    q.addBindValue(f.updatedAt);
    q.addBindValue(f.deleted);
    q.exec();
    emit libraryChanged();
}

Folder DocumentStore::createFolder(const QString &name, const QString &parentId)
{
    const Folder f{QUuid::createUuid().toString(QUuid::WithoutBraces), name.trimmed(),
                   folderExists(parentId) ? parentId : QString(), now(), false};
    upsertFolder(f);
    emit needsSync();
    return f;
}

void DocumentStore::renameFolder(const QString &id, const QString &name)
{
    auto f = folder(id);
    if (!f || name.trimmed().isEmpty())
        return;
    f->name = name.trimmed();
    f->updatedAt = now();
    upsertFolder(*f);
    emit needsSync();
}

bool DocumentStore::moveFolder(const QString &id, const QString &newParentId)
{
    auto f = folder(id);
    if (!f)
        return false;
    // Refuse to move a folder into itself or one of its own subfolders.
    for (QString p = newParentId; !p.isEmpty();) {
        if (p == id)
            return false;
        const auto parent = folder(p);
        p = parent ? parent->parentId : QString();
    }
    f->parentId = folderExists(newParentId) ? newParentId : QString();
    f->updatedAt = now();
    upsertFolder(*f);
    emit needsSync();
    return true;
}

void DocumentStore::deleteFolder(const QString &id)
{
    auto f = folder(id);
    if (!f)
        return;
    const QString parent = folderExists(f->parentId) ? f->parentId : QString();
    for (Folder child : folders()) {
        if (child.parentId == id) {
            child.parentId = parent;
            child.updatedAt = now();
            upsertFolder(child);
        }
    }
    for (Document d : library()) {
        if (d.folderId == id) {
            d.folderId = parent;
            d.updatedAt = now();
            upsert(d);
        }
    }
    f->deleted = true;
    f->updatedAt = now();
    upsertFolder(*f);
    emit needsSync();
}

void DocumentStore::moveDocument(const QString &docKey, const QString &folderId)
{
    auto d = get(docKey);
    if (!d)
        return;
    d->folderId = folderExists(folderId) ? folderId : QString();
    d->updatedAt = now();
    upsert(*d);
    emit needsSync();
}
