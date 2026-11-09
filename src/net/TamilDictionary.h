#pragma once

#include <QHash>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QVector>

struct TamilWord {
    QString tamil;
    QString romanization;
};

/// One meaning of the English word and its Tamil equivalents.
struct TamilSense {
    QString partOfSpeech;
    QString meaning;
    QVector<TamilWord> words;
};

/// A Tamil word whose Wiktionary gloss matches the English word (bundled offline index).
struct TamilIndexHit {
    QString tamil;
    QString romanization;
    QString partOfSpeech;
    QString gloss;
};

struct TamilEntry {
    QString word;
    QVector<TamilSense> senses;     ///< From English Wiktionary translation tables (online, cached).
    QVector<TamilIndexHit> related; ///< From the offline index.
    bool offlineOnly = false;       ///< The online part couldn't be fetched and isn't cached.

    bool isEmpty() const { return senses.isEmpty() && related.isEmpty(); }
    /// A few Tamil words for list views, e.g. "கைவிடு, விடு, துற".
    QString summary(int max = 3) const;
};

/// English -> Tamil from two Wiktionary-derived sources (CC BY-SA), same as the Android app:
///  1. English Wiktionary translation tables, fetched per word and cached in SQLite.
///  2. The bundled kaikki.org reverse index (:/data/en_ta.tsv).
/// Blocking: call from a worker thread.
class TamilDictionary {
public:
    TamilEntry lookup(const QString &rawWord);

    static QStringList baseForms(const QString &w);

private:
    QVector<TamilSense> onlineSenses(const QString &word); // throws HttpError when offline
    const QHash<QString, QVector<TamilIndexHit>> &index();

    QMutex m_indexLock;
    QHash<QString, QVector<TamilIndexHit>> m_index;
    bool m_indexLoaded = false;
};
