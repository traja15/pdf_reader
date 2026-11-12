#include "net/Translator.h"

#include "core/Database.h"
#include "net/Http.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlQuery>
#include <QUrl>
#include <QUrlQuery>

namespace {

QString google(const QString &text, const QString &target)
{
    QUrl url("https://translate.googleapis.com/translate_a/single");
    QUrlQuery q;
    q.addQueryItem("client", "gtx");
    q.addQueryItem("sl", "auto");
    q.addQueryItem("tl", target);
    q.addQueryItem("dt", "t");
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, Http::kUserAgent);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded;charset=UTF-8");
    const HttpResponse r = Http::send(req, "POST", "q=" + QUrl::toPercentEncoding(text));
    Http::check(r, "Google Translate");
    // [[["translated","source",…],["…","…",…]],…]
    QString out;
    for (const QJsonValue &part : QJsonDocument::fromJson(r.body).array().at(0).toArray())
        out += part.toArray().at(0).toString();
    return out.trimmed();
}

QString myMemory(const QString &text, const QString &target)
{
    QUrl url("https://api.mymemory.translated.net/get");
    QUrlQuery q;
    q.addQueryItem("q", text.left(480)); // free tier limit per request
    q.addQueryItem("langpair", "en|" + target);
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, Http::kUserAgent);
    const HttpResponse r = Http::send(req, "GET");
    Http::check(r, "MyMemory");
    return QJsonDocument::fromJson(r.body).object()["responseData"].toObject()["translatedText"].toString().trimmed();
}

} // namespace

namespace Translator {

Result translate(const QString &raw, const QString &target)
{
    Result result;
    const QString text = raw.simplified();
    if (text.isEmpty())
        return result;
    {
        QSqlQuery q(Database::db());
        q.prepare("SELECT translation FROM translation_cache WHERE source = ? AND target = ?");
        q.addBindValue(text);
        q.addBindValue(target);
        if (q.exec() && q.next()) {
            result.text = q.value(0).toString();
            result.cached = true;
            return result;
        }
    }
    QString firstError;
    for (auto service : {google, myMemory}) {
        try {
            result.text = service(text, target);
        } catch (const HttpError &e) {
            if (firstError.isEmpty())
                firstError = e.status == 0 ? QStringLiteral("You seem to be offline.") : QString::fromStdString(e.what());
        }
        if (!result.text.isEmpty())
            break;
    }
    if (result.text.isEmpty()) {
        result.error = firstError.isEmpty() ? QStringLiteral("No translation came back.") : firstError;
        return result;
    }
    QSqlQuery q(Database::db());
    q.prepare("INSERT OR REPLACE INTO translation_cache (source, target, translation, fetchedAt) VALUES (?,?,?,?)");
    q.addBindValue(text);
    q.addBindValue(target);
    q.addBindValue(result.text);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.exec();
    return result;
}

} // namespace Translator
