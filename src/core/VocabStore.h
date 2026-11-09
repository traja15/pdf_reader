#pragma once

#include "core/Models.h"

#include <QObject>
#include <optional>

/// "My Words": saved words with definitions, Tamil meaning and where they were found.
class VocabStore : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    /// Saved (non-deleted) words, newest first.
    QVector<VocabWord> words() const;
    QVector<VocabWord> allIncludingDeleted() const;
    std::optional<VocabWord> get(const QString &word) const;
    bool isSaved(const QString &word) const;

    /// Saves or updates a word. Missing source fields are kept from an earlier save.
    void save(VocabWord w);
    void remove(const QString &word);
    /// Writes a row as-is (sync uses this to apply other devices' changes).
    void upsertRaw(const VocabWord &w);

signals:
    void changed();
    void needsSync();
};
