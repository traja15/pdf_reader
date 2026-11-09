#include "ui/SettingsDialog.h"

#include "ui/Commands.h"
#include "ui/Prefs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace {
constexpr int kIdRole = Qt::UserRole;
constexpr int kMaxKeys = 3;
}

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle("Settings — PenReader");
    resize(760, 600);

    auto *tabs = new QTabWidget;
    tabs->addTab(buildKeyboardPage(), "Keyboard");
    tabs->addTab(buildReadingPage(), "Reading");
    tabs->addTab(buildDictionaryPage(), "Dictionary");
    tabs->addTab(buildSyncPage(), "Sync");

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::Apply);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { apply(); accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, &SettingsDialog::apply);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);
}

// ---------------------------------------------------------------- pages

QWidget *SettingsDialog::buildKeyboardPage()
{
    auto *page = new QWidget;
    m_vim = new QCheckBox("Vim-style keys  (j/k scroll, gg/G, / search, n/N, f follow links, m/' marks, u undo…)");
    m_vim->setChecked(Commands::vimEnabled());
    connect(m_vim, &QCheckBox::toggled, this, &SettingsDialog::refreshKeys);

    m_keyFilter = new QLineEdit;
    m_keyFilter->setPlaceholderText("Filter commands or keys…");
    m_keyFilter->setClearButtonEnabled(true);
    connect(m_keyFilter, &QLineEdit::textChanged, this, &SettingsDialog::refreshKeys);

    m_keys = new QTreeWidget;
    m_keys->setColumnCount(2);
    m_keys->setHeaderLabels({"Command", "Shortcuts"});
    m_keys->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_keys->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_keys->setRootIsDecorated(false);
    m_keys->setAlternatingRowColors(true);
    connect(m_keys, &QTreeWidget::itemActivated, this, &SettingsDialog::editCommand);

    auto *edit = new QPushButton("Change…");
    connect(edit, &QPushButton::clicked, this, [this] { if (m_keys->currentItem()) editCommand(m_keys->currentItem()); });
    auto *reset = new QPushButton("Restore all defaults");
    connect(reset, &QPushButton::clicked, this, [this] {
        m_pending.clear();
        m_resetAll = true;
        refreshKeys();
    });
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(new QLabel("<span style='color:gray'>Double-click a command to change its keys. "
                                  "Red = the same key is used twice.</span>"), 1);
    buttons->addWidget(edit);
    buttons->addWidget(reset);

    auto *layout = new QVBoxLayout(page);
    layout->addWidget(m_vim);
    layout->addWidget(m_keyFilter);
    layout->addWidget(m_keys, 1);
    layout->addLayout(buttons);
    refreshKeys();
    return page;
}

QWidget *SettingsDialog::buildReadingPage()
{
    auto *page = new QWidget;
    m_scrollStep = new QSpinBox;
    m_scrollStep->setRange(10, 600);
    m_scrollStep->setSuffix(" px");
    m_scrollStep->setValue(Prefs::scrollStep());
    m_hintChars = new QLineEdit(Prefs::hintChars());
    m_hintChars->setToolTip("Letters used for link hints, most comfortable first (home row by default).");
    m_restoreTabs = new QCheckBox("Reopen the tabs that were open when PenReader closed");
    m_restoreTabs->setChecked(Prefs::restoreTabs());

    auto *form = new QFormLayout(page);
    form->addRow("Scroll step (j / k / arrows):", m_scrollStep);
    form->addRow("Link hint letters (f):", m_hintChars);
    form->addRow("", m_restoreTabs);
    return page;
}

QWidget *SettingsDialog::buildDictionaryPage()
{
    auto *page = new QWidget;
    m_dictTab = new QComboBox;
    m_dictTab->addItems({"English", "தமிழ் Tamil"});
    m_dictTab->setCurrentIndex(QSettings().value("ui/dictTab", 0).toInt());
    auto *form = new QFormLayout(page);
    form->addRow("Tab to show first:", m_dictTab);
    form->addRow("", new QLabel("<span style='color:gray'>Define a word by right-clicking or double-clicking it, "
                                "with the dictionary tool, or with Look up a word.</span>"));
    return page;
}

QWidget *SettingsDialog::buildSyncPage()
{
    auto *page = new QWidget;
    m_syncInterval = new QSpinBox;
    m_syncInterval->setRange(0, 120);
    m_syncInterval->setSuffix(" min");
    m_syncInterval->setSpecialValueText("Off");
    m_syncInterval->setValue(Prefs::syncInterval());
    m_syncOnClose = new QCheckBox("Sync before closing (waits up to 15 seconds)");
    m_syncOnClose->setChecked(Prefs::syncOnClose());
    auto *form = new QFormLayout(page);
    form->addRow("Sync automatically every:", m_syncInterval);
    form->addRow("", m_syncOnClose);
    form->addRow("", new QLabel("<span style='color:gray'>PenReader also syncs when it starts, after you open a PDF "
                                "or save a word, and when you close a tab.</span>"));
    return page;
}

// ---------------------------------------------------------------- keyboard

std::optional<QList<QKeySequence>> SettingsDialog::overrideFor(const QString &id) const
{
    if (const auto it = m_pending.find(id); it != m_pending.end())
        return *it; // a list, or nullopt = back to defaults
    if (!m_resetAll)
        return Commands::customKeys(id);
    return std::nullopt;
}

QHash<QString, QList<QKeySequence>> SettingsDialog::resolvedKeys() const
{
    return Commands::resolveKeys(m_vim->isChecked(), [this](const QString &id) { return overrideFor(id); });
}

QList<QKeySequence> SettingsDialog::ownKeys(const QString &id) const
{
    if (const auto keys = overrideFor(id))
        return *keys;
    return Commands::defaultKeys(*Commands::find(id), m_vim->isChecked());
}

void SettingsDialog::refreshKeys()
{
    const QString current = m_keys->currentItem() ? m_keys->currentItem()->data(0, kIdRole).toString() : QString();
    // Defaults never clash (vim keys win, see Commands); only the user's own edits can.
    const auto resolved = resolvedKeys();
    QHash<QKeySequence, QStringList> users;
    for (const Command &c : Commands::all())
        if (overrideFor(c.id))
            for (const QKeySequence &k : *overrideFor(c.id)) users[k] << c.name;
    for (const Command &c : Commands::all())
        if (!overrideFor(c.id))
            for (const QKeySequence &k : resolved.value(c.id))
                if (users.contains(k)) users[k] << c.name;

    const QString filter = m_keyFilter->text().trimmed();
    m_keys->clear();
    QString category;
    for (const Command &c : Commands::all()) {
        const QList<QKeySequence> keys = resolved.value(c.id);
        const QString keyText = Commands::describe(keys);
        if (!filter.isEmpty() && !c.name.contains(filter, Qt::CaseInsensitive) && !keyText.contains(filter, Qt::CaseInsensitive))
            continue;
        if (c.category != category) {
            category = c.category;
            auto *header = new QTreeWidgetItem(m_keys, {category});
            QFont f = header->font(0);
            f.setBold(true);
            header->setFont(0, f);
            header->setFlags(Qt::ItemIsEnabled);
            header->setFirstColumnSpanned(true);
        }
        auto *item = new QTreeWidgetItem(m_keys, {"    " + c.name, keyText});
        item->setData(0, kIdRole, c.id);
        QStringList clashes;
        for (const QKeySequence &k : keys)
            for (const QString &other : users.value(k))
                if (other != c.name) clashes << QStringLiteral("%1 is also used by “%2”").arg(Commands::keyName(k), other);
        if (!clashes.isEmpty()) {
            item->setForeground(1, QColor(0xC6, 0x28, 0x28));
            item->setToolTip(1, clashes.join("\n"));
        }
        if (overrideFor(c.id)) {
            QFont f = item->font(1);
            f.setBold(true);
            item->setFont(1, f);
            item->setToolTip(0, "Customised");
        }
        if (c.id == current)
            m_keys->setCurrentItem(item);
    }
}

void SettingsDialog::editCommand(QTreeWidgetItem *item)
{
    const QString id = item->data(0, kIdRole).toString();
    const Command *c = Commands::find(id);
    if (!c)
        return;

    QDialog dlg(this);
    dlg.setWindowTitle("Shortcut — " + c->name);
    auto *layout = new QVBoxLayout(&dlg);
    layout->addWidget(new QLabel(QStringLiteral("<b>%1</b><br><span style='color:gray'>Click a box and press the keys. "
                                                "Two-key sequences like <i>g t</i> work too.</span>").arg(c->name)));
    QList<QKeySequenceEdit *> edits;
    const QList<QKeySequence> keys = ownKeys(id);
    for (int i = 0; i < kMaxKeys; ++i) {
        auto *edit = new QKeySequenceEdit(i < keys.size() ? keys[i] : QKeySequence());
        edit->setMaximumSequenceLength(2);
        edit->setClearButtonEnabled(true);
        auto *row = new QHBoxLayout;
        row->addWidget(new QLabel(i == 0 ? "Shortcut:" : "Alternative:"));
        row->addWidget(edit, 1);
        layout->addLayout(row);
        edits << edit;
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults);
    const QString defaults = Commands::describe(Commands::defaultKeys(*c, m_vim->isChecked()));
    buttons->button(QDialogButtonBox::RestoreDefaults)->setToolTip("Default: " + defaults);
    layout->addWidget(new QLabel("<span style='color:gray'>Default: " + defaults.toHtmlEscaped() + "</span>"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    bool restore = false;
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, &dlg, [&] {
        restore = true;
        dlg.accept();
    });
    if (dlg.exec() != QDialog::Accepted)
        return;
    if (restore) {
        m_pending[id] = std::nullopt;
    } else {
        QList<QKeySequence> chosen;
        for (QKeySequenceEdit *e : edits)
            if (!e->keySequence().isEmpty() && !chosen.contains(e->keySequence())) chosen << e->keySequence();
        m_pending[id] = chosen;
    }
    refreshKeys();
}

void SettingsDialog::apply()
{
    QSettings s;
    s.setValue("reading/scrollStep", m_scrollStep->value());
    s.setValue("reading/hintChars", m_hintChars->text().trimmed().isEmpty() ? QString("asdfghjkl") : m_hintChars->text().trimmed());
    s.setValue("reading/restoreTabs", m_restoreTabs->isChecked());
    s.setValue("ui/dictTab", m_dictTab->currentIndex());
    s.setValue("sync/intervalMinutes", m_syncInterval->value());
    s.setValue("sync/onClose", m_syncOnClose->isChecked());

    Commands::setVimEnabled(m_vim->isChecked());
    if (m_resetAll)
        Commands::resetAll();
    for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
        if (it.value()) Commands::setCustomKeys(it.key(), *it.value());
        else Commands::clearCustomKeys(it.key());
    }
    m_pending.clear();
    m_resetAll = false;
    refreshKeys();
    emit applied();
}
