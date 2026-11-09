#pragma once

#include <QWidget>

class AppContext;
class DefinitionPanel;
class QLineEdit;
class QListWidget;
class QPushButton;

/// "My Words": saved words with search and sort, plus a standalone dictionary lookup.
class VocabWindow : public QWidget {
    Q_OBJECT
public:
    explicit VocabWindow(AppContext &ctx, QWidget *parent = nullptr);

signals:
    void openSourceRequested(const QString &docKey, int page);

private:
    void refresh();

    AppContext &m_ctx;
    QLineEdit *m_search;
    QPushButton *m_sort;
    QListWidget *m_list;
    DefinitionPanel *m_detail;
    bool m_alphabetical = false;
};
