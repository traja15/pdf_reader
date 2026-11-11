#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVector>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

/// Settings for the local AI (Ollama, https://ollama.com). Stored in QSettings.
namespace AiPrefs {
QString url();            ///< default http://localhost:11434
QString embeddingModel(); ///< default nomic-embed-text
QString answerModel();    ///< default qwen2.5:3b
bool autoIndex();         ///< index PDFs in the background
}

/// Blocking Ollama calls for worker threads. Throw HttpError on failure (status 0 = not running).
namespace Ollama {
/// Installed model names, e.g. "qwen2.5:3b".
QStringList installedModels();
bool hasModel(const QString &name);
/// Normalised embeddings, one per input. [isQuery] selects the model's query/document prefix.
QVector<QVector<float>> embed(const QStringList &texts, bool isQuery);
/// Downloads a model; [progress] gets (status text, fraction 0..1 or -1).
void pull(const QString &model, const std::function<void(const QString &, double)> &progress);
} // namespace Ollama

/// Streaming chat answer on the GUI thread.
class OllamaChat : public QObject {
    Q_OBJECT
public:
    explicit OllamaChat(QObject *parent = nullptr);
    /// Starts a new answer (cancels any running one).
    void ask(const QString &system, const QString &user);
    void cancel();
    bool isRunning() const { return m_reply != nullptr; }

signals:
    void token(const QString &text);
    void finished();
    void failed(const QString &message);

private:
    QNetworkAccessManager *m_nam;
    QPointer<QNetworkReply> m_reply;
    QByteArray m_buffer;
};
