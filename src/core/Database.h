#pragma once

#include <QSqlDatabase>
#include <QString>

/// SQLite storage. Each thread gets its own connection (Qt requires that), all on the same file.
class Database {
public:
    /// Creates the schema if needed. Call once on the GUI thread at startup.
    static void init(const QString &path);
    /// The connection for the calling thread.
    static QSqlDatabase db();

private:
    static QString s_path;
};
