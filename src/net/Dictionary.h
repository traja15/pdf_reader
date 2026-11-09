#pragma once

#include <QJsonArray>
#include <QString>
#include <QStringList>
#include <QVector>

struct Definition {
    QString text;
    QString example;
    QStringList synonyms;
};

struct Meaning {
    QString partOfSpeech;
    QVector<Definition> definitions;
    QStringList synonyms;
};

struct DictResult {
    enum class Status { Found, NotFound, Error };
    Status status = Status::Error;
    QString word;
    QString message; ///< For Error: what went wrong, in user terms.
    QString phonetic;
    QString audioUrl;
    QVector<Meaning> meanings;
    bool fromCache = false;

    QString firstDefinition() const;
    QString firstPartOfSpeech() const;
};

/// English dictionary: dictionaryapi.dev first, Wiktionary as a fallback when that service is down
/// or lacks the word. Results are cached in SQLite so looked-up words work offline.
/// Blocking: call from a worker thread.
class Dictionary {
public:
    DictResult lookup(const QString &rawWord);

    /// "Ephemeral," -> "ephemeral", "world's" -> "world".
    static QString normalize(const QString &raw);

private:
    static DictResult fromApiJson(const QString &word, const QJsonArray &entries, bool fromCache);
    static QJsonArray wiktionaryToApiJson(const QString &word, const QByteArray &body);
};
