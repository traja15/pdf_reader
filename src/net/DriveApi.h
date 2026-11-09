#pragma once

#include <QString>
#include <QVector>

struct DriveFile {
    QString id;
    QString name;
};

/// Minimal Google Drive v3 client limited to the app's private appDataFolder (the same one the
/// Android app uses). Blocking: call from a worker thread. Throws HttpError on failure.
namespace DriveApi {
QVector<DriveFile> listAppData(const QString &token);
QByteArray readFile(const QString &token, const QString &fileId);
/// Streams a file to [destPath].
void downloadTo(const QString &token, const QString &fileId, const QString &destPath);
/// Creates or overwrites a small JSON file in appDataFolder.
void writeJson(const QString &token, const QString &name, const QByteArray &json, const QString &existingId);
/// Uploads a (possibly large) file with a resumable session. Returns the new file id.
QString uploadFile(const QString &token, const QString &name, const QString &path, const QString &mimeType);
void remove(const QString &token, const QString &fileId);
} // namespace DriveApi
