#include "net/DriveApi.h"

#include "net/Http.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrlQuery>

namespace {

const QString kFiles = "https://www.googleapis.com/drive/v3/files";
const QString kUpload = "https://www.googleapis.com/upload/drive/v3/files";

QNetworkRequest authed(const QString &url, const QString &token)
{
    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    return req;
}

QByteArray metadata(const QString &name)
{
    return QJsonDocument(QJsonObject{{"name", name}, {"parents", QJsonArray{"appDataFolder"}}}).toJson(QJsonDocument::Compact);
}

} // namespace

namespace DriveApi {

QVector<DriveFile> listAppData(const QString &token)
{
    QVector<DriveFile> out;
    QString pageToken;
    do {
        QUrl url(kFiles);
        QUrlQuery q;
        q.addQueryItem("spaces", "appDataFolder");
        q.addQueryItem("fields", "nextPageToken,files(id,name)");
        q.addQueryItem("pageSize", "1000");
        if (!pageToken.isEmpty())
            q.addQueryItem("pageToken", pageToken);
        url.setQuery(q);
        const HttpResponse r = Http::check(Http::send(authed(url.toString(), token), "GET"), "Listing cloud files");
        const QJsonObject o = QJsonDocument::fromJson(r.body).object();
        for (const QJsonValue &f : o["files"].toArray())
            out.append({f.toObject()["id"].toString(), f.toObject()["name"].toString()});
        pageToken = o["nextPageToken"].toString();
    } while (!pageToken.isEmpty());
    return out;
}

QByteArray readFile(const QString &token, const QString &fileId)
{
    return Http::check(Http::send(authed(kFiles + "/" + fileId + "?alt=media", token), "GET"), "Reading a cloud file").body;
}

void downloadTo(const QString &token, const QString &fileId, const QString &destPath)
{
    QSaveFile out(destPath);
    if (!out.open(QIODevice::WriteOnly))
        throw HttpError(0, "Can't write " + destPath);
    Http::check(Http::download(authed(kFiles + "/" + fileId + "?alt=media", token), &out), "Downloading the PDF");
    if (!out.commit())
        throw HttpError(0, "Couldn't save the downloaded PDF");
}

void writeJson(const QString &token, const QString &name, const QByteArray &json, const QString &existingId)
{
    if (!existingId.isEmpty()) {
        QNetworkRequest req = authed(kUpload + "/" + existingId + "?uploadType=media", token);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=utf-8");
        Http::check(Http::send(req, "PATCH", json), "Updating " + name);
        return;
    }
    const QByteArray boundary = "penreader-boundary-7f3a";
    QByteArray body;
    body += "--" + boundary + "\r\nContent-Type: application/json; charset=utf-8\r\n\r\n" + metadata(name) + "\r\n";
    body += "--" + boundary + "\r\nContent-Type: application/json; charset=utf-8\r\n\r\n" + json + "\r\n";
    body += "--" + boundary + "--\r\n";
    QNetworkRequest req = authed(kUpload + "?uploadType=multipart", token);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "multipart/related; boundary=" + boundary);
    Http::check(Http::send(req, "POST", body), "Creating " + name);
}

QString uploadFile(const QString &token, const QString &name, const QString &path, const QString &mimeType)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        throw HttpError(0, "Can't read " + path);

    QNetworkRequest start = authed(kUpload + "?uploadType=resumable&fields=id", token);
    start.setHeader(QNetworkRequest::ContentTypeHeader, "application/json; charset=utf-8");
    start.setRawHeader("X-Upload-Content-Type", mimeType.toUtf8());
    start.setRawHeader("X-Upload-Content-Length", QByteArray::number(file.size()));
    const HttpResponse session = Http::check(Http::send(start, "POST", metadata(name)), "Starting upload");
    if (session.location.isEmpty())
        throw HttpError(0, "Drive didn't start an upload session");

    QNetworkRequest put{QUrl::fromEncoded(session.location)};
    put.setHeader(QNetworkRequest::ContentTypeHeader, mimeType);
    put.setHeader(QNetworkRequest::ContentLengthHeader, file.size());
    const HttpResponse done = Http::check(Http::send(put, "PUT", &file), "Uploading " + name);
    return QJsonDocument::fromJson(done.body).object()["id"].toString();
}

void remove(const QString &token, const QString &fileId)
{
    const HttpResponse r = Http::send(authed(kFiles + "/" + fileId, token), "DELETE");
    if (r.status != 404) // already gone is fine
        Http::check(r, "Deleting a cloud file");
}

} // namespace DriveApi
