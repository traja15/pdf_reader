#pragma once

#include "core/SearchIndex.h"

#include <QWidget>

class AppContext;
class OllamaChat;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTextBrowser;

/// "Ask your library": hybrid (meaning + keyword) search over every PDF's text, with an optional
/// answer written by a local AI model (Ollama) that cites the passages it used.
class SmartSearchView : public QWidget {
    Q_OBJECT
public:
    explicit SmartSearchView(AppContext &ctx, QWidget *parent = nullptr);
    void focusQuery();
    QString query() const;
    /// Puts text in the search box without running it (session restore).
    void setQuery(const QString &text);
    /// Runs a query programmatically (also used by the self-test).
    void search(const QString &query);
    const QVector<SearchHit> &hits() const { return m_hits; }
    QString answer() const { return m_answer; }

signals:
    void openRequested(const QString &docKey, int page, double pagePos);
    /// A search finished (results shown); the AI answer may still be streaming.
    void searched();
    void answerFinished();

private:
    void runSearch();
    void refreshScopes();
    void renderResults();
    void askAi();
    void renderAnswer(bool done);
    QSet<QString> scopeDocs() const;

    AppContext &m_ctx;
    QLineEdit *m_query;
    QComboBox *m_scope;
    QCheckBox *m_useAi;
    QPushButton *m_go;
    QLabel *m_indexStatus;
    QPushButton *m_indexButton;
    QLabel *m_answerTitle;
    QTextBrowser *m_answerView;
    QTextBrowser *m_resultsView;
    OllamaChat *m_chat;

    QString m_lastQuery;
    QVector<SearchHit> m_hits;
    QString m_answer;
    QString m_searchNote;
    int m_request = 0;
};
