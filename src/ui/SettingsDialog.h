#pragma once

#include <QDialog>
#include <QHash>
#include <QKeySequence>
#include <optional>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QTreeWidget;
class QTreeWidgetItem;

/// Preferences: keyboard shortcuts (with vim mode), reading, dictionary and sync options.
/// Nothing is saved until OK/Apply.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = nullptr);

signals:
    /// Settings were saved; re-read them.
    void applied();

private:
    QWidget *buildKeyboardPage();
    QWidget *buildReadingPage();
    QWidget *buildDictionaryPage();
    QWidget *buildSyncPage();
    /// Keys as they'd be bound with the dialog's unsaved state.
    QHash<QString, QList<QKeySequence>> resolvedKeys() const;
    /// The command's own keys (custom or default), before conflict resolution — what the editor shows.
    QList<QKeySequence> ownKeys(const QString &id) const;
    std::optional<QList<QKeySequence>> overrideFor(const QString &id) const;
    void refreshKeys();
    void editCommand(QTreeWidgetItem *item);
    void apply();

    QCheckBox *m_vim;
    QLineEdit *m_keyFilter;
    QTreeWidget *m_keys;
    QSpinBox *m_scrollStep;
    QLineEdit *m_hintChars;
    QCheckBox *m_restoreTabs;
    QComboBox *m_dictTab;
    QSpinBox *m_syncInterval;
    QCheckBox *m_syncOnClose;

    /// Unsaved shortcut edits: a key list, or nullopt for "back to default".
    QHash<QString, std::optional<QList<QKeySequence>>> m_pending;
    bool m_resetAll = false;
};
