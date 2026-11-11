#include "net/Ollama.h"

#include "net/Http.h"

#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSettings>
#include <cmath>

namespace AiPrefs {
QString url() { return QSettings().value("ai/url", "http://localhost:11434").toString().trimmed(); }
QString embeddingModel() { return QSettings().value("ai/embeddingModel", "nomic-embed-text").toString().trimmed(); }
QString answerModel() { return QSettings().value("ai/answerModel", "qwen2.5:3b").toString().trimmed(); }
bool autoIndex() { return QSettings().value("ai/autoIndex", true).toBool(); }
} // namespace AiPrefs

namespace {

QNetworkRequest jsonRequest(const QString &path)
{
    QNetworkRequest req{QUrl(AiPrefs::url() + path)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setTransferTimeout(0); // model loading can take a while
    return req;
}

QByteArray toJson(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

/// Embedding models expect different prefixes for queries vs. documents.
QString withPrefix(const QString &model, const QString &text, bool isQuery)
{
    const QString m = model.toLower();
    if (m.contains("nomic")) return (isQuery ? "search_query: " : "search_document: ") + text;
    if (m.contains("e5")) return (isQuery ? "query: " : "passage: ") + text;
    if (m.contains("bge") && isQuery) return "Represent this sentence for searching relevant passages: " + text;
    if (m.contains("arctic") && isQuery) return "Represent this sentence for searching relevant passages: " + text;
    return text;
}

} // namespace

namespace Ollama {

QStringList installedModels()
{
    const HttpResponse r = Http::check(Http::send(jsonRequest("/api/tags"), "GET"), "Ollama");
    QStringList names;
    for (const QJsonValue &m : QJsonDocument::fromJson(r.body).object()["models"].toArray())
        names << m.toObject()["name"].toString();
    return names;
}

bool hasModel(const QString &name)
{
    const QStringList models = installedModels();
    // "nomic-embed-text" matches "nomic-embed-text:latest".
    return models.contains(name) || models.contains(name + ":latest");
}

QVector<QVector<float>> embed(const QStringList &texts, bool isQuery)
{
    const QString model = AiPrefs::embeddingModel();
    QJsonArray input;
    for (const QString &t : texts)
        input.append(withPrefix(model, t, isQuery));
    const HttpResponse r = Http::check(
        Http::send(jsonRequest("/api/embed"), "POST", toJson({{"model", model}, {"input", input}, {"truncate", true}})),
        "Ollama embeddings");

    QVector<QVector<float>> out;
    for (const QJsonValue &v : QJsonDocument::fromJson(r.body).object()["embeddings"].toArray()) {
        QVector<float> vec;
        double norm = 0;
        for (const QJsonValue &x : v.toArray()) {
            vec.append(float(x.toDouble()));
            norm += x.toDouble() * x.toDouble();
        }
        norm = std::sqrt(norm);
        if (norm > 0)
            for (float &x : vec) x = float(x / norm);
        out.append(vec);
    }
    if (out.size() != texts.size())
        throw HttpError(500, "Ollama returned the wrong number of embeddings");
    return out;
}

void pull(const QString &model, const std::function<void(const QString &, double)> &progress)
{
    QNetworkAccessManager nam;
    QNetworkReply *reply = nam.post(jsonRequest("/api/pull"), toJson({{"model", model}, {"stream", true}}));
    QByteArray buffer;
    QString error;
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::readyRead, [&] {
        buffer += reply->readAll();
        qsizetype nl;
        while ((nl = buffer.indexOf('\n')) >= 0) {
            const QJsonObject o = QJsonDocument::fromJson(buffer.left(nl)).object();
            buffer.remove(0, nl + 1);
            if (o.contains("error")) error = o["error"].toString();
            const double total = o["total"].toDouble();
            progress(o["status"].toString(), total > 0 ? o["completed"].toDouble() / total : -1);
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QString netError = reply->errorString();
    reply->deleteLater();
    if (status == 0) throw HttpError(0, "Ollama isn't running: " + netError);
    if (!error.isEmpty() || status / 100 != 2) throw HttpError(status, "Couldn't download " + model + ": " + error);
}

} // namespace Ollama

OllamaChat::OllamaChat(QObject *parent) : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

void OllamaChat::ask(const QString &system, const QString &user)
{
    cancel();
    m_buffer.clear();
    const QJsonObject body{
        {"model", AiPrefs::answerModel()},
        {"stream", true},
        {"messages", QJsonArray{QJsonObject{{"role", "system"}, {"content", system}},
                                QJsonObject{{"role", "user"}, {"content", user}}}},
        {"options", QJsonObject{{"temperature", 0.2}, {"num_ctx", 4096}}},
    };
    QNetworkReply *reply = m_nam->post(jsonRequest("/api/chat"), toJson(body));
    m_reply = reply;
    connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
        m_buffer += reply->readAll();
        qsizetype nl;
        while ((nl = m_buffer.indexOf('\n')) >= 0) {
            const QJsonObject o = QJsonDocument::fromJson(m_buffer.left(nl)).object();
            m_buffer.remove(0, nl + 1);
            if (o.contains("error")) {
                emit failed(o["error"].toString());
                continue;
            }
            const QString piece = o["message"].toObject()["content"].toString();
            if (!piece.isEmpty()) emit token(piece);
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool aborted = reply->error() == QNetworkReply::OperationCanceledError;
        if (m_reply == reply) m_reply = nullptr;
        reply->deleteLater();
        if (aborted) return;
        if (status == 0) emit failed("Ollama isn't running. Start it with “ollama serve”.");
        else if (status / 100 != 2) emit failed(QStringLiteral("Ollama error %1 — is the model “%2” downloaded?").arg(status).arg(AiPrefs::answerModel()));
        else emit finished();
    });
}

void OllamaChat::cancel()
{
    if (m_reply) {
        QNetworkReply *r = m_reply;
        m_reply = nullptr;
        r->abort();
    }
}
