#include "net/Dictionary.h"

#include "core/Database.h"
#include "net/Http.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QThread>
#include <QUrl>
#include <QVariant>

namespace {

/// GET with retries for transient failures (network errors, 5xx, 429). 404 is a real answer.
HttpResponse getWithRetry(const QUrl &url)
{
    QNetworkRequest req(url);
    int delayMs = 400;
    HttpResponse r;
    for (int attempt = 0; attempt < 3; ++attempt) {
        r = Http::send(req, "GET");
        const bool transient = r.isNetworkFailure() || r.status >= 500 || r.status == 429;
        if (!transient)
            return r;
        if (attempt < 2) {
            QThread::msleep(delayMs);
            delayMs *= 3;
        }
    }
    return r;
}

QString stripHtml(QString s)
{
    static const QRegularExpression tags("<[^>]*>");
    static const QRegularExpression spaces("\\s+");
    s.remove(tags);
    s.replace("&nbsp;", " ").replace("&quot;", "\"").replace("&#39;", "'")
        .replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&");
    return s.replace(spaces, " ").trimmed();
}

QStringList toStringList(const QJsonValue &v)
{
    QStringList out;
    for (const QJsonValue &x : v.toArray())
        out << x.toString();
    return out;
}

void cacheStore(const QString &word, const QJsonArray &entries)
{
    QSqlQuery q(Database::db());
    q.prepare("INSERT OR REPLACE INTO definition_cache (word, json, fetchedAt) VALUES (?,?,?)");
    q.addBindValue(word);
    q.addBindValue(QString::fromUtf8(QJsonDocument(entries).toJson(QJsonDocument::Compact)));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.exec();
}

} // namespace

QString DictResult::firstDefinition() const
{
    for (const Meaning &m : meanings)
        if (!m.definitions.isEmpty())
            return m.definitions.first().text;
    return {};
}

QString DictResult::firstPartOfSpeech() const
{
    return meanings.isEmpty() ? QString() : meanings.first().partOfSpeech;
}

QString Dictionary::normalize(const QString &raw)
{
    QString w = raw.trimmed();
    while (!w.isEmpty() && !w.front().isLetterOrNumber()) w.remove(0, 1);
    while (!w.isEmpty() && !w.back().isLetterOrNumber()) w.chop(1);
    if (w.endsWith("'s") || w.endsWith(QStringLiteral("’s"))) w.chop(2);
    return w.toLower();
}

DictResult Dictionary::lookup(const QString &rawWord)
{
    const QString word = normalize(rawWord);
    DictResult result;
    result.word = word;
    if (word.isEmpty()) {
        result.status = DictResult::Status::NotFound;
        return result;
    }

    {
        QSqlQuery q(Database::db());
        q.prepare("SELECT json FROM definition_cache WHERE word = ?");
        q.addBindValue(word);
        if (q.exec() && q.next())
            return fromApiJson(word, QJsonDocument::fromJson(q.value(0).toString().toUtf8()).array(), true);
    }

    const QString encoded = QString::fromLatin1(QUrl::toPercentEncoding(word));
    const HttpResponse primary = getWithRetry(QUrl("https://api.dictionaryapi.dev/api/v2/entries/en/" + encoded));
    if (primary.ok()) {
        const QJsonArray entries = QJsonDocument::fromJson(primary.body).array();
        if (!entries.isEmpty()) {
            cacheStore(word, entries);
            return fromApiJson(word, entries, false);
        }
    }

    // The free service is flaky (502s) and has gaps; Wiktionary covers both cases.
    const HttpResponse wiki = getWithRetry(QUrl("https://en.wiktionary.org/api/rest_v1/page/definition/" + encoded));
    if (wiki.ok()) {
        const QJsonArray entries = wiktionaryToApiJson(word, wiki.body);
        if (!entries.isEmpty()) {
            cacheStore(word, entries);
            return fromApiJson(word, entries, false);
        }
    }

    const bool primaryAnswered = primary.ok() || primary.status == 404;
    const bool wikiAnswered = wiki.ok() || wiki.status == 404;
    if (primaryAnswered && wikiAnswered) {
        result.status = DictResult::Status::NotFound;
    } else if (primary.isNetworkFailure() && wiki.isNetworkFailure()) {
        result.message = QStringLiteral("You're offline and \"%1\" isn't saved for offline use yet.").arg(word);
    } else {
        result.message = QStringLiteral("The dictionary service isn't responding right now. Try again.");
    }
    return result;
}

DictResult Dictionary::fromApiJson(const QString &word, const QJsonArray &entries, bool fromCache)
{
    DictResult r;
    r.status = DictResult::Status::Found;
    r.word = word;
    r.fromCache = fromCache;
    for (const QJsonValue &ev : entries) {
        const QJsonObject e = ev.toObject();
        if (r.phonetic.isEmpty())
            r.phonetic = e["phonetic"].toString();
        for (const QJsonValue &pv : e["phonetics"].toArray()) {
            const QJsonObject p = pv.toObject();
            if (r.phonetic.isEmpty()) r.phonetic = p["text"].toString();
            if (r.audioUrl.isEmpty()) r.audioUrl = p["audio"].toString();
        }
        for (const QJsonValue &mv : e["meanings"].toArray()) {
            const QJsonObject m = mv.toObject();
            Meaning meaning;
            meaning.partOfSpeech = m["partOfSpeech"].toString();
            meaning.synonyms = toStringList(m["synonyms"]);
            for (const QJsonValue &dv : m["definitions"].toArray()) {
                const QJsonObject d = dv.toObject();
                meaning.definitions.append({d["definition"].toString(), d["example"].toString(), toStringList(d["synonyms"])});
            }
            r.meanings.append(meaning);
        }
    }
    return r;
}

QJsonArray Dictionary::wiktionaryToApiJson(const QString &word, const QByteArray &body)
{
    QJsonArray meanings;
    for (const QJsonValue &uv : QJsonDocument::fromJson(body).object()["en"].toArray()) {
        const QJsonObject usage = uv.toObject();
        QJsonArray defs;
        for (const QJsonValue &dv : usage["definitions"].toArray()) {
            const QJsonObject d = dv.toObject();
            const QString text = stripHtml(d["definition"].toString());
            if (text.isEmpty())
                continue;
            QJsonObject def{{"definition", text}};
            const QJsonArray examples = d["examples"].toArray();
            if (!examples.isEmpty())
                def["example"] = stripHtml(examples.first().toString());
            defs.append(def);
        }
        if (!defs.isEmpty())
            meanings.append(QJsonObject{{"partOfSpeech", usage["partOfSpeech"].toString().toLower()}, {"definitions", defs}});
    }
    if (meanings.isEmpty())
        return {};
    return QJsonArray{QJsonObject{{"word", word}, {"meanings", meanings}}};
}
