#include "core/Database.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <stdexcept>

QString Database::s_path;

void Database::init(const QString &path)
{
    s_path = path;
    QSqlQuery q(db());
    const char *schema[] = {
        "PRAGMA journal_mode=WAL",
        R"(CREATE TABLE IF NOT EXISTS documents (
            docKey TEXT PRIMARY KEY NOT NULL, title TEXT NOT NULL, localPath TEXT, driveFileId TEXT, folderId TEXT,
            pageCount INTEGER NOT NULL, lastPage INTEGER NOT NULL, lastOffset REAL NOT NULL DEFAULT 0,
            lastOpenedAt INTEGER NOT NULL, updatedAt INTEGER NOT NULL, deleted INTEGER NOT NULL DEFAULT 0))",
        R"(CREATE TABLE IF NOT EXISTS page_ink (
            docKey TEXT NOT NULL, page INTEGER NOT NULL, strokesJson TEXT NOT NULL, updatedAt INTEGER NOT NULL,
            PRIMARY KEY(docKey, page)))",
        R"(CREATE TABLE IF NOT EXISTS vocab (
            word TEXT PRIMARY KEY NOT NULL, phonetic TEXT, partOfSpeech TEXT, definition TEXT NOT NULL,
            tamil TEXT, contextSentence TEXT, sourceDocKey TEXT, sourceTitle TEXT, sourcePage INTEGER,
            addedAt INTEGER NOT NULL, updatedAt INTEGER NOT NULL, deleted INTEGER NOT NULL DEFAULT 0))",
        R"(CREATE TABLE IF NOT EXISTS folders (
            id TEXT PRIMARY KEY NOT NULL, name TEXT NOT NULL, parentId TEXT,
            updatedAt INTEGER NOT NULL, deleted INTEGER NOT NULL DEFAULT 0))",
        R"(CREATE TABLE IF NOT EXISTS definition_cache (
            word TEXT PRIMARY KEY NOT NULL, json TEXT NOT NULL, fetchedAt INTEGER NOT NULL))",
        R"(CREATE TABLE IF NOT EXISTS tamil_cache (
            word TEXT PRIMARY KEY NOT NULL, json TEXT NOT NULL, fetchedAt INTEGER NOT NULL))",
        R"(CREATE TABLE IF NOT EXISTS translation_cache (
            source TEXT NOT NULL, target TEXT NOT NULL, translation TEXT NOT NULL, fetchedAt INTEGER NOT NULL,
            PRIMARY KEY (source, target)))",
    };
    for (const char *sql : schema) {
        if (!q.exec(QString::fromUtf8(sql)))
            throw std::runtime_error(("Database setup failed: " + q.lastError().text()).toStdString());
    }

    // Upgrades for databases created by earlier versions.
    bool hasFolderId = false;
    q.exec("PRAGMA table_info(documents)");
    while (q.next())
        hasFolderId |= q.value(1).toString() == "folderId";
    if (!hasFolderId && !q.exec("ALTER TABLE documents ADD COLUMN folderId TEXT"))
        throw std::runtime_error(("Database upgrade failed: " + q.lastError().text()).toStdString());
}

QSqlDatabase Database::db()
{
    const QString name = QStringLiteral("penreader-%1").arg(quintptr(QThread::currentThreadId()));
    if (QSqlDatabase::contains(name))
        return QSqlDatabase::database(name);

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", name);
    db.setDatabaseName(s_path);
    db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=5000");
    if (!db.open())
        throw std::runtime_error(("Can't open database: " + db.lastError().text()).toStdString());
    return db;
}
