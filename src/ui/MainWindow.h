#pragma once

#include "ui/ToolSettings.h"

#include <QHash>
#include <QMainWindow>
#include <QPointer>
#include <QTimer>
#include <functional>

class AppContext;
class DefinitionPanel;
class LibraryView;
class SmartSearchView;
class ReaderView;
struct Highlight;
class VocabWindow;
class QAction;
class QActionGroup;
class QDialog;
class QDockWidget;
class QLabel;
class QMenu;
class QLineEdit;
class QTreeWidget;
class QTabWidget;
class QToolBar;
struct Document;
struct PickedWord;

/// Tabbed main window: a permanent Home tab plus one tab per open PDF.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(AppContext &ctx, QWidget *parent = nullptr);

    /// Imports a PDF from disk and opens it in a tab.
    void openFile(const QString &path);
    /// Opens a library document in a tab (or switches to its tab), downloading it first if needed.
    void openDocument(const QString &docKey, int page = -1, bool activate = true, double offset = 0);
    /// Shows the Smart search tab (creating it on first use).
    void showSmartSearch();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void buildActions();
    void buildToolbars();
    void buildMenus();
    ReaderView *currentReader() const;
    void updateHighlights();
    void editHighlightNote(ReaderView *reader, const Highlight &h);
    void showHighlightPopup(ReaderView *reader, const Highlight &h, const QPoint &at);
    QDockWidget *m_highlightsDock = nullptr;
    class QListWidget *m_highlightList = nullptr;
    ReaderView *readerFor(const QString &docKey) const;
    QList<ReaderView *> readers() const;
    void addReaderTab(const Document &doc, int page, bool activate, double offset = 0);
    void closeTab(int index);
    void downloadAndOpen(const Document &doc, int page, bool activate, double offset = 0);
    void showDefinition(ReaderView *reader, const PickedWord &picked);
    void showWord(const QString &word);
    void lookUpWordDialog();
    void showVocab();
    void setTools(const ToolSettings &tools);
    void toggleFullScreen();
    void toggleFocusMode();
    /// Shows or hides the menu bar, toolbars, tab bar and status bar for the current mode.
    void applyChrome();
    void positionShowBarsButton();
    void buildMoreMenu();
    void signInOrOut();
    void showSignInDialog(const QUrl &url, bool browserOpened);
    void chooseClientFile();
    void onTabChanged();
    void updateReaderActions();
    void updateToolActions();
    void updateSyncStatus();
    void saveSession();
    void restoreSession();

    // Keyboard, search bar, contents, settings
    QAction *command(const QString &id, const QString &text, const QString &iconName = {});
    void readerCommand(const QString &id, const QString &text, std::function<void(ReaderView *)> run);
    void applySettings();
    void openFindBar(bool backward);
    void closeFindBar();
    void updateContents();
    void showShortcutHelp();
    void showSettings();
    void onEscape();
    void renamePdf();
    void refreshTabTitles();

    AppContext &m_ctx;
    QTabWidget *m_tabs;
    LibraryView *m_home;
    QDockWidget *m_dictDock;
    DefinitionPanel *m_definition;
    QPointer<VocabWindow> m_vocab;
    SmartSearchView *m_smart = nullptr;
    QPointer<QDialog> m_signInDialog;
    QToolBar *m_homeBar;
    QToolBar *m_readerBar;
    QLabel *m_pageLabel;
    QLabel *m_zoomLabel;
    QLabel *m_syncLabel;
    QTimer m_periodicSync;
    QToolBar *m_findBar;
    QLineEdit *m_findEdit;
    QLabel *m_findStatus;
    bool m_findBackward = false;
    QDockWidget *m_contentsDock;
    QTreeWidget *m_contents;
    QHash<QString, QAction *> m_commands;  ///< command id -> action (shortcuts come from Commands)
    QList<QAction *> m_readerOnly;         ///< disabled on the Home tab
    ToolSettings m_tools;
    bool m_closingAfterSync = false;
    bool m_dockWasVisible = false;
    bool m_focus = false;
    bool m_focusDictWasVisible = false;
    bool m_focusContentsWasVisible = false;
    class QToolButton *m_showBars = nullptr; ///< floating button that leaves focus mode / full screen
    QMenu *m_moreMenu = nullptr;
    QAction *m_focusAct = nullptr;
    QAction *m_menuBarAct = nullptr;
    /// True until the previous session has been restored, so start-up doesn't overwrite it.
    bool m_restoring = true;

    QAction *m_openAct, *m_syncAct, *m_signAct, *m_clientFileAct, *m_quitAct, *m_settingsAct, *m_helpAct;
    QAction *m_homeAct, *m_closeTabAct, *m_nextTabAct, *m_prevTabAct, *m_renameAct;
    QAction *m_undoAct, *m_redoAct, *m_clearPageAct;
    class TranslationDialog *m_translation = nullptr;
    QAction *m_penAct, *m_highlighterAct, *m_eraserAct, *m_lookupToolAct, *m_handAct, *m_mouseDrawsAct;
    QAction *m_zoomInAct, *m_zoomOutAct, *m_fitAct, *m_rotateAct, *m_fullScreenAct;
    QAction *m_lookupAct, *m_vocabAct, *m_smartAct;
    QActionGroup *m_toolGroup;
    QList<QAction *> m_penColorActs, m_highlightColorActs, m_widthActs;
};
