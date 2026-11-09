#include "core/VocabStore.h"

#include "core/Database.h"

#include <QDateTime>
#include <QSqlQuery>
#include <QVariant>

namespace {

VocabWord readWord(const QSqlQuery &q)
{
    VocabWord w;
    w.word = q.value("word").toString();
    w.phonetic = q.value("phonetic").toString();
    w.partOfSpeech = q.value("partOfSpeech").toString();
    w.definition = q.value("definition").toString();
    w.tamil = q.value("tamil").toString();
    w.contextSentence = q.value("contextSentence").toString();
    w.sourceDocKey = q.value("sourceDocKey").toString();
    w.sourceTitle = q.value("sourceTitle").toString();
    w.sourcePage = q.value("sourcePage").isNull() ? -1 : q.value("sourcePage").toInt();
    w.addedAt = q.value("addedAt").toLongLong();
    w.updatedAt = q.value("updatedAt").toLongLong();
    w.deleted = q.value("deleted").toBool();
    return w;
}

QVariant orNull(const QString &s) { return s.isEmpty() ? QVariant() : QVariant(s); }

} // namespace

QVector<VocabWord> VocabStore::words() const
{
    QVector<VocabWord> out;
    QSqlQuery q(Database::db());
    q.exec("SELECT * FROM vocab WHERE deleted = 0 ORDER BY addedAt DESC");
    while (q.next())
        out.append(readWord(q));
    return out;
}

QVector<VocabWord> VocabStore::allIncludingDeleted() const
{
    QVector<VocabWord> out;
    QSqlQuery q(Database::db());
    q.exec("SELECT * FROM vocab");
    while (q.next())
        out.append(readWord(q));
    return out;
}

std::optional<VocabWord> VocabStore::get(const QString &word) const
{
    QSqlQuery q(Database::db());
    q.prepare("SELECT * FROM vocab WHERE word = ?");
    q.addBindValue(word);
    if (q.exec() && q.next())
        return readWord(q);
    return std::nullopt;
}

bool VocabStore::isSaved(const QString &word) const
{
    const auto w = get(word);
    return w && !w->deleted;
}

void VocabStore::save(VocabWord w)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (const auto existing = get(w.word)) {
        if (w.tamil.isEmpty()) w.tamil = existing->tamil;
        if (w.contextSentence.isEmpty()) w.contextSentence = existing->contextSentence;
        if (w.sourceDocKey.isEmpty()) {
            w.sourceDocKey = existing->sourceDocKey;
            w.sourceTitle = existing->sourceTitle;
            w.sourcePage = existing->sourcePage;
        }
        w.addedAt = existing->deleted ? now : existing->addedAt;
    } else {
        w.addedAt = now;
    }
    w.updatedAt = now;
    w.deleted = false;
    upsertRaw(w);
    emit needsSync();
}

void VocabStore::remove(const QString &word)
{
    auto w = get(word);
    if (!w)
        return;
    w->deleted = true;
    w->updatedAt = QDateTime::currentMSecsSinceEpoch();
    upsertRaw(*w);
    emit needsSync();
}

void VocabStore::upsertRaw(const VocabWord &w)
{
    QSqlQuery q(Database::db());
    q.prepare("INSERT OR REPLACE INTO vocab (word, phonetic, partOfSpeech, definition, tamil, contextSentence, "
              "sourceDocKey, sourceTitle, sourcePage, addedAt, updatedAt, deleted) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
    q.addBindValue(w.word);
    q.addBindValue(orNull(w.phonetic));
    q.addBindValue(orNull(w.partOfSpeech));
    q.addBindValue(w.definition);
    q.addBindValue(orNull(w.tamil));
    q.addBindValue(orNull(w.contextSentence));
    q.addBindValue(orNull(w.sourceDocKey));
    q.addBindValue(orNull(w.sourceTitle));
    q.addBindValue(w.sourcePage >= 0 ? QVariant(w.sourcePage) : QVariant());
    q.addBindValue(w.addedAt);
    q.addBindValue(w.updatedAt);
    q.addBindValue(w.deleted);
    q.exec();
    emit changed();
}
