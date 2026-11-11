#include "ui/Commands.h"

#include <QSettings>
#include <QSet>
#include <optional>

namespace {

using K = QKeySequence;

QList<Command> buildCommands()
{
    // Vim keys follow zathura, the vim-style PDF reader, where it has an equivalent.
    return {
        // Navigation
        {"scrollDown", "Navigation", "Scroll down", {K(Qt::Key_Down)}, {K("J")}},
        {"scrollUp", "Navigation", "Scroll up", {K(Qt::Key_Up)}, {K("K")}},
        {"scrollLeft", "Navigation", "Scroll left", {K(Qt::Key_Left)}, {K("H")}},
        {"scrollRight", "Navigation", "Scroll right", {K(Qt::Key_Right)}, {K("L")}},
        {"halfPageDown", "Navigation", "Half a screen down", {}, {K("Ctrl+D")}},
        {"halfPageUp", "Navigation", "Half a screen up", {}, {K("Ctrl+U")}},
        {"screenDown", "Navigation", "One screen down", {K(Qt::Key_PageDown), K(Qt::Key_Space)}, {}},
        {"screenUp", "Navigation", "One screen up", {K(Qt::Key_PageUp), K("Shift+Space")}, {}},
        {"nextPage", "Navigation", "Next page", {}, {K("Shift+J")}},
        {"prevPage", "Navigation", "Previous page", {}, {K("Shift+K")}},
        {"firstPage", "Navigation", "First page", {K(Qt::Key_Home), K("Ctrl+Home")}, {K("G, G")}},
        {"lastPage", "Navigation", "Last page", {K(Qt::Key_End), K("Ctrl+End")}, {K("Shift+G")}},
        {"goToPage", "Navigation", "Go to page…", {K("Ctrl+G")}, {K(":")}},
        {"jumpBack", "Navigation", "Jump back (after a link, search or go-to)", {K("Alt+Left")}, {K("Ctrl+O")}},
        {"jumpForward", "Navigation", "Jump forward", {K("Alt+Right")}, {K("Ctrl+I")}},
        {"setMark", "Navigation", "Set a mark (then press a letter)", {}, {K("M")}},
        {"jumpToMark", "Navigation", "Go to a mark (then press its letter)", {}, {K("'")}},
        {"contents", "Navigation", "Table of contents", {K(Qt::Key_F9)}, {}},

        // Search & links
        {"find", "Search & links", "Search in document", {K(QKeySequence::Find)}, {K("/")}},
        {"findBackward", "Search & links", "Search backwards", {}, {K("?")}},
        {"findNext", "Search & links", "Next match", {K(Qt::Key_F3)}, {K("N")}},
        {"findPrev", "Search & links", "Previous match", {K("Shift+F3")}, {K("Shift+N")}},
        {"followLink", "Search & links", "Follow a link (shows letter hints)", {K("Ctrl+Shift+F")}, {K("F")}},
        {"smartSearch", "Search & links", "Smart search (ask your PDFs)", {K("Ctrl+K")}, {}},
        {"lookupWord", "Search & links", "Look up a word in the dictionary…", {K("Ctrl+L")}, {}},
        {"dictionaryPanel", "Search & links", "Show/hide dictionary panel", {K("Ctrl+Shift+D")}, {}},

        // View
        {"zoomIn", "View", "Zoom in", {K(QKeySequence::ZoomIn)}, {K("+")}},
        {"zoomOut", "View", "Zoom out", {K(QKeySequence::ZoomOut)}, {K("-")}},
        {"fitWidth", "View", "Fit width", {K("Ctrl+0")}, {K("="), K("S")}},
        {"rotate", "View", "Rotate 90°", {K("Ctrl+R")}, {K("R")}},
        {"fullScreen", "View", "Full screen", {K(Qt::Key_F11)}, {}},
        {"focusMode", "View", "Focus mode (hide everything but the PDF)", {K(Qt::Key_F8)}, {}},

        // Editing & tools
        {"undo", "Editing & tools", "Undo", {K(QKeySequence::Undo)}, {K("U")}},
        {"redo", "Editing & tools", "Redo", {K("Ctrl+Shift+Z"), K("Ctrl+Y")}, {K("Ctrl+R")}},
        {"clearPage", "Editing & tools", "Clear page", {}, {}},
        {"penTool", "Editing & tools", "Pen", {K("1"), K("P")}, {}},
        {"highlighterTool", "Editing & tools", "Highlighter", {K("2"), K("H")}, {}},
        {"eraserTool", "Editing & tools", "Eraser", {K("3"), K("E")}, {}},
        {"lookupTool", "Editing & tools", "Dictionary tool (click a word)", {K("4"), K("D")}, {}},
        {"mouseDraws", "Editing & tools", "Mouse draws on/off", {K("5"), K("M")}, {}},

        // Tabs & app
        {"openPdf", "Tabs & app", "Open PDF…", {K(QKeySequence::Open)}, {K("O")}},
        {"home", "Tabs & app", "Home tab", {K("Alt+Home")}, {K("G, H")}},
        {"nextTab", "Tabs & app", "Next tab", {K("Ctrl+Tab"), K("Ctrl+PgDown")}, {K("G, T")}},
        {"prevTab", "Tabs & app", "Previous tab", {K("Ctrl+Shift+Tab"), K("Ctrl+PgUp")}, {K("G, Shift+T")}},
        {"closeTab", "Tabs & app", "Close tab", {K(QKeySequence::Close)}, {}},
        {"tab1", "Tabs & app", "Go to tab 1 (Home)", {K("Alt+1")}, {}},
        {"tab2", "Tabs & app", "Go to tab 2", {K("Alt+2")}, {}},
        {"tab3", "Tabs & app", "Go to tab 3", {K("Alt+3")}, {}},
        {"tab4", "Tabs & app", "Go to tab 4", {K("Alt+4")}, {}},
        {"tab5", "Tabs & app", "Go to tab 5", {K("Alt+5")}, {}},
        {"tab6", "Tabs & app", "Go to tab 6", {K("Alt+6")}, {}},
        {"tab7", "Tabs & app", "Go to tab 7", {K("Alt+7")}, {}},
        {"tab8", "Tabs & app", "Go to tab 8", {K("Alt+8")}, {}},
        {"tab9", "Tabs & app", "Go to the last tab", {K("Alt+9")}, {}},
        {"renamePdf", "Tabs & app", "Rename PDF…", {K(Qt::Key_F2)}, {}},
        {"myWords", "Tabs & app", "My Words", {K("Ctrl+M")}, {}},
        {"sync", "Tabs & app", "Sync now", {K(Qt::Key_F5)}, {}},
        {"settings", "Tabs & app", "Settings…", {K("Ctrl+,")}, {}},
        {"shortcutHelp", "Tabs & app", "Keyboard shortcuts", {K(Qt::Key_F1)}, {}},
        {"quit", "Tabs & app", "Quit", {K(QKeySequence::Quit)}, {}},
    };
}

QString settingsKey(const QString &id) { return "shortcuts/" + id; }

} // namespace

namespace Commands {

const QList<Command> &all()
{
    static const QList<Command> commands = buildCommands();
    return commands;
}

const Command *find(const QString &id)
{
    for (const Command &c : all())
        if (c.id == id) return &c;
    return nullptr;
}

bool vimEnabled() { return QSettings().value("keys/vim", true).toBool(); }
void setVimEnabled(bool on) { QSettings().setValue("keys/vim", on); }

std::optional<QList<QKeySequence>> customKeys(const QString &id)
{
    QSettings s;
    if (!s.contains(settingsKey(id)))
        return std::nullopt;
    QList<QKeySequence> keys;
    for (const QString &k : s.value(settingsKey(id)).toStringList())
        if (const QKeySequence seq(k, QKeySequence::PortableText); !seq.isEmpty()) keys << seq;
    return keys;
}

void setCustomKeys(const QString &id, const QList<QKeySequence> &keys)
{
    QStringList list;
    for (const QKeySequence &k : keys)
        if (!k.isEmpty()) list << k.toString(QKeySequence::PortableText);
    QSettings().setValue(settingsKey(id), list);
}

void clearCustomKeys(const QString &id) { QSettings().remove(settingsKey(id)); }

void resetAll() { QSettings().remove("shortcuts"); }

QList<QKeySequence> defaultKeys(const Command &c, bool vim)
{
    return vim ? c.vimKeys + c.standardKeys : c.standardKeys;
}

QHash<QString, QList<QKeySequence>> effectiveKeys()
{
    return resolveKeys(vimEnabled(), [](const QString &id) { return customKeys(id); });
}

QHash<QString, QList<QKeySequence>> resolveKeys(
    bool vim, const std::function<std::optional<QList<QKeySequence>>(const QString &id)> &override)
{
    QHash<QString, QList<QKeySequence>> out;
    QHash<QKeySequence, QString> owner; // key -> command that keeps it
    QSet<QString> custom;

    // Pass 1: customised commands and vim keys claim their keys first.
    for (const Command &c : all()) {
        if (const auto keys = override(c.id)) {
            out[c.id] = *keys;
            custom.insert(c.id);
            for (const QKeySequence &k : *keys) owner.insert(k, c.id);
        } else if (vim) {
            for (const QKeySequence &k : c.vimKeys) {
                if (owner.contains(k)) continue;
                out[c.id] << k;
                owner.insert(k, c.id);
            }
        }
    }
    // Pass 2: standard keys, unless already taken.
    for (const Command &c : all()) {
        if (custom.contains(c.id)) continue;
        for (const QKeySequence &k : c.standardKeys) {
            if (owner.contains(k)) continue;
            out[c.id] << k;
            owner.insert(k, c.id);
        }
    }
    return out;
}

QString keyName(const QKeySequence &key)
{
    QStringList chords;
    for (int i = 0; i < key.count(); ++i) {
        const QKeyCombination combo = key[i];
        const int k = int(combo.key());
        const Qt::KeyboardModifiers mods = combo.keyboardModifiers();
        if (k >= Qt::Key_A && k <= Qt::Key_Z && (mods == Qt::NoModifier || mods == Qt::ShiftModifier)) {
            const QChar letter = QChar('a' + (k - Qt::Key_A));
            chords << (mods == Qt::ShiftModifier ? letter.toUpper() : letter); // vim: G means Shift+g
        } else {
            chords << QKeySequence(combo).toString(QKeySequence::NativeText);
        }
    }
    return chords.join(' ');
}

QString describe(const QList<QKeySequence> &keys)
{
    QStringList parts;
    for (const QKeySequence &k : keys)
        parts << keyName(k);
    return parts.isEmpty() ? QStringLiteral("—") : parts.join("  ·  ");
}

} // namespace Commands
