#include "net/GoogleAuth.h"

#include "core/OpenUrl.h"
#include "net/Http.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrlQuery>
#include <QtConcurrent>

namespace {

const char *kAuthEndpoint = "https://accounts.google.com/o/oauth2/v2/auth";
const char *kTokenEndpoint = "https://oauth2.googleapis.com/token";
const char *kScope = "https://www.googleapis.com/auth/drive.appdata";
const char *kRefreshKey = "google/refreshToken";

QString randomUrlSafe(int bytes)
{
    QByteArray buf(bytes, Qt::Uninitialized);
    QRandomGenerator::system()->generate(buf.begin(), buf.end());
    return QString::fromLatin1(buf.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QByteArray form(const QList<std::pair<QString, QString>> &fields)
{
    QUrlQuery q;
    for (const auto &[k, v] : fields)
        q.addQueryItem(k, QString::fromLatin1(QUrl::toPercentEncoding(v)));
    return q.query(QUrl::FullyEncoded).toUtf8();
}

QNetworkRequest tokenRequest()
{
    QNetworkRequest req{QUrl(kTokenEndpoint)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    return req;
}

} // namespace

GoogleAuth::GoogleAuth(QObject *parent) : QObject(parent) { loadClient(); }

QString GoogleAuth::clientConfigPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/google_oauth_client.json";
}

void GoogleAuth::loadClient()
{
    // Config dir first (a file the user chose), then next to the executable (handy for portable
    // Windows installs), then one built into the program (cmake -DPENREADER_GOOGLE_CLIENT=…).
    for (const QString &path : {clientConfigPath(), QCoreApplication::applicationDirPath() + "/google_oauth_client.json",
                                QStringLiteral(":/google/google_oauth_client.json")}) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
        const QJsonObject c = root.contains("installed") ? root["installed"].toObject() : root;
        m_clientId = c["client_id"].toString();
        m_clientSecret = c["client_secret"].toString();
        if (isConfigured())
            return;
    }
}

QString GoogleAuth::installClientFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return "Can't read that file.";
    const QByteArray data = f.readAll();
    const QJsonObject root = QJsonDocument::fromJson(data).object();
    if (!root.contains("installed"))
        return "That isn't a \"Desktop app\" OAuth client file. In Google Cloud Console create an OAuth client "
               "ID of type Desktop app and download its JSON.";
    QDir().mkpath(QFileInfo(clientConfigPath()).absolutePath());
    QFile out(clientConfigPath());
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size())
        return "Couldn't save the client file.";
    out.close();
    loadClient();
    return {};
}

bool GoogleAuth::isSignedIn() const { return !QSettings().value(kRefreshKey).toString().isEmpty(); }

void GoogleAuth::signIn()
{
    if (!isConfigured()) {
        emit signInFinished(false, "Google sign-in isn't set up yet. Use File → Google client file… first.");
        return;
    }
    if (m_server)
        m_server->deleteLater();
    m_server = new QTcpServer(this);
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        emit signInFinished(false, "Couldn't open a local port for the Google sign-in reply.");
        return;
    }
    connect(m_server, &QTcpServer::newConnection, this, &GoogleAuth::onBrowserRedirect);

    m_verifier = randomUrlSafe(48);
    m_state = randomUrlSafe(16);
    m_redirectUri = QStringLiteral("http://127.0.0.1:%1").arg(m_server->serverPort());
    const QString challenge = QString::fromLatin1(
        QCryptographicHash::hash(m_verifier.toLatin1(), QCryptographicHash::Sha256)
            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));

    QUrl url(kAuthEndpoint);
    QUrlQuery q;
    q.addQueryItem("client_id", m_clientId);
    q.addQueryItem("redirect_uri", m_redirectUri);
    q.addQueryItem("response_type", "code");
    q.addQueryItem("scope", kScope);
    q.addQueryItem("code_challenge", challenge);
    q.addQueryItem("code_challenge_method", "S256");
    q.addQueryItem("state", m_state);
    q.addQueryItem("access_type", "offline");
    q.addQueryItem("prompt", "consent");
    url.setQuery(q);
    emit browserSignInStarted(url, openInBrowser(url));
}

bool GoogleAuth::openInBrowser(const QUrl &url) { return openUrlInBrowser(url); }

void GoogleAuth::cancelSignIn()
{
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
    }
}

void GoogleAuth::onBrowserRedirect()
{
    QTcpSocket *socket = m_server->nextPendingConnection();
    if (!socket)
        return;
    connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
        // "GET /?state=…&code=… HTTP/1.1"
        const QByteArray requestLine = socket->readLine();
        const QList<QByteArray> parts = requestLine.split(' ');
        if (parts.size() < 2)
            return;
        const QUrlQuery query(QUrl::fromEncoded(parts[1]).query());
        const QString code = query.queryItemValue("code", QUrl::FullyDecoded);
        const QString error = query.queryItemValue("error");
        const bool stateOk = query.queryItemValue("state") == m_state;
        if (code.isEmpty() && error.isEmpty())
            return; // e.g. a favicon request

        const QByteArray page = (!code.isEmpty() && stateOk)
            ? "<h2>PenReader is signed in.</h2><p>You can close this tab and go back to the app.</p>"
            : "<h2>Sign-in didn't complete.</h2><p>You can close this tab and try again from PenReader.</p>";
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
                      "<html><body style='font-family:sans-serif;margin:3em'>" + page + "</body></html>");
        socket->disconnectFromHost();
        m_server->close();

        if (!code.isEmpty() && stateOk)
            exchangeCode(code);
        else
            emit signInFinished(false, error.isEmpty() ? "Sign-in was interrupted." : "Google sign-in failed: " + error);
    });
}

void GoogleAuth::exchangeCode(const QString &code)
{
    const QByteArray body = form({{"client_id", m_clientId}, {"client_secret", m_clientSecret}, {"code", code},
                                  {"code_verifier", m_verifier}, {"grant_type", "authorization_code"},
                                  {"redirect_uri", m_redirectUri}});
    auto *watcher = new QFutureWatcher<HttpResponse>(this);
    connect(watcher, &QFutureWatcher<HttpResponse>::finished, this, [this, watcher] {
        watcher->deleteLater();
        const HttpResponse r = watcher->result();
        const QJsonObject o = QJsonDocument::fromJson(r.body).object();
        const QString refresh = o["refresh_token"].toString();
        if (!r.ok() || refresh.isEmpty()) {
            emit signInFinished(false, "Google rejected the sign-in: " +
                                           (r.isNetworkFailure() ? r.networkError : QString::fromUtf8(r.body.left(300))));
            return;
        }
        QSettings().setValue(kRefreshKey, refresh);
        {
            QMutexLocker lock(&m_tokenLock);
            m_accessToken = o["access_token"].toString();
            m_expiry = QDateTime::currentDateTimeUtc().addSecs(o["expires_in"].toInt(3600) - 60);
        }
        emit signedInChanged(true);
        emit signInFinished(true, {});
    });
    watcher->setFuture(QtConcurrent::run([body] { return Http::send(tokenRequest(), "POST", body); }));
}

void GoogleAuth::signOut()
{
    QSettings().remove(kRefreshKey);
    QMutexLocker lock(&m_tokenLock);
    m_accessToken.clear();
    lock.unlock();
    emit signedInChanged(false);
}

QString GoogleAuth::accessToken()
{
    QMutexLocker lock(&m_tokenLock);
    if (!m_accessToken.isEmpty() && QDateTime::currentDateTimeUtc() < m_expiry)
        return m_accessToken;

    const QString refresh = QSettings().value(kRefreshKey).toString();
    if (refresh.isEmpty() || !isConfigured())
        throw NeedsSignIn();

    const HttpResponse r = Http::send(tokenRequest(), "POST",
                                      form({{"client_id", m_clientId}, {"client_secret", m_clientSecret},
                                            {"refresh_token", refresh}, {"grant_type", "refresh_token"}}));
    if (r.status == 400 || r.status == 401) {
        // Revoked or expired (test-mode refresh tokens last 7 days): the user must sign in again.
        QSettings().remove(kRefreshKey);
        m_accessToken.clear();
        lock.unlock();
        QMetaObject::invokeMethod(this, [this] { emit signedInChanged(false); }, Qt::QueuedConnection);
        throw NeedsSignIn();
    }
    Http::check(r, "Google token refresh");
    const QJsonObject o = QJsonDocument::fromJson(r.body).object();
    m_accessToken = o["access_token"].toString();
    m_expiry = QDateTime::currentDateTimeUtc().addSecs(o["expires_in"].toInt(3600) - 60);
    return m_accessToken;
}
