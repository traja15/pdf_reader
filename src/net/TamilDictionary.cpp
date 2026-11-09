#include "net/TamilDictionary.h"

#include "core/Database.h"
#include "net/Dictionary.h"
#include "net/Http.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSet>
#include <QSqlQuery>
#include <QTextStream>
#include <QUrlQuery>
#include <QVariant>
#include <algorithm>

namespace {

const QSet<QString> kPartsOfSpeech = {
    "Noun", "Verb", "Adjective", "Adverb", "Pronoun", "Preposition", "Conjunction",
    "Interjection", "Phrase", "Proper noun", "Determiner", "Numeral",
};

/// Raw wikitext of a Wiktionary page, or an empty string when the page doesn't exist.
QString fetchWikitext(const QString &title)
{
    QUrl url("https://en.wiktionary.org/w/api.php");
    QUrlQuery query;
    query.addQueryItem("action", "parse");
    query.addQueryItem("page", title);
    query.addQueryItem("prop", "wikitext");
    query.addQueryItem("format", "json");
    query.addQueryItem("formatversion", "2");
    url.setQuery(query);
    const HttpResponse r = Http::check(Http::send(QNetworkRequest(url), "GET"), "Wiktionary");
    return QJsonDocument::fromJson(r.body).object()["parse"].toObject()["wikitext"].toString();
}

QString englishSection(const QString &wikitext)
{
    const qsizetype start = wikitext.indexOf("==English==");
    if (start < 0)
        return {};
    static const QRegularExpression nextLanguage("^==[^=].*==\\s*$", QRegularExpression::MultilineOption);
    const auto m = nextLanguage.match(wikitext, start + 11);
    return wikitext.mid(start, m.hasMatch() ? m.capturedStart() - start : -1);
}

QString cleanWikitext(QString s)
{
    static const QRegularExpression link("\\[\\[(?:[^|\\]]*\\|)?([^\\]]*)\\]\\]");
    static const QRegularExpression quotes("'{2,}");
    return s.replace(link, "\\1").remove(quotes).trimmed();
}

QVector<TamilSense> parseTranslations(const QString &section)
{
    static const QRegularExpression heading("^(={3,6})\\s*([^=]+?)\\s*\\1$");
    static const QRegularExpression transTop("\\{\\{trans-top\\|([^}]*)\\}\\}");
    static const QRegularExpression tamilLine("^\\*:?\\s*Tamil:");
    static const QRegularExpression tamilTerm("\\{\\{tt?\\+?\\|ta\\|([^|}]+)([^}]*)\\}\\}");
    static const QRegularExpression translit("\\|tr=([^|}]+)");

    QVector<TamilSense> senses;
    QString pos, meaning;
    for (const QString &line : section.split('\n')) {
        if (const auto h = heading.match(line.trimmed()); h.hasMatch() && kPartsOfSpeech.contains(h.captured(2)))
            pos = h.captured(2).toLower();
        if (const auto t = transTop.match(line); t.hasMatch()) {
            meaning.clear();
            for (const QString &param : t.captured(1).split('|')) {
                if (!param.contains('=')) { meaning = cleanWikitext(param); break; }
            }
        }
        if (tamilLine.match(line).hasMatch()) {
            QVector<TamilWord> words;
            for (auto it = tamilTerm.globalMatch(line); it.hasNext();) {
                const auto m = it.next();
                const QString tamil = m.captured(1).trimmed();
                if (!tamil.isEmpty())
                    words.append({tamil, translit.match(m.captured(2)).captured(1).trimmed()});
            }
            if (!words.isEmpty())
                senses.append({pos, meaning, words});
        }
    }
    return senses;
}

QJsonArray sensesToJson(const QVector<TamilSense> &senses)
{
    QJsonArray arr;
    for (const TamilSense &s : senses) {
        QJsonArray words;
        for (const TamilWord &w : s.words)
            words.append(QJsonObject{{"tamil", w.tamil}, {"romanization", w.romanization}});
        arr.append(QJsonObject{{"partOfSpeech", s.partOfSpeech}, {"meaning", s.meaning}, {"words", words}});
    }
    return arr;
}

QVector<TamilSense> sensesFromJson(const QJsonArray &arr)
{
    QVector<TamilSense> out;
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        TamilSense s{o["partOfSpeech"].toString(), o["meaning"].toString(), {}};
        for (const QJsonValue &w : o["words"].toArray())
            s.words.append({w.toObject()["tamil"].toString(), w.toObject()["romanization"].toString()});
        out.append(s);
    }
    return out;
}

/// Rank Tamil words glossed *primarily* as this English word first ("abandon, forsake" beats "leave, abandon").
int glossRank(const QString &gloss, const QString &key)
{
    static const QRegularExpression sep("[,;]");
    const QStringList items = gloss.toLower().split(sep);
    for (int i = 0; i < items.size(); ++i) {
        QString item = items[i].trimmed();
        if (item.startsWith("to ")) item = item.mid(3).trimmed();
        if (item == key) return i;
    }
    return 99;
}

} // namespace

QString TamilEntry::summary(int max) const
{
    QStringList out;
    auto add = [&](const QString &t) { if (!out.contains(t) && out.size() < max) out << t; };
    for (const TamilSense &s : senses)
        for (const TamilWord &w : s.words) add(w.tamil);
    for (const TamilIndexHit &h : related) add(h.tamil);
    return out.join(", ");
}

QStringList TamilDictionary::baseForms(const QString &w)
{
    QStringList out;
    const auto len = w.size();
    if (w.endsWith("ies") && len > 4) out << w.chopped(3) + "y";
    if (w.endsWith("es") && len > 3) out << w.chopped(2);
    if (w.endsWith("s") && !w.endsWith("ss") && len > 3) out << w.chopped(1);
    if (w.endsWith("ied") && len > 4) out << w.chopped(3) + "y";
    if (w.endsWith("ed") && len > 3) out << w.chopped(2) << w.chopped(1);
    if (w.endsWith("ing") && len > 5) out << w.chopped(3) << w.chopped(3) + "e";
    if (w.endsWith("ly") && len > 4) out << w.chopped(2);
    return out;
}

TamilEntry TamilDictionary::lookup(const QString &rawWord)
{
    TamilEntry entry;
    entry.word = Dictionary::normalize(rawWord);
    if (entry.word.isEmpty())
        return entry;

    QStringList candidates = QStringList{entry.word} + baseForms(entry.word);
    candidates.removeDuplicates();

    bool offline = false;
    for (const QString &w : candidates) {
        try {
            entry.senses = onlineSenses(w);
        } catch (const std::exception &) {
            offline = true; // network down or odd response: fall back to the offline index
        }
        if (!entry.senses.isEmpty())
            break;
    }

    QSet<QString> known;
    for (const TamilSense &s : entry.senses)
        for (const TamilWord &w : s.words) known.insert(w.tamil);
    const auto &idx = index();
    for (const QString &w : candidates) {
        const auto it = idx.find(w);
        if (it == idx.end() || it->isEmpty())
            continue;
        for (const TamilIndexHit &h : *it)
            if (!known.contains(h.tamil)) entry.related.append(h);
        break;
    }
    entry.offlineOnly = offline && entry.senses.isEmpty();
    return entry;
}

QVector<TamilSense> TamilDictionary::onlineSenses(const QString &word)
{
    {
        QSqlQuery q(Database::db());
        q.prepare("SELECT json FROM tamil_cache WHERE word = ?");
        q.addBindValue(word);
        if (q.exec() && q.next())
            return sensesFromJson(QJsonDocument::fromJson(q.value(0).toString().toUtf8()).array());
    }

    const QString english = englishSection(fetchWikitext(word));
    QVector<TamilSense> senses = parseTranslations(english);
    // Long entries keep translations on a separate "word/translations" page.
    if (english.contains("see translation subpage") || english.contains("trans-see"))
        senses += parseTranslations(englishSection(fetchWikitext(word + "/translations")));

    QSqlQuery q(Database::db());
    q.prepare("INSERT OR REPLACE INTO tamil_cache (word, json, fetchedAt) VALUES (?,?,?)");
    q.addBindValue(word);
    q.addBindValue(QString::fromUtf8(QJsonDocument(sensesToJson(senses)).toJson(QJsonDocument::Compact)));
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.exec();
    return senses;
}

const QHash<QString, QVector<TamilIndexHit>> &TamilDictionary::index()
{
    QMutexLocker lock(&m_indexLock);
    if (m_indexLoaded)
        return m_index;
    m_indexLoaded = true;

    QFile f(":/data/en_ta.tsv");
    if (!f.open(QIODevice::ReadOnly))
        return m_index; // a missing index must never break the dictionary
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    while (!in.atEnd()) {
        const QStringList cols = in.readLine().split('\t');
        if (cols.size() >= 5)
            m_index[cols[0]].append({cols[1], cols[2], cols[3], cols[4]});
    }
    for (auto it = m_index.begin(); it != m_index.end(); ++it) {
        const QString key = it.key();
        std::stable_sort(it->begin(), it->end(), [&](const TamilIndexHit &a, const TamilIndexHit &b) {
            const int ra = glossRank(a.gloss, key), rb = glossRank(b.gloss, key);
            return ra != rb ? ra < rb : a.gloss.size() < b.gloss.size();
        });
    }
    return m_index;
}
