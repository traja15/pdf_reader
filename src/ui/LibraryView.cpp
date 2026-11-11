#include "ui/LibraryView.h"

#include "core/AppContext.h"
#include "core/DocumentStore.h"
#include "core/VocabStore.h"
#include "net/GoogleAuth.h"
#include "ui/Commands.h"
#include "ui/Theme.h"
#include "ui/Thumbnails.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <functional>

namespace {

const QString kAll = "__all";
const QString kUnfiled = "__unfiled";
constexpr int kIdRole = Qt::UserRole;
constexpr int kKindRole = Qt::UserRole + 1; ///< "doc" or "folder"

QLabel *heading(const QString &text, qreal scale)
{
    auto *label = new QLabel(text);
    QFont f = label->font();
    f.setPointSizeF(f.pointSizeF() * scale);
    f.setBold(true);
    label->setFont(f);
    return label;
}

QIcon icon(const QString &name) { return Theme::icon(name); }

} // namespace

LibraryView::LibraryView(AppContext &ctx, QWidget *parent)
    : QWidget(parent), m_ctx(ctx), m_thumbs(new Thumbnails(this)), m_folder(QSettings().value("ui/homeFolder", kAll).toString())
{
    m_banner = new QFrame;
    m_banner->setStyleSheet("QFrame { background: palette(alternate-base); border-radius: 8px; }");
    auto *bannerText = new QLabel("Sign in with Google to keep your PDFs, folders, notes and saved words on all your "
                                  "devices — including the PenReader Android app.");
    bannerText->setWordWrap(true);
    auto *signIn = new QPushButton("Sign in");
    connect(signIn, &QPushButton::clicked, this, &LibraryView::signInRequested);
    auto *bannerLayout = new QHBoxLayout(m_banner);
    bannerLayout->addWidget(bannerText, 1);
    bannerLayout->addWidget(signIn);

    // ---- Folder tree (left)
    m_folders = new QTreeWidget;
    m_folders->setHeaderHidden(true);
    m_folders->setIconSize(QSize(20, 20));
    m_folders->setContextMenuPolicy(Qt::CustomContextMenu);
    m_folders->setAcceptDrops(true);
    m_folders->viewport()->installEventFilter(this); // drops of PDFs onto folders
    m_folders->installEventFilter(this);
    connect(m_folders, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        if (item) selectFolder(item->data(0, kIdRole).toString());
    });
    connect(m_folders, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QTreeWidgetItem *item = m_folders->itemAt(pos);
        const QString id = item ? item->data(0, kIdRole).toString() : QString();
        showFolderMenu(id == kAll || id == kUnfiled ? QString() : id, m_folders->viewport()->mapToGlobal(pos));
    });
    auto *newFolder = new QPushButton(icon("folder-plus"), "New folder");
    connect(newFolder, &QPushButton::clicked, this, [this] { newFolderDialog(currentFolderId()); });
    auto *foldersCol = new QVBoxLayout;
    foldersCol->setContentsMargins(0, 0, 0, 0);
    foldersCol->addWidget(heading("Folders", 1.25));
    foldersCol->addWidget(m_folders, 1);
    foldersCol->addWidget(newFolder);
    auto *foldersPane = new QWidget;
    foldersPane->setLayout(foldersCol);

    // ---- Current folder (middle)
    m_up = new QPushButton(icon("back"), "Up");
    m_up->setToolTip("Parent folder (h)");
    connect(m_up, &QPushButton::clicked, this, &LibraryView::goUp);
    m_crumb = new QLabel;
    QFont cf = m_crumb->font();
    cf.setPointSizeF(cf.pointSizeF() * 1.25);
    cf.setBold(true);
    m_crumb->setFont(cf);
    m_search = new QLineEdit;
    m_search->setPlaceholderText("Search all PDFs…");
    m_search->setClearButtonEnabled(true);
    m_search->setMaximumWidth(280);
    connect(m_search, &QLineEdit::textChanged, this, &LibraryView::refreshDocuments);
    auto *crumbRow = new QHBoxLayout;
    crumbRow->addWidget(m_up);
    crumbRow->addWidget(m_crumb, 1);
    crumbRow->addWidget(m_search);

    // A grid of square tiles: the page each PDF is open at, with its name underneath.
    m_list = new QListWidget;
    m_list->setViewMode(QListView::IconMode);
    m_list->setMovement(QListView::Static);
    m_list->setResizeMode(QListView::Adjust);
    m_list->setIconSize(QSize(150, 150));
    m_list->setGridSize(QSize(178, 226));
    m_list->setSpacing(6);
    m_list->setWordWrap(true);
    m_list->setUniformItemSizes(true);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->setDragEnabled(true); // after setMovement(Static), which turns dragging off
    m_list->setDragDropMode(QAbstractItemView::DragOnly);
    m_list->setDefaultDropAction(Qt::CopyAction);
    m_list->installEventFilter(this);
    connect(m_list, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        if (!(item->flags() & Qt::ItemIsEnabled)) return;
        if (item->data(kKindRole) == "folder") selectFolder(item->data(kIdRole).toString());
        else emit openRequested(item->data(kIdRole).toString());
    });
    connect(m_list, &QListWidget::customContextMenuRequested, this, &LibraryView::showDocumentMenu);
    m_empty = new QLabel;
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setWordWrap(true);
    m_empty->setStyleSheet("color: gray;");
    auto *docsCol = new QVBoxLayout;
    docsCol->setContentsMargins(0, 0, 0, 0);
    docsCol->addLayout(crumbRow);
    docsCol->addWidget(new QLabel("<span style='color:gray'>Double-click to open · drag PDFs onto a folder to file "
                                  "them · right-click for more</span>"));
    docsCol->addWidget(m_list, 1);
    docsCol->addWidget(m_empty, 1);
    auto *docsPane = new QWidget;
    docsPane->setLayout(docsCol);

    // ---- Recently learnt words (right)
    m_wordsTitle = heading("Recently learnt words", 1.25);
    m_words = new QListWidget;
    m_words->setWordWrap(true);
    m_words->setSpacing(2);
    m_words->installEventFilter(this);
    connect(m_words, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        emit wordRequested(item->data(kIdRole).toString());
    });
    auto *allWords = new QPushButton("See all in My Words →");
    allWords->setFlat(true);
    connect(allWords, &QPushButton::clicked, this, &LibraryView::myWordsRequested);
    auto *wordsCol = new QVBoxLayout;
    wordsCol->setContentsMargins(0, 0, 0, 0);
    wordsCol->addWidget(m_wordsTitle);
    wordsCol->addWidget(m_words, 1);
    wordsCol->addWidget(allWords, 0, Qt::AlignRight);
    auto *wordsPane = new QWidget;
    wordsPane->setLayout(wordsCol);

    auto *splitter = new QSplitter;
    splitter->addWidget(foldersPane);
    splitter->addWidget(docsPane);
    splitter->addWidget(wordsPane);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 5);
    splitter->setStretchFactor(2, 3);
    splitter->setSizes({230, 600, 330});

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 12, 20, 12);
    layout->addWidget(m_banner);
    layout->addWidget(splitter, 1);

    // Sync can change many rows quickly; refresh at most every 150 ms.
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(150);
    connect(&m_refreshTimer, &QTimer::timeout, this, &LibraryView::refresh);
    connect(m_ctx.docs, &DocumentStore::libraryChanged, &m_refreshTimer, qOverload<>(&QTimer::start));
    connect(m_ctx.auth, &GoogleAuth::signedInChanged, this, &LibraryView::refresh);
    connect(m_ctx.vocab, &VocabStore::changed, this, &LibraryView::refreshWords);
    connect(m_thumbs, &Thumbnails::ready, this, [this](const QString &docKey) {
        const auto doc = m_ctx.docs->get(docKey);
        for (int i = 0; doc && i < m_list->count(); ++i) {
            QListWidgetItem *item = m_list->item(i);
            if (item->data(kKindRole) == "doc" && item->data(kIdRole) == docKey) item->setIcon(m_thumbs->icon(*doc));
        }
    });
    refresh();
    refreshWords();
}

QString LibraryView::currentFolderId() const
{
    return m_folder == kAll || m_folder == kUnfiled ? QString() : m_folder;
}

void LibraryView::setOpenDocuments(const QSet<QString> &docKeys)
{
    m_open = docKeys;
    refreshDocuments();
}

void LibraryView::refresh()
{
    m_banner->setVisible(!m_ctx.auth->isSignedIn());
    if (m_folder != kAll && m_folder != kUnfiled && !m_ctx.docs->folderExists(m_folder))
        m_folder = kAll; // deleted here or on another device
    refreshFolders();
    refreshDocuments();
}

void LibraryView::selectFolder(const QString &id)
{
    if (id.isEmpty() || id == m_folder)
        return;
    m_folder = id;
    QSettings().setValue("ui/homeFolder", id);
    m_search->clear();
    refreshFolders();
    refreshDocuments();
}

void LibraryView::goUp()
{
    if (const auto f = m_ctx.docs->folder(m_folder))
        selectFolder(m_ctx.docs->folderExists(f->parentId) ? f->parentId : kAll);
}

// ---------------------------------------------------------------- folder tree

void LibraryView::refreshFolders()
{
    const QVector<Folder> folders = m_ctx.docs->folders();
    const QVector<Document> docs = m_ctx.docs->library();
    QSet<QString> live;
    for (const Folder &f : folders) live.insert(f.id);

    // PDF counts, including subfolders.
    QHash<QString, QString> parentOf;
    for (const Folder &f : folders) parentOf.insert(f.id, live.contains(f.parentId) ? f.parentId : QString());
    QHash<QString, int> counts;
    int unfiled = 0;
    for (const Document &d : docs) {
        if (!live.contains(d.folderId)) { ++unfiled; continue; }
        int depth = 0;
        for (QString f = d.folderId; !f.isEmpty() && depth < 32; f = parentOf.value(f), ++depth) ++counts[f];
    }

    QSet<QString> expanded;
    for (QTreeWidgetItemIterator it(m_folders); *it; ++it)
        if ((*it)->isExpanded()) expanded.insert((*it)->data(0, kIdRole).toString());
    const bool firstFill = m_folders->topLevelItemCount() == 0;

    const QSignalBlocker block(m_folders);
    m_folders->clear();
    auto add = [&](QTreeWidgetItem *parent, const QString &id, const QString &name, const QString &iconName, int count) {
        auto *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(m_folders);
        item->setText(0, count > 0 ? QStringLiteral("%1  (%2)").arg(name).arg(count) : name);
        item->setIcon(0, icon(iconName));
        item->setData(0, kIdRole, id);
        item->setToolTip(0, name);
        return item;
    };
    add(nullptr, kAll, "All PDFs", "library", int(docs.size()));
    add(nullptr, kUnfiled, "Unfiled", "inbox", unfiled);

    QSet<QString> placed; // guards against parent loops in synced data
    std::function<void(QTreeWidgetItem *, const QString &)> addChildren = [&](QTreeWidgetItem *into, const QString &parentId) {
        for (const Folder &f : folders) {
            if (parentOf.value(f.id) != parentId || placed.contains(f.id)) continue;
            placed.insert(f.id);
            QTreeWidgetItem *item = add(into, f.id, f.name, "folder", counts.value(f.id));
            addChildren(item, f.id);
            item->setExpanded(firstFill || expanded.contains(f.id));
        }
    };
    addChildren(nullptr, {});

    for (QTreeWidgetItemIterator it(m_folders); *it; ++it) {
        if ((*it)->data(0, kIdRole).toString() == m_folder) {
            m_folders->setCurrentItem(*it);
            for (QTreeWidgetItem *p = (*it)->parent(); p; p = p->parent()) p->setExpanded(true);
        }
    }
}

// ---------------------------------------------------------------- folder contents

void LibraryView::refreshDocuments()
{
    const QString query = m_search->text().trimmed();
    const bool searching = !query.isEmpty();
    const QVector<Document> docs = m_ctx.docs->library();

    if (searching) m_crumb->setText(QStringLiteral("Search: “%1”").arg(query));
    else if (m_folder == kAll) m_crumb->setText("All PDFs");
    else if (m_folder == kUnfiled) m_crumb->setText("Unfiled");
    else m_crumb->setText(m_ctx.docs->folderPath(m_folder));
    m_up->setVisible(!searching && !currentFolderId().isEmpty());

    const QStringList selectedKeys = selectedDocKeys();
    const QSet<QString> selected(selectedKeys.begin(), selectedKeys.end());
    const QSignalBlocker block(m_list);
    m_list->clear();

    // Subfolders first when browsing a folder.
    if (!searching && !currentFolderId().isEmpty()) {
        for (const Folder &f : m_ctx.docs->folders()) {
            if (f.parentId != m_folder) continue;
            int count = 0;
            for (const Document &d : docs) count += d.folderId == f.id;
            auto *item = new QListWidgetItem(icon("folder"), f.name + "\n" +
                                             (count == 1 ? QString("1 PDF") : QStringLiteral("%1 PDFs").arg(count)));
            item->setData(kIdRole, f.id);
            item->setData(kKindRole, "folder");
            item->setToolTip(m_ctx.docs->folderPath(f.id));
            m_list->addItem(item);
        }
    }

    for (const Document &d : docs) {
        const bool filed = m_ctx.docs->folderExists(d.folderId);
        if (searching) {
            if (!d.title.contains(query, Qt::CaseInsensitive) &&
                !(filed && m_ctx.docs->folderPath(d.folderId).contains(query, Qt::CaseInsensitive)))
                continue;
        } else if (m_folder == kUnfiled) {
            if (filed) continue;
        } else if (m_folder != kAll) {
            if (d.folderId != m_folder) continue;
        }

        const int percent = d.pageCount > 0 ? qRound(100.0 * (d.lastPage + d.lastOffset) / d.pageCount) : 0;
        QString status = QStringLiteral("Page %1 of %2 · %3% read").arg(d.lastPage + 1).arg(d.pageCount).arg(percent);
        if ((searching || m_folder == kAll) && filed)
            status += " · 📁 " + m_ctx.docs->folderPath(d.folderId);
        bool enabled = true;
        QString badge = QStringLiteral("%1%").arg(percent); // short line under the name
        if (m_open.contains(d.docKey)) {
            status += " · open in a tab";
            badge += " · open";
        } else if (!d.onDevice() && d.inCloud()) {
            status += " · in the cloud, downloads when opened";
            badge = "☁ in the cloud";
        } else if (!d.onDevice()) {
            status += " · waiting for the other device to upload it";
            badge = "waiting for upload";
            enabled = false;
        } else if (!d.inCloud() && m_ctx.auth->isSignedIn()) {
            status += " · uploading…";
        }
        auto *item = new QListWidgetItem(m_thumbs->icon(d), d.title + "\n" + badge);
        item->setData(kIdRole, d.docKey);
        item->setData(kKindRole, "doc");
        item->setToolTip(d.title + "\n" + status);
        if (m_open.contains(d.docKey)) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        }
        if (!enabled)
            item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
        m_list->addItem(item);
        if (selected.contains(d.docKey))
            item->setSelected(true);
    }

    const bool empty = m_list->count() == 0;
    m_list->setVisible(!empty);
    m_empty->setVisible(empty);
    if (searching)
        m_empty->setText("No PDFs match your search.");
    else if (docs.isEmpty())
        m_empty->setText("No PDFs yet.\n\nClick “Open PDF” or drop PDFs onto this window. Create folders on the left "
                         "to organise them — your folders appear on all your devices.");
    else if (m_folder == kUnfiled)
        m_empty->setText("Every PDF is in a folder.");
    else
        m_empty->setText("This folder is empty.\n\nDrag PDFs onto it in the folder list, or open a PDF while "
                         "this folder is selected.");
}

QString LibraryView::selectedDocument() const
{
    const QStringList keys = selectedDocKeys();
    return keys.size() == 1 ? keys.first() : QString();
}

void LibraryView::renameDocument(const QString &docKey)
{
    const auto doc = m_ctx.docs->get(docKey);
    if (!doc)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, "Rename PDF", "Name (changes on all your devices):",
                                               QLineEdit::Normal, doc->title, &ok);
    if (ok && !name.trimmed().isEmpty())
        m_ctx.docs->renameDocument(docKey, name);
}

QStringList LibraryView::selectedDocKeys() const
{
    QStringList keys;
    for (QListWidgetItem *item : m_list->selectedItems())
        if (item->data(kKindRole) == "doc") keys << item->data(kIdRole).toString();
    return keys;
}

// ---------------------------------------------------------------- folder actions

QString LibraryView::newFolderDialog(const QString &parentId)
{
    const QString where = parentId.isEmpty() ? QString("at the top level")
                                             : "inside “" + m_ctx.docs->folderPath(parentId) + "”";
    bool ok = false;
    const QString name = QInputDialog::getText(this, "New folder", "Folder name (" + where + "):",
                                               QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || name.isEmpty())
        return {};
    return m_ctx.docs->createFolder(name, parentId).id;
}

void LibraryView::addMoveMenu(QMenu *menu, const QStringList &docKeys, const QString &movingFolder)
{
    QMenu *move = menu->addMenu(icon("folder"), "Move to");
    auto moveTo = [this, docKeys, movingFolder](const QString &target) {
        if (!movingFolder.isEmpty()) {
            if (!m_ctx.docs->moveFolder(movingFolder, target))
                QMessageBox::information(this, "Move folder", "A folder can't be moved inside itself.");
        } else {
            for (const QString &key : docKeys) m_ctx.docs->moveDocument(key, target);
        }
    };
    move->addAction(icon(movingFolder.isEmpty() ? "inbox" : "library"),
                    movingFolder.isEmpty() ? "Unfiled" : "Top level", this, [moveTo] { moveTo({}); });
    move->addSeparator();

    const QVector<Folder> folders = m_ctx.docs->folders();
    QSet<QString> live;
    for (const Folder &f : folders) live.insert(f.id);
    QSet<QString> placed;
    std::function<void(const QString &, int)> addLevel = [&](const QString &parentId, int depth) {
        for (const Folder &f : folders) {
            const QString p = live.contains(f.parentId) ? f.parentId : QString();
            if (p != parentId || f.id == movingFolder || placed.contains(f.id)) continue; // not inside itself
            placed.insert(f.id);
            move->addAction(icon("folder"), QString(depth * 4, ' ') + f.name, this, [moveTo, id = f.id] { moveTo(id); });
            addLevel(f.id, depth + 1);
        }
    };
    addLevel({}, 0);
    if (movingFolder.isEmpty()) {
        move->addSeparator();
        move->addAction(icon("folder-plus"), "New folder…", this, [this, docKeys] {
            const QString id = newFolderDialog(currentFolderId());
            if (!id.isEmpty())
                for (const QString &key : docKeys) m_ctx.docs->moveDocument(key, id);
        });
    }
}

void LibraryView::showDocumentMenu(const QPoint &pos)
{
    QListWidgetItem *item = m_list->itemAt(pos);
    if (!item) {
        QMenu menu(this);
        menu.addAction(icon("folder-plus"), "New folder…", this, [this] { newFolderDialog(currentFolderId()); });
        menu.exec(m_list->viewport()->mapToGlobal(pos));
        return;
    }
    if (item->data(kKindRole) == "folder") {
        showFolderMenu(item->data(kIdRole).toString(), m_list->viewport()->mapToGlobal(pos));
        return;
    }
    if (!item->isSelected()) {
        m_list->clearSelection();
        item->setSelected(true);
    }
    const QStringList keys = selectedDocKeys();
    QMenu menu(this);
    if (keys.size() == 1) {
        const auto doc = m_ctx.docs->get(keys.first());
        if (!doc) return;
        if (doc->onDevice() || doc->inCloud())
            menu.addAction("Open in a tab", this, [this, key = doc->docKey] { emit openRequested(key); });
        menu.addAction("Rename…", this, [this, key = doc->docKey] { renameDocument(key); });
        if (m_ctx.docs->folderExists(doc->folderId) && m_folder != doc->folderId)
            menu.addAction(icon("folder"), "Show in “" + m_ctx.docs->folderPath(doc->folderId) + "”", this,
                           [this, id = doc->folderId] { selectFolder(id); });
    }
    addMoveMenu(&menu, keys);
    menu.addSeparator();
    if (keys.size() == 1) {
        const auto doc = m_ctx.docs->get(keys.first());
        if (doc && doc->onDevice() && doc->inCloud() && !m_open.contains(doc->docKey))
            menu.addAction("Remove download (keep in cloud)", this, [this, key = doc->docKey] { m_ctx.docs->removeFromDevice(key); });
    }
    QStringList deletable;
    for (const QString &k : keys)
        if (!m_open.contains(k)) deletable << k;
    if (!deletable.isEmpty()) {
        const QString label = deletable.size() == 1 ? QString("Delete from all devices…")
                                                    : QStringLiteral("Delete %1 PDFs from all devices…").arg(deletable.size());
        menu.addAction(label, this, [this, deletable] {
            const auto first = m_ctx.docs->get(deletable.first());
            const QString what = deletable.size() == 1 && first ? "“" + first->title + "”"
                                                                : QStringLiteral("%1 PDFs").arg(deletable.size());
            if (QMessageBox::question(this, "Delete", "Delete " + what + "?\n\nThe PDF and your handwritten notes on it "
                                                      "will be removed from all your devices. Saved words are kept.")
                == QMessageBox::Yes)
                for (const QString &k : deletable) m_ctx.docs->deleteEverywhere(k);
        });
    } else {
        menu.addAction("Close its tab to delete it")->setEnabled(false);
    }
    menu.exec(m_list->viewport()->mapToGlobal(pos));
}

void LibraryView::showFolderMenu(const QString &folderId, const QPoint &globalPos)
{
    QMenu menu(this);
    if (folderId.isEmpty()) {
        menu.addAction(icon("folder-plus"), "New folder…", this, [this] { newFolderDialog({}); });
        menu.exec(globalPos);
        return;
    }
    menu.addAction("Open", this, [this, folderId] { selectFolder(folderId); });
    menu.addAction(icon("folder-plus"), "New subfolder…", this, [this, folderId] { newFolderDialog(folderId); });
    menu.addAction("Rename…", this, [this, folderId] {
        const auto f = m_ctx.docs->folder(folderId);
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Rename folder", "Name:", QLineEdit::Normal,
                                                   f ? f->name : QString(), &ok);
        if (ok && !name.trimmed().isEmpty()) m_ctx.docs->renameFolder(folderId, name);
    });
    addMoveMenu(&menu, {}, folderId);
    menu.addSeparator();
    menu.addAction("Delete folder…", this, [this, folderId] { deleteFolderAsk(folderId); });
    menu.exec(globalPos);
}

void LibraryView::deleteFolderAsk(const QString &folderId)
{
    const auto f = m_ctx.docs->folder(folderId);
    if (!f) return;
    const bool hasParent = m_ctx.docs->folderExists(f->parentId);
    const QString parent = hasParent ? "“" + m_ctx.docs->folderPath(f->parentId) + "”" : QString("Unfiled");
    if (QMessageBox::question(this, "Delete folder",
                              QStringLiteral("Delete the folder “%1”?\n\nNo PDFs are deleted: the PDFs and subfolders "
                                             "inside it move to %2. This applies on all your devices.").arg(f->name, parent))
        == QMessageBox::Yes) {
        if (m_folder == folderId) selectFolder(hasParent ? f->parentId : kAll);
        m_ctx.docs->deleteFolder(folderId);
    }
}

// ---------------------------------------------------------------- words

void LibraryView::refreshWords()
{
    const QVector<VocabWord> words = m_ctx.vocab->words();
    m_wordsTitle->setText(QStringLiteral("Recently learnt words (%1)").arg(words.size()));
    m_words->clear();
    if (words.isEmpty()) {
        auto *hint = new QListWidgetItem("Words you save from the dictionary appear here.");
        hint->setFlags(Qt::NoItemFlags);
        m_words->addItem(hint);
        return;
    }
    for (int i = 0; i < words.size() && i < 12; ++i) {
        const VocabWord &w = words[i];
        QString text = w.word;
        if (!w.tamil.isEmpty()) text += "  ·  " + w.tamil;
        text += "\n" + w.definition.left(110) + (w.definition.size() > 110 ? "…" : "");
        auto *item = new QListWidgetItem(text);
        item->setData(kIdRole, w.word);
        m_words->addItem(item);
    }
}

// ---------------------------------------------------------------- keyboard & drag-and-drop

bool LibraryView::eventFilter(QObject *watched, QEvent *event)
{
    // Drag PDFs (or folders) from the middle list onto a folder in the tree.
    if (watched == m_folders->viewport()) {
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove) {
            auto *e = static_cast<QDragMoveEvent *>(event);
            QTreeWidgetItem *target = m_folders->itemAt(e->position().toPoint());
            const QString id = target ? target->data(0, kIdRole).toString() : QString();
            if (e->source() == m_list && target && id != kAll) {
                e->setDropAction(Qt::MoveAction);
                e->accept();
            } else {
                e->ignore();
            }
            return true;
        }
        if (event->type() == QEvent::Drop) {
            auto *e = static_cast<QDropEvent *>(event);
            QTreeWidgetItem *target = m_folders->itemAt(e->position().toPoint());
            if (e->source() == m_list && target) {
                const QString id = target->data(0, kIdRole).toString();
                const QString folder = id == kUnfiled ? QString() : id;
                for (QListWidgetItem *item : m_list->selectedItems()) {
                    const QString key = item->data(kIdRole).toString();
                    if (item->data(kKindRole) == "folder") m_ctx.docs->moveFolder(key, folder);
                    else m_ctx.docs->moveDocument(key, folder);
                }
                e->accept();
            }
            return true;
        }
    }

    // Vim-style keys in the Home lists: j / k move, l / Enter open, h goes up a folder.
    if (event->type() == QEvent::KeyPress && Commands::vimEnabled()) {
        auto *key = static_cast<QKeyEvent *>(event);
        auto *view = qobject_cast<QAbstractItemView *>(watched);
        if (view && key->modifiers() == Qt::NoModifier) {
            auto forward = [view](Qt::Key k) {
                QKeyEvent synthetic(QEvent::KeyPress, k, Qt::NoModifier);
                QApplication::sendEvent(view, &synthetic);
                return true;
            };
            switch (key->key()) {
            case Qt::Key_J: return forward(Qt::Key_Down);
            case Qt::Key_K: return forward(Qt::Key_Up);
            case Qt::Key_L:
                if (view == m_folders) { m_list->setFocus(); return true; }
                return forward(Qt::Key_Return);
            case Qt::Key_H:
                if (view == m_list) {
                    if (!currentFolderId().isEmpty()) goUp();
                    else m_folders->setFocus();
                    return true;
                }
                return false;
            default:
                break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}
