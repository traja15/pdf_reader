#include "ui/MainWindow.h"

#include "core/AppContext.h"
#include "core/DocumentStore.h"
#include "core/SearchIndex.h"
#include "net/Ollama.h"
#include "net/DriveApi.h"
#include "net/GoogleAuth.h"
#include "net/SyncManager.h"
#include "ui/Commands.h"
#include "ui/DefinitionPanel.h"
#include "ui/LibraryView.h"
#include "ui/Prefs.h"
#include "ui/ReaderView.h"
#include "ui/SettingsDialog.h"
#include "ui/SmartSearchView.h"
#include "ui/Theme.h"
#include "ui/VocabWindow.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFile>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPdfBookmarkModel>
#include <QPdfDocument>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {

QIcon icon(const QString &name) { return Theme::icon(name); }

QIcon swatch(quint32 argb)
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QColor c = QColor::fromRgba(argb);
    c.setAlpha(255);
    p.setBrush(c);
    p.setPen(QPen(Qt::gray, 1));
    p.drawEllipse(QRectF(2, 2, 18, 18));
    return QIcon(pm);
}

QIcon widthIcon(int level)
{
    QPixmap pm(22, 22);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(Theme::isDark() ? QColor(0xC9, 0xD1, 0xD6) : QColor(0x3c, 0x40, 0x43));
    p.setPen(Qt::NoPen);
    const qreal r = 2.5 + level * 2.5;
    p.drawEllipse(QPointF(11, 11), r, r);
    return QIcon(pm);
}

const QList<quint32> kPenColors = {0xFF000000, 0xFF1A237E, 0xFFC62828, 0xFF2E7D32, 0xFF6A1B9A};
const QList<quint32> kHighlightColors = {0x66FFEB3B, 0x6681D4FA, 0x66A5D6A7, 0x66F48FB1};
const QList<float> kPenWidths = {0.0016f, 0.0028f, 0.0045f};

/// Result of a background import/download: the docKey, or an error message.
struct OpenResult {
    QString docKey;
    QString error;
    bool needsSignIn = false;
};

QString shortTitle(const QString &title)
{
    return title.size() > 28 ? title.left(26) + "…" : title;
}

} // namespace

MainWindow::MainWindow(AppContext &ctx, QWidget *parent) : QMainWindow(parent), m_ctx(ctx)
{
    setWindowTitle("PenReader");
    setAcceptDrops(true);
    resize(1200, 860);

    m_home = new LibraryView(ctx);
    m_tabs = new QTabWidget;
    m_tabs->setDocumentMode(true);
    m_tabs->setTabsClosable(true);
    m_tabs->setMovable(true);
    m_tabs->tabBar()->setExpanding(false);
    m_tabs->tabBar()->setElideMode(Qt::ElideRight);
    m_tabs->addTab(m_home, icon("home"), "Home");
    m_tabs->tabBar()->setTabButton(0, QTabBar::RightSide, nullptr); // Home can't be closed
    m_tabs->tabBar()->setTabButton(0, QTabBar::LeftSide, nullptr);
    setCentralWidget(m_tabs);
    connect(m_tabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeTab);
    connect(m_tabs, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
    connect(m_tabs->tabBar(), &QTabBar::tabMoved, this, [this](int from, int to) {
        // Keep Home first.
        if (to == 0 || from == 0) {
            const QSignalBlocker block(m_tabs->tabBar());
            m_tabs->tabBar()->moveTab(m_tabs->indexOf(m_home), 0);
        }
        saveSession();
    });

    m_definition = new DefinitionPanel(ctx);
    m_dictDock = new QDockWidget("Dictionary", this);
    m_dictDock->setObjectName("dictionaryDock");
    m_dictDock->setWidget(m_definition);
    m_dictDock->setMinimumWidth(320);
    addDockWidget(Qt::RightDockWidgetArea, m_dictDock);
    m_dictDock->hide();
    connect(m_dictDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (!visible)
            for (ReaderView *r : readers()) r->clearHighlight();
    });
    connect(m_definition, &DefinitionPanel::openSourceRequested, this, [this](const QString &key, int page) { openDocument(key, page); });

    m_pageLabel = new QLabel;
    m_zoomLabel = new QLabel;
    m_syncLabel = new QLabel;
    statusBar()->addWidget(m_pageLabel);
    statusBar()->addWidget(m_zoomLabel);
    statusBar()->addPermanentWidget(m_syncLabel);

    m_contents = new QTreeWidget;
    m_contents->setHeaderHidden(true);
    m_contentsDock = new QDockWidget("Contents", this);
    m_contentsDock->setObjectName("contentsDock");
    m_contentsDock->setWidget(m_contents);
    addDockWidget(Qt::LeftDockWidgetArea, m_contentsDock);
    m_contentsDock->hide();
    connect(m_contentsDock, &QDockWidget::visibilityChanged, this, [this](bool visible) { if (visible) updateContents(); });
    connect(m_contents, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        if (ReaderView *r = currentReader(); r && item->data(0, Qt::UserRole).isValid()) {
            r->goToPage(item->data(0, Qt::UserRole).toInt(), item->data(0, Qt::UserRole + 1).toDouble());
            r->setFocus();
        }
    });

    m_tools.mouseDraws = QSettings().value("reader/mouseDraws", true).toBool();
    buildActions();
    buildToolbars();
    buildMenus();
    buildMoreMenu();

    // Search bar (/ or Ctrl+F): incremental search with match count.
    m_findBar = new QToolBar("Search", this);
    m_findBar->setObjectName("findBar");
    m_findBar->setMovable(false);
    addToolBar(Qt::BottomToolBarArea, m_findBar);
    m_findEdit = new QLineEdit;
    m_findEdit->setClearButtonEnabled(true);
    m_findEdit->setMaximumWidth(420);
    m_findStatus = new QLabel;
    m_findBar->addWidget(new QLabel("  Search: "));
    m_findBar->addWidget(m_findEdit);
    m_findBar->addAction("▲", this, [this] { if (ReaderView *r = currentReader()) r->findPrevious(); })->setToolTip("Previous match");
    m_findBar->addAction("▼", this, [this] { if (ReaderView *r = currentReader()) r->findNext(); })->setToolTip("Next match");
    m_findBar->addWidget(m_findStatus);
    m_findBar->addSeparator();
    m_findBar->addAction(icon("book"), "Define", this, [this] {
        if (!m_findEdit->text().trimmed().isEmpty()) showWord(m_findEdit->text().trimmed());
    })->setToolTip("Look the search word up in the dictionary");
    m_findBar->addAction("✕", this, &MainWindow::closeFindBar)->setToolTip("Close (Esc)");
    m_findBar->hide();
    connect(m_findEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (ReaderView *r = currentReader()) r->setSearchText(text, m_findBackward);
    });
    connect(m_findEdit, &QLineEdit::returnPressed, this, [this] {
        ReaderView *r = currentReader();
        if (!r) return;
        if (Commands::vimEnabled()) {
            // Vim: Enter accepts the search and returns to the page; n / N move between matches.
            if (!r->hasCurrentMatch()) r->findNext();
            closeFindBar();
        } else {
            r->findNext();
        }
    });

    connect(m_home, &LibraryView::openRequested, this, [this](const QString &key) { openDocument(key); });
    connect(m_home, &LibraryView::signInRequested, this, &MainWindow::signInOrOut);
    connect(m_home, &LibraryView::myWordsRequested, this, &MainWindow::showVocab);
    connect(m_home, &LibraryView::wordRequested, this, &MainWindow::showWord);

    connect(m_ctx.docs, &DocumentStore::libraryChanged, this, &MainWindow::refreshTabTitles);
    connect(m_ctx.sync, &SyncManager::statusChanged, this, &MainWindow::updateSyncStatus);
    connect(m_ctx.auth, &GoogleAuth::signedInChanged, this, &MainWindow::updateSyncStatus);
    connect(m_ctx.auth, &GoogleAuth::browserSignInStarted, this, &MainWindow::showSignInDialog);
    connect(m_ctx.auth, &GoogleAuth::signInFinished, this, [this](bool ok, const QString &error) {
        if (m_signInDialog)
            m_signInDialog->close();
        if (ok) {
            statusBar()->showMessage("Signed in. Syncing…", 4000);
            m_ctx.sync->syncNow();
        } else {
            QMessageBox::warning(this, "Google sign-in", error);
        }
        updateSyncStatus();
    });

    // Pick up changes from other devices regularly while the app is open (interval in Settings).
    connect(&m_periodicSync, &QTimer::timeout, m_ctx.sync, &SyncManager::syncNow);

    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    connect(escape, &QShortcut::activated, this, &MainWindow::onEscape);
    applySettings();

    QSettings settings;
    restoreGeometry(settings.value("window/geometry").toByteArray());
    restoreState(settings.value("window/state").toByteArray());
    m_dictDock->hide();
    m_contentsDock->hide();
    m_findBar->hide();
    updateToolActions();
    onTabChanged();
    updateSyncStatus();

    // On launch: pull from other devices, and reopen the tabs left open last time.
    QTimer::singleShot(0, this, [this] {
        m_ctx.sync->syncNow();
        restoreSession();
    });
    // Index PDFs for smart search once the window is up.
    QTimer::singleShot(4000, this, [this] { if (AiPrefs::autoIndex()) m_ctx.indexer->start(); });
}

// ---------------------------------------------------------------- actions, toolbars, menus

QAction *MainWindow::command(const QString &id, const QString &text, const QString &iconName)
{
    auto *a = new QAction(iconName.isEmpty() ? QIcon() : icon(iconName), text, this);
    a->setProperty("themeIcon", iconName); // re-tinted when the theme changes
    addAction(a); // shortcuts work even when the action isn't on a visible toolbar or menu
    m_commands.insert(id, a);
    return a;
}

void MainWindow::readerCommand(const QString &id, const QString &text, std::function<void(ReaderView *)> run)
{
    QAction *a = command(id, text);
    connect(a, &QAction::triggered, this, [this, run] { if (ReaderView *r = currentReader()) run(r); });
    m_readerOnly << a;
}

void MainWindow::buildActions()
{
    // Shortcuts are assigned in applySettings() from the configurable Commands registry.
    auto onReader = [this](QAction *a, void (ReaderView::*method)()) {
        connect(a, &QAction::triggered, this, [this, method] { if (ReaderView *r = currentReader()) (r->*method)(); });
        m_readerOnly << a;
    };

    m_openAct = command("openPdf", "Open PDF…", "open");
    connect(m_openAct, &QAction::triggered, this, [this] {
        const QStringList paths = QFileDialog::getOpenFileNames(this, "Open PDF", QSettings().value("ui/lastDir").toString(),
                                                                "PDF files (*.pdf)");
        for (const QString &path : paths) {
            QSettings().setValue("ui/lastDir", QFileInfo(path).absolutePath());
            openFile(path);
        }
    });
    m_syncAct = command("sync", "Sync now", "sync");
    connect(m_syncAct, &QAction::triggered, this, [this] {
        if (m_ctx.auth->isSignedIn()) m_ctx.sync->syncNow();
        else signInOrOut();
    });
    m_signAct = new QAction("Sign in with Google…", this);
    connect(m_signAct, &QAction::triggered, this, &MainWindow::signInOrOut);
    m_clientFileAct = new QAction("Google client file…", this);
    connect(m_clientFileAct, &QAction::triggered, this, &MainWindow::chooseClientFile);
    m_quitAct = command("quit", "Quit");
    connect(m_quitAct, &QAction::triggered, this, &QWidget::close);
    m_settingsAct = command("settings", "Settings…");
    connect(m_settingsAct, &QAction::triggered, this, &MainWindow::showSettings);
    m_helpAct = command("shortcutHelp", "Keyboard shortcuts");
    connect(m_helpAct, &QAction::triggered, this, &MainWindow::showShortcutHelp);

    m_homeAct = command("home", "Home", "home");
    connect(m_homeAct, &QAction::triggered, this, [this] { m_tabs->setCurrentWidget(m_home); });
    m_closeTabAct = command("closeTab", "Close tab");
    connect(m_closeTabAct, &QAction::triggered, this, [this] { closeTab(m_tabs->currentIndex()); });
    m_readerOnly << m_closeTabAct;
    // Alt+1…8 go to that tab (1 = Home), Alt+9 to the last one, like web browsers.
    for (int n = 1; n <= 9; ++n) {
        QAction *a = command(QStringLiteral("tab%1").arg(n), n == 9 ? QString("Last tab") : QStringLiteral("Tab %1").arg(n));
        connect(a, &QAction::triggered, this, [this, n] {
            const int index = n == 9 ? m_tabs->count() - 1 : n - 1;
            if (index < m_tabs->count()) m_tabs->setCurrentIndex(index);
        });
    }
    m_renameAct = command("renamePdf", "Rename PDF…");
    connect(m_renameAct, &QAction::triggered, this, &MainWindow::renamePdf);
    m_nextTabAct = command("nextTab", "Next tab");
    connect(m_nextTabAct, &QAction::triggered, this, [this] { m_tabs->setCurrentIndex((m_tabs->currentIndex() + 1) % m_tabs->count()); });
    m_prevTabAct = command("prevTab", "Previous tab");
    connect(m_prevTabAct, &QAction::triggered, this, [this] {
        m_tabs->setCurrentIndex((m_tabs->currentIndex() + m_tabs->count() - 1) % m_tabs->count());
    });

    m_undoAct = command("undo", "Undo", "undo");
    onReader(m_undoAct, &ReaderView::undo);
    m_redoAct = command("redo", "Redo", "redo");
    onReader(m_redoAct, &ReaderView::redo);
    m_clearPageAct = command("clearPage", "Clear page");
    onReader(m_clearPageAct, &ReaderView::clearPage);

    m_toolGroup = new QActionGroup(this);
    auto tool = [&](const QString &id, const QString &iconName, const QString &text, ReaderTool t) {
        QAction *a = command(id, text, iconName);
        a->setCheckable(true);
        m_toolGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, t] { ToolSettings s = m_tools; s.tool = t; setTools(s); });
        return a;
    };
    m_penAct = tool("penTool", "pen", "Pen", ReaderTool::Pen);
    m_highlighterAct = tool("highlighterTool", "highlighter", "Highlighter", ReaderTool::Highlighter);
    m_eraserAct = tool("eraserTool", "eraser", "Eraser (or the pen's eraser end / barrel button)", ReaderTool::Eraser);
    m_lookupToolAct = tool("lookupTool", "book", "Dictionary tool: click a word to define it", ReaderTool::Lookup);
    m_penAct->setChecked(true);

    m_mouseDrawsAct = command("mouseDraws", "Mouse draws (off: only a pen tablet draws, the mouse pans)", "mouse");
    m_mouseDrawsAct->setCheckable(true);
    m_mouseDrawsAct->setChecked(m_tools.mouseDraws);
    connect(m_mouseDrawsAct, &QAction::toggled, this, [this](bool on) {
        ToolSettings s = m_tools;
        s.mouseDraws = on;
        setTools(s);
        QSettings().setValue("reader/mouseDraws", on);
    });

    auto *colorGroup = new QActionGroup(this);
    for (quint32 c : kPenColors) {
        auto *a = new QAction(swatch(c), "Pen colour", this);
        a->setCheckable(true);
        colorGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, c] { ToolSettings s = m_tools; s.penColor = c; setTools(s); });
        m_penColorActs << a;
    }
    auto *highlightGroup = new QActionGroup(this);
    for (quint32 c : kHighlightColors) {
        auto *a = new QAction(swatch(c), "Highlighter colour", this);
        a->setCheckable(true);
        highlightGroup->addAction(a);
        connect(a, &QAction::triggered, this, [this, c] { ToolSettings s = m_tools; s.highlighterColor = c; setTools(s); });
        m_highlightColorActs << a;
    }
    auto *widthGroup = new QActionGroup(this);
    const QStringList widthNames = {"Thin", "Medium", "Thick"};
    for (int i = 0; i < kPenWidths.size(); ++i) {
        auto *a = new QAction(widthIcon(i), widthNames[i] + " pen", this);
        a->setCheckable(true);
        widthGroup->addAction(a);
        const float w = kPenWidths[i];
        connect(a, &QAction::triggered, this, [this, w] { ToolSettings s = m_tools; s.penWidth = w; setTools(s); });
        m_widthActs << a;
    }
    m_penColorActs[1]->setChecked(true);
    m_highlightColorActs[0]->setChecked(true);
    m_widthActs[1]->setChecked(true);

    m_zoomInAct = command("zoomIn", "Zoom in", "zoom-in");
    onReader(m_zoomInAct, &ReaderView::zoomIn);
    m_zoomOutAct = command("zoomOut", "Zoom out", "zoom-out");
    onReader(m_zoomOutAct, &ReaderView::zoomOut);
    m_fitAct = command("fitWidth", "Fit width", "fit");
    onReader(m_fitAct, &ReaderView::fitWidth);
    m_rotateAct = command("rotate", "Rotate 90°", "rotate");
    connect(m_rotateAct, &QAction::triggered, this, [this] {
        if (ReaderView *r = currentReader()) {
            r->rotateClockwise();
            QSettings().setValue("reader/rotation", r->rotation());
        }
    });
    m_readerOnly << m_rotateAct;
    m_fullScreenAct = command("fullScreen", "Full screen", "fullscreen");
    connect(m_fullScreenAct, &QAction::triggered, this, &MainWindow::toggleFullScreen);
    m_focusAct = command("focusMode", "Focus mode — just the PDF", "focus");
    m_focusAct->setCheckable(true);
    connect(m_focusAct, &QAction::triggered, this, &MainWindow::toggleFocusMode);
    m_menuBarAct = new QAction("Show menu bar", this);
    m_menuBarAct->setCheckable(true);
    m_menuBarAct->setChecked(QSettings().value("ui/showMenuBar", false).toBool());
    connect(m_menuBarAct, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue("ui/showMenuBar", on);
        applyChrome();
    });
    m_readerOnly << m_fullScreenAct;
    m_lookupAct = command("lookupWord", "Look up a word…", "search");
    connect(m_lookupAct, &QAction::triggered, this, &MainWindow::lookUpWordDialog);
    m_vocabAct = command("myWords", "My Words", "words");
    connect(m_vocabAct, &QAction::triggered, this, &MainWindow::showVocab);
    m_smartAct = command("smartSearch", "Smart search", "sparkle");
    connect(m_smartAct, &QAction::triggered, this, &MainWindow::showSmartSearch);
    m_commands.insert("dictionaryPanel", m_dictDock->toggleViewAction());
    addAction(m_dictDock->toggleViewAction());

    // Navigation (vim/zathura-style keys by default; see Settings → Keyboard)
    readerCommand("scrollDown", "Scroll down", [](ReaderView *r) { r->scrollLines(0, 1); });
    readerCommand("scrollUp", "Scroll up", [](ReaderView *r) { r->scrollLines(0, -1); });
    readerCommand("scrollLeft", "Scroll left", [](ReaderView *r) { r->scrollLines(-1, 0); });
    readerCommand("scrollRight", "Scroll right", [](ReaderView *r) { r->scrollLines(1, 0); });
    readerCommand("halfPageDown", "Half a screen down", [](ReaderView *r) { r->scrollScreens(0.5); });
    readerCommand("halfPageUp", "Half a screen up", [](ReaderView *r) { r->scrollScreens(-0.5); });
    readerCommand("screenDown", "One screen down", [](ReaderView *r) { r->scrollScreens(1.0); });
    readerCommand("screenUp", "One screen up", [](ReaderView *r) { r->scrollScreens(-1.0); });
    readerCommand("nextPage", "Next page", [](ReaderView *r) { r->nextPage(); });
    readerCommand("prevPage", "Previous page", [](ReaderView *r) { r->prevPage(); });
    readerCommand("firstPage", "First page", [](ReaderView *r) { r->goToFirstPage(); });
    readerCommand("lastPage", "Last page", [](ReaderView *r) { r->goToLastPage(); });
    readerCommand("goToPage", "Go to page…", [this](ReaderView *r) {
        bool ok = false;
        const int page = QInputDialog::getInt(this, "Go to page", QStringLiteral("Page (1–%1):").arg(r->document().pageCount),
                                              r->currentPage() + 1, 1, r->document().pageCount, 1, &ok);
        if (ok) r->goToPage(page - 1);
    });
    readerCommand("jumpBack", "Jump back", [](ReaderView *r) { r->jumpBack(); });
    readerCommand("jumpForward", "Jump forward", [](ReaderView *r) { r->jumpForward(); });
    readerCommand("setMark", "Set a mark…", [](ReaderView *r) { r->beginSetMark(); });
    readerCommand("jumpToMark", "Go to a mark…", [](ReaderView *r) { r->beginJumpToMark(); });
    readerCommand("contents", "Table of contents", [this](ReaderView *) {
        m_contentsDock->setVisible(!m_contentsDock->isVisible());
        if (m_contentsDock->isVisible()) m_contents->setFocus();
    });
    readerCommand("find", "Search in document…", [this](ReaderView *) { openFindBar(false); });
    readerCommand("findBackward", "Search backwards…", [this](ReaderView *) { openFindBar(true); });
    readerCommand("findNext", "Next match", [](ReaderView *r) { r->findNext(); });
    readerCommand("findPrev", "Previous match", [](ReaderView *r) { r->findPrevious(); });
    readerCommand("followLink", "Follow a link…", [](ReaderView *r) { r->showLinkHints(); });
}

void MainWindow::buildToolbars()
{
    m_homeBar = addToolBar("Home");
    m_homeBar->setObjectName("homeBar");
    m_homeBar->setMovable(false);
    m_homeBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_homeBar->addAction(m_openAct);
    m_homeBar->addAction(m_smartAct);
    m_homeBar->addAction(m_vocabAct);
    m_homeBar->addAction(m_lookupAct);
    m_homeBar->addAction(m_syncAct);
    auto *homeSpacer = new QWidget;
    homeSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_homeBar->addWidget(homeSpacer);

    m_readerBar = addToolBar("Reader");
    m_readerBar->setObjectName("readerBar");
    m_readerBar->setMovable(false);
    m_readerBar->setIconSize(QSize(22, 22));
    // Only what's needed while reading; everything else lives in the ⋯ menu.
    m_readerBar->addAction(m_homeAct);
    m_readerBar->addSeparator();
    m_readerBar->addAction(m_undoAct);
    m_readerBar->addAction(m_redoAct);
    m_readerBar->addSeparator();
    m_readerBar->addActions(m_toolGroup->actions());
    m_readerBar->addActions(m_penColorActs);
    m_readerBar->addActions(m_highlightColorActs);
    m_readerBar->addActions(m_widthActs);
    m_readerBar->addAction(m_clearPageAct);
    m_readerBar->addSeparator();
    m_readerBar->addAction(m_zoomOutAct);
    m_readerBar->addAction(m_fitAct);
    m_readerBar->addAction(m_zoomInAct);
    auto *readerSpacer = new QWidget;
    readerSpacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_readerBar->addWidget(readerSpacer);
    m_readerBar->addAction(m_focusAct);
}

void MainWindow::buildMoreMenu()
{
    // "⋯": everything that isn't on the slim toolbars, including all the menus.
    m_moreMenu = new QMenu(this);
    m_moreMenu->addAction(m_openAct);
    m_moreMenu->addAction(m_smartAct);
    m_moreMenu->addAction(m_lookupAct);
    m_moreMenu->addAction(m_vocabAct);
    m_moreMenu->addSeparator();
    m_moreMenu->addAction(m_focusAct);
    m_moreMenu->addAction(m_fullScreenAct);
    m_moreMenu->addAction(m_rotateAct);
    m_moreMenu->addAction(m_commands.value("contents"));
    m_moreMenu->addAction(m_mouseDrawsAct);
    m_moreMenu->addSeparator();
    for (QAction *menu : menuBar()->actions())
        if (menu->menu()) m_moreMenu->addMenu(menu->menu());
    m_moreMenu->addSeparator();
    m_moreMenu->addAction(m_menuBarAct);
    m_moreMenu->addAction(m_settingsAct);

    for (QToolBar *bar : {m_homeBar, m_readerBar}) {
        auto *more = new QToolButton;
        more->setIcon(icon("more"));
        more->setToolTip("More");
        more->setMenu(m_moreMenu);
        more->setPopupMode(QToolButton::InstantPopup);
        more->setStyleSheet("QToolButton::menu-indicator { image: none; }");
        bar->addWidget(more);
    }
}

void MainWindow::buildMenus()
{
    QMenu *file = menuBar()->addMenu("&File");
    file->addAction(m_openAct);
    file->addAction(m_closeTabAct);
    file->addAction(m_renameAct);
    file->addSeparator();
    file->addAction(m_syncAct);
    file->addAction(m_signAct);
    file->addAction(m_clientFileAct);
    file->addSeparator();
    file->addAction(m_settingsAct);
    file->addSeparator();
    file->addAction(m_quitAct);

    QMenu *edit = menuBar()->addMenu("&Edit");
    edit->addAction(m_undoAct);
    edit->addAction(m_redoAct);
    edit->addAction(m_clearPageAct);

    QMenu *view = menuBar()->addMenu("&View");
    view->addAction(m_homeAct);
    view->addAction(m_nextTabAct);
    view->addAction(m_prevTabAct);
    view->addSeparator();
    view->addAction(m_smartAct);
    view->addAction(m_vocabAct);
    view->addAction(m_lookupAct);
    view->addAction(m_dictDock->toggleViewAction());
    view->addSeparator();
    view->addAction(m_zoomInAct);
    view->addAction(m_zoomOutAct);
    view->addAction(m_fitAct);
    view->addAction(m_rotateAct);
    view->addAction(m_fullScreenAct);
    view->addAction(m_focusAct);
    view->addAction(m_menuBarAct);

    QMenu *go = menuBar()->addMenu("&Go");
    for (const char *id : {"nextPage", "prevPage", "firstPage", "lastPage", "goToPage", "", "halfPageDown", "halfPageUp",
                           "", "jumpBack", "jumpForward", "setMark", "jumpToMark", "", "find", "findBackward", "findNext",
                           "findPrev", "followLink", "", "contents"}) {
        if (!*id) go->addSeparator();
        else go->addAction(m_commands.value(id));
    }

    QMenu *tools = menuBar()->addMenu("&Tools");
    tools->addActions(m_toolGroup->actions());
    tools->addSeparator();
    tools->addAction(m_mouseDrawsAct);

    QMenu *help = menuBar()->addMenu("&Help");
    help->addAction(m_helpAct);
    help->addAction("About PenReader", this, [this] {
        QMessageBox::about(this, "PenReader",
            "PenReader 0.1 — PDF reader with pen notes, English & Tamil dictionary, and My Words.\n"
            "Syncs with the PenReader Android app through your Google Drive.\n\n"
            "Dictionary data: dictionaryapi.dev and Wiktionary (CC BY-SA).");
    });
}

// ---------------------------------------------------------------- tabs

ReaderView *MainWindow::currentReader() const { return qobject_cast<ReaderView *>(m_tabs->currentWidget()); }

QList<ReaderView *> MainWindow::readers() const
{
    QList<ReaderView *> out;
    for (int i = 0; i < m_tabs->count(); ++i)
        if (auto *r = qobject_cast<ReaderView *>(m_tabs->widget(i))) out << r;
    return out;
}

ReaderView *MainWindow::readerFor(const QString &docKey) const
{
    for (ReaderView *r : readers())
        if (r->document().docKey == docKey) return r;
    return nullptr;
}

void MainWindow::addReaderTab(const Document &doc, int page, bool activate, double offset)
{
    auto *reader = new ReaderView(m_ctx);
    reader->setTools(m_tools);
    reader->setRotation(QSettings().value("reader/rotation", 0).toInt());
    const int index = m_tabs->addTab(reader, icon("pdf"), shortTitle(doc.title));
    m_tabs->setTabToolTip(index, doc.title);
    // Open after the tab exists so the view has its real size when it scrolls to the saved spot.
    if (!reader->open(doc, page, offset)) {
        m_tabs->removeTab(index);
        reader->deleteLater();
        QMessageBox::warning(this, "PenReader", "Couldn't open “" + doc.title + "”. The file may be damaged.");
        return;
    }
    m_ctx.docs->markOpened(doc.docKey);

    connect(reader, &ReaderView::wordPicked, this, [this, reader](const PickedWord &w) { showDefinition(reader, w); });
    connect(reader, &ReaderView::undoStateChanged, this, [this, reader] { if (reader == currentReader()) updateReaderActions(); });
    connect(reader, &ReaderView::pageChanged, this, [this, reader](int p, int count) {
        if (reader == currentReader()) m_pageLabel->setText(QStringLiteral("Page %1 of %2").arg(p + 1).arg(count));
    });
    connect(reader, &ReaderView::zoomChanged, this, [this, reader](int pct) {
        if (reader == currentReader()) m_zoomLabel->setText(QStringLiteral("%1%").arg(pct));
    });
    connect(reader, &ReaderView::message, this, [this](const QString &text) {
        if (text.isEmpty()) statusBar()->clearMessage();
        else statusBar()->showMessage(text, 6000);
    });
    connect(reader, &ReaderView::searchStatus, this, [this, reader](int current, int total) {
        if (reader != currentReader()) return;
        if (reader->searchText().isEmpty()) m_findStatus->clear();
        else if (total == 0) m_findStatus->setText("  no matches yet");
        else m_findStatus->setText(QStringLiteral("  %1 of %2").arg(current).arg(total));
    });

    if (activate)
        m_tabs->setCurrentWidget(reader);
    saveSession();

    // Catch up with other devices: newer reading position and notes.
    if (m_ctx.auth->isSignedIn()) {
        const QString key = doc.docKey;
        QPointer<ReaderView> guard(reader);
        connect(m_ctx.sync, &SyncManager::finished, this, [this, guard, key, page](bool ok) {
            if (!ok || !guard || guard->document().docKey != key)
                return;
            const auto fresh = m_ctx.docs->get(key);
            if (fresh && page < 0 && !guard->userMoved() &&
                (fresh->lastPage != guard->document().lastPage ||
                 std::abs(fresh->lastOffset - guard->document().lastOffset) > 0.02))
                guard->jumpTo(fresh->lastPage, fresh->lastOffset);
            guard->reloadInkIfUntouched();
        }, Qt::SingleShotConnection);
        if (!m_restoring)
            m_ctx.sync->syncNow();
    }
}

void MainWindow::closeTab(int index)
{
    if (m_smart && m_tabs->widget(index) == m_smart) {
        m_tabs->removeTab(index); // kept for next time, with its last results
        return;
    }
    auto *reader = qobject_cast<ReaderView *>(m_tabs->widget(index));
    if (!reader)
        return; // Home stays
    reader->close(); // saves the reading position
    m_tabs->removeTab(index);
    reader->deleteLater();
    saveSession();
    m_ctx.sync->syncNow(); // push the notes made in that document
}

void MainWindow::onTabChanged()
{
    ReaderView *r = currentReader();
    setWindowTitle(r ? r->document().title + " — PenReader" : "PenReader");
    if (m_findBar)
        m_findBar->hide();
    if (r) {
        m_pageLabel->setText(QStringLiteral("Page %1 of %2").arg(r->currentPage() + 1).arg(r->document().pageCount));
        m_zoomLabel->setText({});
        r->setFocus(); // so keyboard commands and mark/hint letters reach it
    } else {
        m_dictDock->hide();
        m_contentsDock->hide();
    }
    if (m_contentsDock->isVisible())
        updateContents();
    updateReaderActions();
    saveSession();
}

void MainWindow::saveSession()
{
    if (m_restoring)
        return;
    QStringList keys;
    for (ReaderView *r : readers())
        keys << r->document().docKey;
    QSettings s;
    s.setValue("session/openDocs", keys);
    s.setValue("session/current", currentReader() ? currentReader()->document().docKey
                                  : (m_smart && m_tabs->currentWidget() == m_smart ? QString("smart") : QString()));
    const bool smartOpen = m_smart && m_tabs->indexOf(m_smart) >= 0;
    s.setValue("session/smartOpen", smartOpen);
    s.setValue("session/smartQuery", smartOpen ? m_smart->query() : QString());
    s.setValue("session/dictWord", m_dictDock->isVisible() ? m_definition->word() : QString());
    s.setValue("session/fullscreen", isFullScreen());
    s.setValue("session/focus", m_focus);
    m_home->setOpenDocuments(QSet<QString>(keys.begin(), keys.end()));
}

void MainWindow::restoreSession()
{
    if (!Prefs::restoreTabs()) {
        m_restoring = false;
        saveSession();
        return;
    }
    QSettings s;
    QStringList keys = s.value("session/openDocs").toStringList();
    const QString legacy = s.value("session/openDoc").toString(); // single-document sessions (pre-tabs)
    if (keys.isEmpty() && !legacy.isEmpty())
        keys << legacy;
    const QString current = s.value("session/current", legacy).toString();

    m_restoring = true;
    for (const QString &key : keys) {
        // Skip tabs already open (e.g. a PDF passed on the command line / "Open with"), and only
        // reopen PDFs on this computer; cloud-only ones wait to be opened from Home.
        if (readerFor(key))
            continue;
        if (const auto doc = m_ctx.docs->get(key); doc && !doc->deleted && doc->onDevice())
            addReaderTab(*doc, -1, false);
    }
    const bool openedFromCommandLine = currentReader() != nullptr;
    const QString dictWord = s.value("session/dictWord").toString();
    const bool fullscreen = s.value("session/fullscreen").toBool();
    const bool smartOpen = s.value("session/smartOpen").toBool();
    const QString smartQuery = s.value("session/smartQuery").toString();
    if (smartOpen) {
        showSmartSearch();
        m_smart->setQuery(smartQuery);
    }
    if (!openedFromCommandLine) {
        if (ReaderView *r = readerFor(current)) m_tabs->setCurrentWidget(r);
        else if (current == "smart" && m_smart) m_tabs->setCurrentWidget(m_smart);
        else m_tabs->setCurrentWidget(m_home);
    }
    if (currentReader() && !dictWord.isEmpty())
        showWord(dictWord); // the dictionary panel as it was
    if (currentReader() && fullscreen && !isFullScreen())
        toggleFullScreen();
    if (s.value("session/focus").toBool() && !m_focus)
        toggleFocusMode();
    m_restoring = false; // from now on, changes are saved
    saveSession();
}

// ---------------------------------------------------------------- opening documents

void MainWindow::openFile(const QString &path)
{
    statusBar()->showMessage("Opening " + QFileInfo(path).fileName() + "…");
    auto *watcher = new QFutureWatcher<OpenResult>(this);
    connect(watcher, &QFutureWatcher<OpenResult>::finished, this, [this, watcher] {
        watcher->deleteLater();
        statusBar()->clearMessage();
        const OpenResult r = watcher->result();
        if (!r.error.isEmpty())
            QMessageBox::warning(this, "Open PDF", r.error);
        else
            openDocument(r.docKey);
    });
    DocumentStore *docs = m_ctx.docs;
    const QString folder = m_home->currentFolderId(); // file new PDFs into the folder being browsed
    watcher->setFuture(QtConcurrent::run([docs, path, folder] {
        try {
            return OpenResult{docs->importFile(path, {}, folder).docKey, {}};
        } catch (const std::exception &e) {
            return OpenResult{{}, QString::fromStdString(e.what())};
        }
    }));
}

void MainWindow::openDocument(const QString &docKey, int page, bool activate, double offset)
{
    if (ReaderView *open = readerFor(docKey)) {
        if (activate) m_tabs->setCurrentWidget(open);
        if (page >= 0) open->goToPage(page, offset);
        return;
    }
    const auto doc = m_ctx.docs->get(docKey);
    if (!doc || doc->deleted) {
        QMessageBox::information(this, "PenReader", "That document is no longer in your library.");
        return;
    }
    if (doc->onDevice())
        addReaderTab(*doc, page, activate, offset);
    else if (doc->inCloud())
        downloadAndOpen(*doc, page, activate, offset);
    else
        QMessageBox::information(this, "PenReader",
                                 "This PDF was opened on another device and hasn't finished uploading yet. "
                                 "Try again after that device syncs.");
}

void MainWindow::downloadAndOpen(const Document &doc, int page, bool activate, double offset)
{
    statusBar()->showMessage("Downloading “" + doc.title + "” from your cloud library…");
    auto *watcher = new QFutureWatcher<OpenResult>(this);
    connect(watcher, &QFutureWatcher<OpenResult>::finished, this, [this, watcher, page, activate, offset] {
        watcher->deleteLater();
        statusBar()->clearMessage();
        const OpenResult r = watcher->result();
        if (r.needsSignIn) {
            if (QMessageBox::question(this, "PenReader", "Sign in with Google to download this PDF?") == QMessageBox::Yes)
                signInOrOut();
        } else if (!r.error.isEmpty()) {
            QMessageBox::warning(this, "Download failed", r.error);
        } else {
            openDocument(r.docKey, page, activate, offset);
        }
    });
    AppContext *ctx = &m_ctx;
    watcher->setFuture(QtConcurrent::run([ctx, doc] {
        const QString tmp = ctx->docs->docsDir() + "/download-" + doc.docKey + ".pdf";
        try {
            DriveApi::downloadTo(ctx->auth->accessToken(), doc.driveFileId, tmp);
            const QString key = ctx->docs->importFile(tmp, doc.driveFileId).docKey;
            QFile::remove(tmp);
            return OpenResult{key, {}};
        } catch (const NeedsSignIn &) {
            return OpenResult{{}, {}, true};
        } catch (const std::exception &e) {
            QFile::remove(tmp);
            return OpenResult{{}, QString::fromStdString(e.what())};
        }
    }));
}

// ---------------------------------------------------------------- dictionary & words

void MainWindow::showDefinition(ReaderView *reader, const PickedWord &picked)
{
    std::optional<WordSource> source;
    if (reader && reader->isOpen() && picked.page >= 0) {
        const Document &d = reader->document();
        source = WordSource{d.docKey, d.title, picked.page, picked.context};
    }
    if (!isFullScreen())
        m_dictDock->show();
    m_dictDock->raise();
    m_definition->showWord(picked.word, source);
}

void MainWindow::showWord(const QString &word)
{
    m_dictDock->show();
    m_dictDock->raise();
    m_definition->showWord(word);
}

void MainWindow::lookUpWordDialog()
{
    bool ok = false;
    const QString word = QInputDialog::getText(this, "Look up a word", "Word:", QLineEdit::Normal, {}, &ok).trimmed();
    if (ok && !word.isEmpty())
        showWord(word);
}

void MainWindow::showVocab()
{
    if (!m_vocab) {
        m_vocab = new VocabWindow(m_ctx, this);
        m_vocab->setAttribute(Qt::WA_DeleteOnClose);
        connect(m_vocab, &VocabWindow::openSourceRequested, this, [this](const QString &key, int page) {
            openDocument(key, page);
            raise();
            activateWindow();
        });
    }
    m_vocab->show();
    m_vocab->raise();
    m_vocab->activateWindow();
}

void MainWindow::setTools(const ToolSettings &tools)
{
    m_tools = tools;
    for (ReaderView *r : readers())
        r->setTools(tools); // same tools in every tab
    updateToolActions();
}

// ---------------------------------------------------------------- full screen

void MainWindow::toggleFullScreen()
{
    const bool enter = !isFullScreen();
    if (enter) {
        m_dockWasVisible = m_dictDock->isVisible();
        m_dictDock->hide();
        showFullScreen();
    } else {
        showNormal();
        if (m_dockWasVisible) m_dictDock->show();
    }
    m_fullScreenAct->setText(enter ? "Leave full screen" : "Full screen");
    applyChrome();
}

void MainWindow::toggleFocusMode()
{
    m_focus = !m_focus;
    if (m_focus) {
        m_focusDictWasVisible = m_dictDock->isVisible();
        m_focusContentsWasVisible = m_contentsDock->isVisible();
        m_dictDock->hide();
        m_contentsDock->hide();
        m_findBar->hide();
    } else {
        if (m_focusDictWasVisible && currentReader()) m_dictDock->show();
        if (m_focusContentsWasVisible && currentReader()) m_contentsDock->show();
    }
    m_focusAct->setChecked(m_focus);
    applyChrome();
    if (ReaderView *r = currentReader()) r->setFocus();
    if (!m_restoring) saveSession();
}

void MainWindow::applyChrome()
{
    const bool bare = isFullScreen() || m_focus;
    const bool reading = currentReader() && currentReader()->isOpen();
    menuBar()->setVisible(!bare && m_menuBarAct && m_menuBarAct->isChecked());
    m_tabs->tabBar()->setVisible(!bare);
    m_readerBar->setVisible(!bare && reading);
    m_homeBar->setVisible(!bare && !reading);
    statusBar()->setVisible(!bare);
    if (!m_showBars) {
        // Faint button in the corner to bring everything back (F8 / F11 / Esc do the same).
        m_showBars = new QToolButton(m_tabs);
        m_showBars->setIcon(icon("show-bars"));
        m_showBars->setIconSize(QSize(18, 18));
        m_showBars->setToolTip("Show toolbars (F8 or Esc)");
        m_showBars->setCursor(Qt::PointingHandCursor);
        m_showBars->setStyleSheet("QToolButton { background: rgba(55, 71, 79, 70); border: none; border-radius: 15px; padding: 6px; }"
                                  "QToolButton:hover { background: rgba(55, 71, 79, 230); }");
        connect(m_showBars, &QToolButton::clicked, this, [this] {
            if (isFullScreen()) toggleFullScreen();
            if (m_focus) toggleFocusMode();
        });
        m_tabs->installEventFilter(this);
    }
    m_showBars->setVisible(bare);
    if (bare) positionShowBarsButton();
}

void MainWindow::positionShowBarsButton()
{
    if (!m_showBars) return;
    m_showBars->adjustSize();
    m_showBars->move(m_tabs->width() - m_showBars->width() - 26, 8); // clear of the scroll bar
    m_showBars->raise();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_tabs && event->type() == QEvent::Resize && m_showBars && m_showBars->isVisible())
        positionShowBarsButton();
    return QMainWindow::eventFilter(watched, event);
}

// ---------------------------------------------------------------- Google sign-in & sync

void MainWindow::signInOrOut()
{
    if (m_ctx.auth->isSignedIn()) {
        if (QMessageBox::question(this, "Sign out", "Sign out of Google? Your notes stay on this computer and in "
                                                    "the cloud; they just stop syncing until you sign in again.")
            == QMessageBox::Yes)
            m_ctx.auth->signOut();
        return;
    }
    if (!m_ctx.auth->isConfigured()) {
        chooseClientFile();
        if (!m_ctx.auth->isConfigured())
            return;
    }
    m_ctx.auth->signIn();
}

void MainWindow::showSignInDialog(const QUrl &url, bool browserOpened)
{
    if (m_signInDialog)
        m_signInDialog->close();
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle("Sign in with Google");
    dlg->setMinimumWidth(520);
    m_signInDialog = dlg;

    auto *text = new QLabel(browserOpened
        ? "Your web browser should now show Google's sign-in page.<br>Choose your account and allow access, "
          "then come back here — this window closes by itself.<br><br>"
          "<b>Nothing opened?</b> Copy the link below and paste it into your browser."
        : "<b>Your browser couldn't be opened automatically.</b><br>Copy the link below, paste it into your "
          "browser and sign in. This window closes by itself when you're done.");
    text->setWordWrap(true);
    auto *link = new QLineEdit(url.toString(QUrl::FullyEncoded));
    link->setReadOnly(true);
    link->setCursorPosition(0);

    auto *buttons = new QDialogButtonBox;
    auto *copy = buttons->addButton("Copy link", QDialogButtonBox::ActionRole);
    auto *again = buttons->addButton("Open browser again", QDialogButtonBox::ActionRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    connect(copy, &QPushButton::clicked, dlg, [link, copy] {
        QApplication::clipboard()->setText(link->text());
        copy->setText("Copied ✓");
    });
    connect(again, &QPushButton::clicked, dlg, [url] { GoogleAuth::openInBrowser(url); });
    connect(buttons, &QDialogButtonBox::rejected, dlg, [this, dlg] {
        m_ctx.auth->cancelSignIn();
        dlg->close();
    });

    auto *layout = new QVBoxLayout(dlg);
    layout->addWidget(text);
    layout->addWidget(link);
    layout->addWidget(buttons);
    dlg->show();
}

void MainWindow::chooseClientFile()
{
    QMessageBox::information(this, "Google sign-in setup",
        "PenReader for desktop needs its own OAuth client in the same Google Cloud project as the Android app "
        "(so both share your synced data):\n\n"
        "1. Google Cloud Console → APIs & Services → Credentials\n"
        "2. Create credentials → OAuth client ID → Application type: Desktop app\n"
        "3. Download the JSON file, then choose it in the next dialog.");
    const QString path = QFileDialog::getOpenFileName(this, "Choose the downloaded client JSON", QDir::homePath(),
                                                      "JSON files (*.json)");
    if (path.isEmpty())
        return;
    const QString error = m_ctx.auth->installClientFile(path);
    if (error.isEmpty())
        QMessageBox::information(this, "Google sign-in setup", "Done. You can sign in now.");
    else
        QMessageBox::warning(this, "Google sign-in setup", error);
}

void MainWindow::updateSyncStatus()
{
    const bool signedIn = m_ctx.auth->isSignedIn();
    m_signAct->setText(signedIn ? "Sign out of Google" : "Sign in with Google…");
    QString text;
    QString style;
    switch (m_ctx.sync->status()) {
    case SyncManager::Status::Running: text = "Syncing…"; break;
    case SyncManager::Status::Done: text = "Synced " + m_ctx.sync->lastSync().toString("HH:mm"); break;
    case SyncManager::Status::Failed: text = "Sync failed: " + m_ctx.sync->message(); style = "color:#c62828"; break;
    case SyncManager::Status::SignedOut:
    case SyncManager::Status::Idle: text = signedIn ? "Not synced yet" : "Not signed in — notes stay on this computer"; break;
    }
    m_syncLabel->setText(text);
    m_syncLabel->setStyleSheet(style);
    m_syncLabel->setToolTip(m_ctx.sync->message());
}

// ---------------------------------------------------------------- state updates

void MainWindow::updateReaderActions()
{
    ReaderView *r = currentReader();
    const bool reading = r && r->isOpen();
    applyChrome();
    // Disabled on Home so plain keys like j / k reach the Home lists instead.
    for (QAction *a : std::as_const(m_readerOnly))
        a->setEnabled(reading);
    m_undoAct->setEnabled(reading && r->canUndo());
    m_redoAct->setEnabled(reading && r->canRedo());
    m_toolGroup->setEnabled(reading);
    m_pageLabel->setVisible(reading);
    m_zoomLabel->setVisible(reading);
}

void MainWindow::updateToolActions()
{
    const ReaderTool t = m_tools.tool;
    for (QAction *a : std::as_const(m_penColorActs)) a->setVisible(t == ReaderTool::Pen);
    for (QAction *a : std::as_const(m_widthActs)) a->setVisible(t == ReaderTool::Pen);
    for (QAction *a : std::as_const(m_highlightColorActs)) a->setVisible(t == ReaderTool::Highlighter);
    m_clearPageAct->setVisible(t == ReaderTool::Eraser);
}

// ---------------------------------------------------------------- window events

void MainWindow::closeEvent(QCloseEvent *event)
{
    for (ReaderView *r : readers())
        r->flushProgress();
    saveSession(); // tabs, active tab, Smart search, dictionary, full screen — as they are right now
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", saveState());

    // Push the latest notes before quitting (gives up after 15 s, e.g. when offline).
    if (m_ctx.auth->isSignedIn() && Prefs::syncOnClose() && !m_closingAfterSync) {
        m_closingAfterSync = true;
        event->ignore();
        statusBar()->show();
        statusBar()->showMessage("Syncing before closing…");
        connect(m_ctx.sync, &SyncManager::finished, this, &QWidget::close, Qt::SingleShotConnection);
        QTimer::singleShot(15000, this, &QWidget::close);
        m_ctx.sync->syncNow();
        return;
    }
    // Keep the session (open tabs) as-is so they reopen next time.
    m_restoring = true;
    for (ReaderView *r : readers())
        r->close();
    event->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    for (const QUrl &url : event->mimeData()->urls()) {
        if (url.isLocalFile() && url.toLocalFile().endsWith(".pdf", Qt::CaseInsensitive)) {
            event->acceptProposedAction();
            return;
        }
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    for (const QUrl &url : event->mimeData()->urls())
        if (url.isLocalFile() && url.toLocalFile().endsWith(".pdf", Qt::CaseInsensitive))
            openFile(url.toLocalFile()); // each dropped PDF gets its own tab
}

// ---------------------------------------------------------------- keyboard, search bar, contents, settings

void MainWindow::applySettings()
{
    // Appearance: window theme, icons tinted for it, and page colours in every open PDF.
    Theme::apply();
    for (QAction *a : std::as_const(m_commands))
        if (const QString name = a->property("themeIcon").toString(); !name.isEmpty()) a->setIcon(icon(name));
    for (QToolButton *b : findChildren<QToolButton *>())
        if (b->menu() == m_moreMenu && m_moreMenu) b->setIcon(icon("more"));
    m_tabs->setTabIcon(m_tabs->indexOf(m_home), icon("home"));
    for (int i = 0; i < m_widthActs.size(); ++i) m_widthActs[i]->setIcon(widthIcon(i));
    for (ReaderView *r : readers()) r->refreshColors();

    const auto keys = Commands::effectiveKeys();
    for (auto it = m_commands.begin(); it != m_commands.end(); ++it) {
        QAction *a = it.value();
        const QList<QKeySequence> k = keys.value(it.key());
        a->setShortcuts(k);
        QString name = a->text();
        name.remove('&');
        a->setToolTip(k.isEmpty() ? name : QStringLiteral("%1  (%2)").arg(name, Commands::describe(k)));
    }
    const int minutes = Prefs::syncInterval();
    if (minutes > 0) {
        m_periodicSync.setInterval(minutes * 60 * 1000);
        m_periodicSync.start();
    } else {
        m_periodicSync.stop();
    }
}

void MainWindow::openFindBar(bool backward)
{
    ReaderView *r = currentReader();
    if (!r)
        return;
    m_findBackward = backward;
    m_findBar->show();
    m_findEdit->setPlaceholderText(backward ? "Search backwards…" : "Search in this document…");
    const QSignalBlocker block(m_findEdit);
    m_findEdit->setText(r->searchText());
    m_findEdit->selectAll();
    m_findEdit->setFocus();
    if (!r->searchText().isEmpty())
        r->setSearchText(r->searchText(), backward); // switch direction for n / N
}

void MainWindow::closeFindBar()
{
    m_findBar->hide();
    if (ReaderView *r = currentReader())
        r->setFocus(); // highlights stay; n / N keep working
}

void MainWindow::onEscape()
{
    if (m_findBar->isVisible()) {
        closeFindBar();
    } else if (isFullScreen()) {
        toggleFullScreen();
    } else if (m_focus) {
        toggleFocusMode();
    } else if (ReaderView *r = currentReader()) {
        r->clearSearch();
        r->clearHighlight();
        statusBar()->clearMessage();
    }
}

void MainWindow::updateContents()
{
    m_contents->clear();
    ReaderView *r = currentReader();
    QPdfDocument *pdf = r ? r->pdfDocument() : nullptr;
    if (!pdf)
        return;
    QPdfBookmarkModel model;
    model.setDocument(pdf);
    std::function<void(const QModelIndex &, QTreeWidgetItem *)> add = [&](const QModelIndex &parent, QTreeWidgetItem *into) {
        for (int row = 0; row < model.rowCount(parent); ++row) {
            const QModelIndex idx = model.index(row, 0, parent);
            const int page = idx.data(int(QPdfBookmarkModel::Role::Page)).toInt();
            const QPointF loc = idx.data(int(QPdfBookmarkModel::Role::Location)).toPointF();
            const qreal height = pdf->pagePointSize(page).height();
            auto *item = into ? new QTreeWidgetItem(into) : new QTreeWidgetItem(m_contents);
            item->setText(0, idx.data(int(QPdfBookmarkModel::Role::Title)).toString());
            item->setToolTip(0, QStringLiteral("Page %1").arg(page + 1));
            item->setData(0, Qt::UserRole, page);
            item->setData(0, Qt::UserRole + 1, height > 0 ? qBound(0.0, loc.y() / height, 1.0) : 0.0);
            add(idx, item);
        }
    };
    add({}, nullptr);
    if (m_contents->topLevelItemCount() == 0) {
        auto *none = new QTreeWidgetItem(m_contents, {"This PDF has no table of contents."});
        none->setFlags(Qt::NoItemFlags);
    }
    m_contents->expandToDepth(0);
}

void MainWindow::showShortcutHelp()
{
    auto *dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle("Keyboard shortcuts — PenReader");
    dlg->resize(620, 680);
    auto *tree = new QTreeWidget;
    tree->setHeaderLabels({"Command", "Keys"});
    tree->setRootIsDecorated(false);
    tree->setAlternatingRowColors(true);
    const auto keys = Commands::effectiveKeys();
    QString category;
    for (const Command &c : Commands::all()) {
        if (c.category != category) {
            category = c.category;
            auto *header = new QTreeWidgetItem(tree, {category});
            QFont f = header->font(0);
            f.setBold(true);
            header->setFont(0, f);
            header->setFirstColumnSpanned(true);
        }
        new QTreeWidgetItem(tree, {"    " + c.name, Commands::describe(keys.value(c.id))});
    }
    tree->resizeColumnToContents(0);
    auto *tips = new QLabel(
        "<b>Link hints:</b> press the follow-link key, then type the letters shown on a link.<br>"
        "<b>Marks:</b> press the set-mark key and a letter to remember a spot; the go-to-mark key and that letter "
        "jump back (marks are saved per document).<br>"
        "<b>Search:</b> matches highlight as you type; Enter returns to the page, then next/previous match move between them.<br>"
        "<b>Pen tablet:</b> the eraser end or a held barrel button erases. <b>Words:</b> right-click or double-click to define.");
    tips->setWordWrap(true);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    auto *change = buttons->addButton("Change shortcuts…", QDialogButtonBox::ActionRole);
    connect(change, &QPushButton::clicked, this, [this, dlg] { dlg->close(); showSettings(); });
    connect(buttons, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    auto *layout = new QVBoxLayout(dlg);
    layout->addWidget(tree, 1);
    layout->addWidget(tips);
    layout->addWidget(buttons);
    dlg->show();
}

void MainWindow::showSettings()
{
    SettingsDialog dlg(m_ctx, this);
    connect(&dlg, &SettingsDialog::applied, this, &MainWindow::applySettings);
    dlg.exec();
}

void MainWindow::showSmartSearch()
{
    if (!m_smart) {
        m_smart = new SmartSearchView(m_ctx, this);
        connect(m_smart, &SmartSearchView::openRequested, this, [this](const QString &key, int page, double pos) {
            openDocument(key, page, true, qMax(0.0, pos - 0.03)); // a little above the passage
        });
    }
    if (m_tabs->indexOf(m_smart) < 0) {
        const int index = m_tabs->insertTab(1, m_smart, icon("sparkle"), "Smart search");
        m_tabs->setTabToolTip(index, "Ask questions across all your PDFs");
    }
    m_tabs->setCurrentWidget(m_smart);
    m_smart->focusQuery();
}

void MainWindow::renamePdf()
{
    const QString key = currentReader() ? currentReader()->document().docKey
                                        : (m_tabs->currentWidget() == m_home ? m_home->selectedDocument() : QString());
    if (key.isEmpty()) {
        statusBar()->showMessage("Open a PDF or select one on Home to rename it", 4000);
        return;
    }
    m_home->renameDocument(key);
}

void MainWindow::refreshTabTitles()
{
    // Renames made here or synced from another device show up in open tabs right away.
    for (ReaderView *r : readers()) {
        const auto doc = m_ctx.docs->get(r->document().docKey);
        if (!doc || doc->title == r->document().title) continue;
        r->setDocumentTitle(doc->title);
        const int i = m_tabs->indexOf(r);
        m_tabs->setTabText(i, shortTitle(doc->title));
        m_tabs->setTabToolTip(i, doc->title);
        if (r == currentReader()) setWindowTitle(doc->title + " — PenReader");
    }
}
