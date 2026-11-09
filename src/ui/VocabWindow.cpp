#include "ui/VocabWindow.h"

#include "core/AppContext.h"
#include "core/VocabStore.h"
#include "net/Dictionary.h"
#include "ui/DefinitionPanel.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm>

namespace {
constexpr int kLookupRole = Qt::UserRole + 1; ///< Marks the "Look up …" row.
}

VocabWindow::VocabWindow(AppContext &ctx, QWidget *parent) : QWidget(parent, Qt::Window), m_ctx(ctx)
{
    setWindowTitle("My Words — PenReader");
    resize(1000, 680);

    m_search = new QLineEdit;
    m_search->setPlaceholderText("Search your words (English or Tamil) or look up a new one");
    m_search->setClearButtonEnabled(true);
    connect(m_search, &QLineEdit::textChanged, this, &VocabWindow::refresh);
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        if (m_list->count() > 0) {
            m_list->setCurrentRow(0);
            emit m_list->itemActivated(m_list->item(0));
        }
    });

    m_sort = new QPushButton("Sort A–Z");
    connect(m_sort, &QPushButton::clicked, this, [this] {
        m_alphabetical = !m_alphabetical;
        m_sort->setText(m_alphabetical ? "Sort by newest" : "Sort A–Z");
        refresh();
    });

    m_list = new QListWidget;
    m_list->setWordWrap(true);
    m_list->setAlternatingRowColors(true);
    m_list->setSpacing(2);

    m_detail = new DefinitionPanel(ctx);
    connect(m_detail, &DefinitionPanel::openSourceRequested, this, &VocabWindow::openSourceRequested);

    auto show = [this](QListWidgetItem *item) {
        if (item) m_detail->showWord(item->data(Qt::UserRole).toString());
    };
    connect(m_list, &QListWidget::currentItemChanged, this, show);
    connect(m_list, &QListWidget::itemActivated, this, show);

    auto *left = new QWidget;
    auto *top = new QHBoxLayout;
    top->addWidget(m_search, 1);
    top->addWidget(m_sort);
    auto *leftLayout = new QVBoxLayout(left);
    leftLayout->addLayout(top);
    leftLayout->addWidget(m_list, 1);

    auto *splitter = new QSplitter;
    splitter->addWidget(left);
    splitter->addWidget(m_detail);
    splitter->setStretchFactor(0, 2);
    splitter->setStretchFactor(1, 3);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(splitter);

    connect(m_ctx.vocab, &VocabStore::changed, this, &VocabWindow::refresh);
    refresh();
}

void VocabWindow::refresh()
{
    const QString current = m_list->currentItem() ? m_list->currentItem()->data(Qt::UserRole).toString() : QString();
    const QString q = m_search->text().trimmed().toLower();
    QVector<VocabWord> words = m_ctx.vocab->words();
    if (m_alphabetical)
        std::sort(words.begin(), words.end(), [](const VocabWord &a, const VocabWord &b) { return a.word < b.word; });

    const QSignalBlocker block(m_list);
    m_list->clear();
    const QString normalized = Dictionary::normalize(q);
    const bool known = std::any_of(words.begin(), words.end(), [&](const VocabWord &w) { return w.word == normalized; });
    if (!normalized.isEmpty() && !known) {
        auto *item = new QListWidgetItem(QIcon(":/resources/icons/book.svg"),
                                         QStringLiteral("Look up “%1” in the dictionary").arg(normalized));
        item->setData(Qt::UserRole, normalized);
        item->setData(kLookupRole, true);
        m_list->addItem(item);
    }
    for (const VocabWord &w : words) {
        if (!q.isEmpty() && !w.word.contains(q) && !w.definition.toLower().contains(q) && !w.tamil.contains(q))
            continue;
        QString text = w.word;
        if (!w.phonetic.isEmpty()) text += "   " + w.phonetic;
        if (!w.tamil.isEmpty()) text += "\n" + w.tamil;
        text += "\n" + (w.partOfSpeech.isEmpty() ? QString() : "(" + w.partOfSpeech + ") ") + w.definition;
        QString meta = "Added " + QDateTime::fromMSecsSinceEpoch(w.addedAt).date().toString(Qt::TextDate);
        if (!w.sourceTitle.isEmpty() && w.sourcePage >= 0)
            meta += QStringLiteral(" · %1, p. %2").arg(w.sourceTitle).arg(w.sourcePage + 1);
        text += "\n" + meta;
        auto *item = new QListWidgetItem(text);
        item->setData(Qt::UserRole, w.word);
        item->setToolTip(w.contextSentence.isEmpty() ? QString() : "“" + w.contextSentence + "”");
        m_list->addItem(item);
        if (w.word == current)
            m_list->setCurrentItem(item);
    }
    setWindowTitle(QStringLiteral("My Words (%1) — PenReader").arg(words.size()));
}
