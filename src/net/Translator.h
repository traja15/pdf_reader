#pragma once

#include <QString>

/**
 * Sentence / paragraph translation (English → Tamil by default).
 * Google Translate's free web endpoint first, MyMemory as a fallback; results are cached on this
 * computer so a passage translated once also works offline. Blocking: call from a worker thread.
 * Only the text you choose to translate is sent.
 */
namespace Translator {
struct Result {
    QString text;      ///< The translation (empty on failure).
    QString error;     ///< Why it failed, in plain words.
    bool cached = false;
};
Result translate(const QString &text, const QString &target = QStringLiteral("ta"));
} // namespace Translator
