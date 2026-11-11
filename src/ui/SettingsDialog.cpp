#include "ui/SettingsDialog.h"

#include "core/AppContext.h"
#include "core/SearchIndex.h"
#include "net/Ollama.h"
#include "ui/Commands.h"
#include "ui/Prefs.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QProgressDialog>
#include <QTimer>
#include <QtConcurrent>
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

SettingsDialog::SettingsDialog(AppContext &ctx, QWidget *parent) : QDialog(parent), m_ctx(ctx)
{
    setWindowTitle("Settings — PenReader");
    resize(760, 600);

    auto *tabs = new QTabWidget;
    tabs->addTab(buildAppearancePage(), "Appearance");
    tabs->addTab(buildKeyboardPage(), "Keyboard");
    tabs->addTab(buildReadingPage(), "Reading");
    tabs->addTab(buildDictionaryPage(), "Dictionary");
    tabs->addTab(buildSyncPage(), "Sync");
    tabs->addTab(buildSmartSearchPage(), "Smart search");

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

QWidget *SettingsDialog::buildAppearancePage()
{
    auto *page = new QWidget;
    m_theme = new QComboBox;
    m_theme->addItem("Dark", "dark");
    m_theme->addItem("Light", "light");
    m_theme->addItem("Follow the system", "system");
    m_theme->setCurrentIndex(int(Theme::mode() == Theme::Mode::Light ? 1 : Theme::mode() == Theme::Mode::System ? 2 : 0));

    m_pageMode = new QComboBox;
    m_pageMode->addItem("Dark — light text on a dark page", "dark");
    m_pageMode->addItem("Sepia — warm paper", "sepia");
    m_pageMode->addItem("Original — the PDF's own colours", "original");
    m_pageMode->addItem("Custom — choose text and background colours", "custom");
    m_pageMode->setCurrentIndex(qMax(0, m_pageMode->findData(Theme::pageMode())));

    const Theme::PageColors custom = Theme::colorsFor("custom");
    m_customText = custom.text;
    m_customBack = custom.background;
    m_textColorButton = new QPushButton("Text colour…");
    m_backColorButton = new QPushButton("Background colour…");
    connect(m_textColorButton, &QPushButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(m_customText, this, "Text (front) colour");
        if (c.isValid()) { m_customText = c; updateAppearancePreview(); }
    });
    connect(m_backColorButton, &QPushButton::clicked, this, [this] {
        const QColor c = QColorDialog::getColor(m_customBack, this, "Background (back) colour");
        if (c.isValid()) { m_customBack = c; updateAppearancePreview(); }
    });
    auto *colors = new QHBoxLayout;
    colors->addWidget(m_textColorButton);
    colors->addWidget(m_backColorButton);
    colors->addStretch();

    m_preview = new QLabel;
    m_preview->setWordWrap(true);
    m_preview->setMinimumHeight(110);
    m_preview->setMargin(14);
    connect(m_pageMode, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateAppearancePreview);

    auto *form = new QFormLayout(page);
    form->addRow("App theme:", m_theme);
    form->addRow("Reading colours:", m_pageMode);
    form->addRow("", colors);
    form->addRow("Preview:", m_preview);
    form->addRow("", new QLabel("<span style='color:gray'>Reading colours change how PDF pages look. Your handwriting "
                                "stays readable: on dark pages, dark ink is shown lighter, keeping its colour.</span>"));
    qobject_cast<QLabel *>(form->itemAt(form->rowCount() - 1, QFormLayout::FieldRole)->widget())->setWordWrap(true);
    updateAppearancePreview();
    return page;
}

void SettingsDialog::updateAppearancePreview()
{
    const QString mode = m_pageMode->currentData().toString();
    const bool custom = mode == "custom";
    m_textColorButton->setEnabled(custom);
    m_backColorButton->setEnabled(custom);
    auto swatch = [](const QColor &c) {
        QPixmap pm(16, 16);
        pm.fill(c);
        return QIcon(pm);
    };
    m_textColorButton->setIcon(swatch(m_customText));
    m_backColorButton->setIcon(swatch(m_customBack));
    const Theme::PageColors c = custom ? Theme::PageColors{false, m_customText, m_customBack} : Theme::colorsFor(mode);
    m_preview->setStyleSheet(QStringLiteral("QLabel { color: %1; background: %2; border: 1px solid palette(mid); "
                                            "font-family: serif; font-size: 13pt; }")
                                 .arg(c.text.name(), c.background.name()));
    m_preview->setText("<b>Chapter 1</b><br>The quick brown fox jumps over the lazy dog. "
                       "This is how your PDF pages will look.");
}

QWidget *SettingsDialog::buildSmartSearchPage()
{
    auto *page = new QWidget;
    m_aiUrl = new QLineEdit(AiPrefs::url());
    m_embedModel = new QComboBox;
    m_embedModel->setEditable(true);
    m_embedModel->addItems({"nomic-embed-text", "mxbai-embed-large", "bge-m3", "all-minilm"});
    m_embedModel->setCurrentText(AiPrefs::embeddingModel());
    m_embedModel->setToolTip("Turns passages into meaning vectors. Changing it re-processes every passage.");
    m_answerModel = new QComboBox;
    m_answerModel->setEditable(true);
    m_answerModel->addItems({"qwen2.5:3b", "qwen2.5:1.5b", "llama3.2:3b", "gemma3:4b", "qwen2.5:7b"});
    m_answerModel->setCurrentText(AiPrefs::answerModel());
    m_answerModel->setToolTip("Writes answers from the best passages. Smaller = faster, larger = better answers.");
    m_autoIndex = new QCheckBox("Index new PDFs automatically in the background");
    m_autoIndex->setChecked(AiPrefs::autoIndex());
    m_aiStatus = new QLabel;
    m_aiStatus->setWordWrap(true);

    auto *check = new QPushButton("Check Ollama");
    connect(check, &QPushButton::clicked, this, &SettingsDialog::checkOllama);
    auto *download = new QPushButton("Download models");
    connect(download, &QPushButton::clicked, this, &SettingsDialog::downloadModels);
    auto *rebuild = new QPushButton("Rebuild index");
    rebuild->setToolTip("Re-read every PDF from scratch");
    connect(rebuild, &QPushButton::clicked, this, [this] {
        if (QMessageBox::question(this, "Rebuild index", "Re-read and re-process every PDF? This can take a while "
                                                          "for a big library.") != QMessageBox::Yes)
            return;
        m_ctx.indexer->stop();
        QTimer::singleShot(500, this, [this] {
            m_ctx.search->clear();
            m_ctx.indexer->start();
            m_aiStatus->setText("Rebuilding the index in the background…");
        });
    });
    auto *buttons = new QHBoxLayout;
    buttons->addWidget(check);
    buttons->addWidget(download);
    buttons->addWidget(rebuild);
    buttons->addStretch();

    auto *form = new QFormLayout(page);
    form->addRow(new QLabel("Smart search runs open-source AI models on this computer with "
                            "<a href='https://ollama.com'>Ollama</a> — your PDFs never leave it. "
                            "Keyword search works even without Ollama."));
    qobject_cast<QLabel *>(form->itemAt(0)->widget())->setOpenExternalLinks(true);
    qobject_cast<QLabel *>(form->itemAt(0)->widget())->setWordWrap(true);
    form->addRow("Ollama address:", m_aiUrl);
    form->addRow("Embedding model:", m_embedModel);
    form->addRow("Answer model:", m_answerModel);
    form->addRow("", m_autoIndex);
    form->addRow("", buttons);
    form->addRow("", m_aiStatus);
    form->addRow("", new QLabel("<span style='color:gray'>Status: " + m_ctx.indexer->status().toHtmlEscaped() + "</span>"));
    QTimer::singleShot(0, this, &SettingsDialog::checkOllama);
    return page;
}

void SettingsDialog::checkOllama()
{
    QSettings().setValue("ai/url", m_aiUrl->text().trimmed()); // so the check uses what's typed
    m_aiStatus->setText("Checking…");
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher] {
        watcher->deleteLater();
        m_aiStatus->setText(watcher->result());
    });
    const QString embed = m_embedModel->currentText().trimmed(), answer = m_answerModel->currentText().trimmed();
    watcher->setFuture(QtConcurrent::run([embed, answer]() -> QString {
        try {
            const QStringList models = Ollama::installedModels();
            auto has = [&](const QString &m) { return models.contains(m) || models.contains(m + ":latest"); };
            QString s = "✅ Ollama is running. Installed: " + models.join(", ") + "<br>";
            s += (has(embed) ? "✅ " : "⬇️ Not downloaded yet: ") + embed + "<br>";
            s += (has(answer) ? "✅ " : "⬇️ Not downloaded yet: ") + answer;
            return s;
        } catch (const std::exception &e) {
            return QString("❌ Can't reach Ollama. Install it from ollama.com and start it (“ollama serve”).<br>"
                           "<span style='color:gray'>") + QString::fromStdString(e.what()).toHtmlEscaped() + "</span>";
        }
    }));
}

void SettingsDialog::downloadModels()
{
    QSettings().setValue("ai/url", m_aiUrl->text().trimmed());
    const QStringList models = {m_embedModel->currentText().trimmed(), m_answerModel->currentText().trimmed()};
    auto *progress = new QProgressDialog("Downloading models…", "Hide", 0, 1000, this);
    progress->setWindowTitle("Smart search models");
    progress->setMinimumDuration(0);
    progress->setValue(0);
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, progress] {
        watcher->deleteLater();
        progress->close();
        progress->deleteLater();
        const QString error = watcher->result();
        if (!error.isEmpty()) QMessageBox::warning(this, "Download models", error);
        checkOllama();
        m_ctx.indexer->start();
    });
    watcher->setFuture(QtConcurrent::run([models, progress]() -> QString {
        try {
            for (const QString &m : models) {
                Ollama::pull(m, [&](const QString &status, double fraction) {
                    QMetaObject::invokeMethod(progress, [progress, m, status, fraction] {
                        progress->setLabelText(QStringLiteral("%1: %2").arg(m, status));
                        if (fraction >= 0) progress->setValue(int(fraction * 1000));
                    });
                });
            }
            return {};
        } catch (const std::exception &e) {
            return QString::fromStdString(e.what());
        }
    }));
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
    const QString theme = m_theme->currentData().toString();
    Theme::setMode(theme == "light" ? Theme::Mode::Light : theme == "system" ? Theme::Mode::System : Theme::Mode::Dark);
    Theme::setCustomPageColors(m_customText, m_customBack);
    Theme::setPageMode(m_pageMode->currentData().toString());
    const bool embedChanged = m_embedModel->currentText().trimmed() != AiPrefs::embeddingModel();
    s.setValue("ai/url", m_aiUrl->text().trimmed());
    s.setValue("ai/embeddingModel", m_embedModel->currentText().trimmed());
    s.setValue("ai/answerModel", m_answerModel->currentText().trimmed());
    s.setValue("ai/autoIndex", m_autoIndex->isChecked());
    if (embedChanged) m_ctx.indexer->start(); // re-embed passages with the new model

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
