#pragma once

#include <QSet>
#include <QTimer>
#include <QWidget>

class AppContext;
class QFrame;
class QLabel;
class QLineEdit;
class QListWidget;
class QMenu;
class QPushButton;
class QTreeWidget;
class Thumbnails;

/// The Home tab: the PDF catalogue (folder tree + the selected folder's contents, synced across
/// devices), recently learnt words, and the sign-in banner.
class LibraryView : public QWidget {
    Q_OBJECT
public:
    explicit LibraryView(AppContext &ctx, QWidget *parent = nullptr);

    /// Marks which documents are open in tabs.
    void setOpenDocuments(const QSet<QString> &docKeys);
    /// The folder being browsed (empty for All PDFs / Unfiled): new PDFs are filed here.
    QString currentFolderId() const;
    /// The single selected PDF in the list, if any.
    QString selectedDocument() const;
    /// Asks for a new name for a PDF.
    void renameDocument(const QString &docKey);

signals:
    void openRequested(const QString &docKey);
    void signInRequested();
    void myWordsRequested();
    void wordRequested(const QString &word);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refresh();
    void refreshFolders();
    void refreshDocuments();
    void refreshWords();
    void selectFolder(const QString &id);
    void goUp();
    QString newFolderDialog(const QString &parentId);
    void addMoveMenu(QMenu *menu, const QStringList &docKeys, const QString &movingFolder = {});
    void showDocumentMenu(const QPoint &pos);
    void showFolderMenu(const QString &folderId, const QPoint &globalPos);
    void deleteFolderAsk(const QString &folderId);
    QStringList selectedDocKeys() const;

    AppContext &m_ctx;
    Thumbnails *m_thumbs;
    QFrame *m_banner;
    QTreeWidget *m_folders;
    QLabel *m_crumb;
    QPushButton *m_up;
    QLineEdit *m_search;
    QListWidget *m_list;
    QLabel *m_empty;
    QListWidget *m_words;
    QLabel *m_wordsTitle;
    QLabel *m_wordsCount = nullptr;
    QLabel *m_docCount = nullptr;
    QLabel *m_stats = nullptr;
    void refreshStats();
    QSet<QString> m_open;
    QString m_folder; ///< folder id, or kAll / kUnfiled
    QTimer m_refreshTimer;
};
