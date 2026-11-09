#pragma once

#include <QByteArray>
#include <QNetworkRequest>
#include <QString>
#include <stdexcept>

class QIODevice;

struct HttpResponse {
    int status = 0;          ///< HTTP status; 0 when the request never reached a server.
    QByteArray body;
    QByteArray location;     ///< "Location" header (resumable upload sessions).
    QString networkError;    ///< Non-empty when there was no usable HTTP response.

    bool ok() const { return status >= 200 && status < 300; }
    bool isNetworkFailure() const { return status == 0; }
};

/// Thrown by code that needs a successful response.
class HttpError : public std::runtime_error {
public:
    HttpError(int status, const QString &message)
        : std::runtime_error(message.toStdString()), status(status) {}
    int status; ///< 0 = network failure (offline, DNS, timeout).
};

/// Blocking HTTP for worker threads (QtConcurrent). Each thread gets its own network manager.
/// Never call these on the GUI thread.
namespace Http {
inline const char *kUserAgent = "PenReader/0.1 (desktop PDF reader; personal use)";

HttpResponse send(QNetworkRequest request, const QByteArray &verb, const QByteArray &body = {});
/// Sends with a streamed body (large uploads).
HttpResponse send(QNetworkRequest request, const QByteArray &verb, QIODevice *body);
/// Streams the response body into [out] (large downloads).
HttpResponse download(QNetworkRequest request, QIODevice *out);

/// Throws HttpError unless the response is 2xx.
const HttpResponse &check(const HttpResponse &r, const QString &what);
} // namespace Http
