#pragma once

#include <QDateTime>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <stdexcept>

class QTcpServer;

/// Thrown when there is no valid Google sign-in; the user must sign in from the UI.
class NeedsSignIn : public std::runtime_error {
public:
    NeedsSignIn() : std::runtime_error("Sign in with Google to sync") {}
};

/**
 * Google OAuth for desktop ("installed app" flow with PKCE and a loopback redirect).
 *
 * It needs an OAuth client of type "Desktop app" from the *same* Google Cloud project as the
 * Android app, so both share the Drive appDataFolder. Download its JSON from Cloud Console and
 * load it with File → Google client file… (stored as google_oauth_client.json in the config dir).
 */
class GoogleAuth : public QObject {
    Q_OBJECT
public:
    explicit GoogleAuth(QObject *parent = nullptr);

    bool isConfigured() const { return !m_clientId.isEmpty(); }
    bool isSignedIn() const;
    /// Copies a client JSON downloaded from Cloud Console into the config dir. Returns an error or "".
    QString installClientFile(const QString &path);

    /// GUI thread: opens the browser for consent; emits browserSignInStarted, then signInFinished.
    void signIn();
    /// Abandons a sign-in that's waiting for the browser.
    void cancelSignIn();
    void signOut();
    /// Opens [url] in the user's browser. Returns false if nothing could be launched.
    static bool openInBrowser(const QUrl &url);

    /// Worker thread: a valid access token, refreshing it if needed. Throws NeedsSignIn or HttpError.
    QString accessToken();

    static QString clientConfigPath();

signals:
    /// The consent page URL; [opened] is false when no browser could be launched (show the link).
    void browserSignInStarted(const QUrl &url, bool opened);
    void signInFinished(bool ok, const QString &error);
    void signedInChanged(bool signedIn);

private:
    void loadClient();
    void onBrowserRedirect();
    void exchangeCode(const QString &code);

    QString m_clientId;
    QString m_clientSecret;

    QMutex m_tokenLock;
    QString m_accessToken;
    QDateTime m_expiry;

    QPointer<QTcpServer> m_server;
    QString m_verifier;
    QString m_state;
    QString m_redirectUri;
};
