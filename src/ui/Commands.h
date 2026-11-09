#pragma once

#include <QHash>
#include <QKeySequence>
#include <QList>
#include <QString>
#include <functional>
#include <optional>

/// One keyboard-bindable command.
struct Command {
    QString id;
    QString category;
    QString name;
    QList<QKeySequence> standardKeys; ///< Always active (unless customised).
    QList<QKeySequence> vimKeys;      ///< Active when vim-style keys are on.
};

/// Registry of every bindable command plus the user's customised shortcuts (QSettings).
/// Effective keys = the user's override if set, else standard keys + (vim keys when vim mode is on).
/// With vim mode on, a vim key wins over another command's standard key (e.g. Ctrl+R is redo, not rotate).
namespace Commands {

const QList<Command> &all();
const Command *find(const QString &id);

bool vimEnabled();
void setVimEnabled(bool on);

/// The user's override for one command, if any.
std::optional<QList<QKeySequence>> customKeys(const QString &id);
void setCustomKeys(const QString &id, const QList<QKeySequence> &keys);
void clearCustomKeys(const QString &id);
void resetAll();

/// Default keys for a command given the vim setting (before conflict resolution).
QList<QKeySequence> defaultKeys(const Command &c, bool vim);
/// Keys actually bound, for every command, after applying overrides and conflict rules.
QHash<QString, QList<QKeySequence>> effectiveKeys();
/// Same, for unsaved settings: [vim] and an [override] lookup (nullopt = use the defaults).
QHash<QString, QList<QKeySequence>> resolveKeys(
    bool vim, const std::function<std::optional<QList<QKeySequence>>(const QString &id)> &override);
/// Vim-style display: "j", "G" (Shift+g), "g g", "Ctrl+D".
QString keyName(const QKeySequence &key);
/// Several keys, e.g. "j  ·  Down".
QString describe(const QList<QKeySequence> &keys);

} // namespace Commands
