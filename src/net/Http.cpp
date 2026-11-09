#include "net/Http.h"

#include <QEventLoop>
#include <QIODevice>
#include <QNetworkAccessManager>
#include <QNetworkReply>

namespace {

QNetworkAccessManager &manager()
{
    // One manager per worker thread; QtConcurrent reuses its pooled threads.
    thread_local QNetworkAccessManager *nam = nullptr;
    if (!nam) {
        nam = new QNetworkAccessManager;
        nam->setTransferTimeout(30'000);
    }
    return *nam;
}

HttpResponse finish(QNetworkReply *reply, QIODevice *out = nullptr)
{
    QEventLoop loop;
    if (out) {
        QObject::connect(reply, &QNetworkReply::readyRead, [reply, out] {
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() / 100 == 2)
                out->write(reply->readAll());
        });
    }
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    if (!reply->isFinished())
        loop.exec();

    HttpResponse r;
    r.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray rest = reply->readAll();
    if (out && r.status / 100 == 2)
        out->write(rest);
    else
        r.body = rest;
    r.location = reply->rawHeader("Location");
    if (r.status == 0)
        r.networkError = reply->errorString();
    reply->deleteLater();
    return r;
}

void prepare(QNetworkRequest &request)
{
    if (!request.hasRawHeader("User-Agent"))
        request.setRawHeader("User-Agent", Http::kUserAgent);
}

} // namespace

namespace Http {

HttpResponse send(QNetworkRequest request, const QByteArray &verb, const QByteArray &body)
{
    prepare(request);
    return finish(manager().sendCustomRequest(request, verb, body));
}

HttpResponse send(QNetworkRequest request, const QByteArray &verb, QIODevice *body)
{
    prepare(request);
    return finish(manager().sendCustomRequest(request, verb, body));
}

HttpResponse download(QNetworkRequest request, QIODevice *out)
{
    prepare(request);
    return finish(manager().get(request), out);
}

const HttpResponse &check(const HttpResponse &r, const QString &what)
{
    if (r.ok())
        return r;
    if (r.isNetworkFailure())
        throw HttpError(0, what + ": " + r.networkError);
    throw HttpError(r.status, QStringLiteral("%1 failed (HTTP %2): %3")
                                  .arg(what).arg(r.status).arg(QString::fromUtf8(r.body.left(300))));
}

} // namespace Http
