#include "ui/SmartSearchView.h"

#include "core/AppContext.h"
#include "core/DocumentStore.h"
#include "net/Http.h"
#include "net/Ollama.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSplitter>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {

constexpr int kMaxHits = 20;
constexpr int kAnswerPassages = 6;

const char *kSystemPrompt =
    "You are a careful research assistant. Answer the user's question using ONLY the numbered excerpts from "
    "their own PDF library. Cite every fact with the excerpt number in square brackets, like [2] or [1][3]. "
    "If the excerpts don't contain the answer, say so plainly instead of guessing. Be concise: a short "
    "paragraph or a few bullet points.";

QStringList queryTerms(const QString &query)
{
    static const QRegularExpression wordRe("[\\p{L}\\p{N}]{3,}");
    QStringList terms;
    for (auto it = wordRe.globalMatch(query); it.hasNext();) terms << it.next().captured();
    return terms;
}

/// A ~360-character snippet around the first query word, with query words in bold.
QString snippet(const QString &text, const QStringList &terms)
{
    int at = -1;
    for (const QString &t : terms) {
        const int i = int(text.indexOf(t, 0, Qt::CaseInsensitive));
        if (i >= 0 && (at < 0 || i < at)) at = i;
    }
    int start = at < 0 ? 0 : std::max(0, at - 120);
    while (start > 0 && !text[start - 1].isSpace()) --start;
    QString s = text.mid(start, 360);
    QString html = (start > 0 ? "…" : "") + s.toHtmlEscaped() + (start + 360 < text.size() ? "…" : "");
    for (const QString &t : terms) {
        const QRegularExpression re("(" + QRegularExpression::escape(t.toHtmlEscaped()) + ")",
                                    QRegularExpression::CaseInsensitiveOption);
        html.replace(re, "<b>\\1</b>");
    }
    return html;
}

} // namespace

SmartSearchView::SmartSearchView(AppContext &ctx, QWidget *parent) : QWidget(parent), m_ctx(ctx), m_chat(new OllamaChat(this))
{
    auto *title = new QLabel("Smart search");
    QFont tf = title->font();
    tf.setPointSizeF(tf.pointSizeF() * 1.6);
    tf.setBold(true);
    title->setFont(tf);
    auto *subtitle = new QLabel("<span style='color:gray'>Ask a question or describe what you're looking for — "
                                "PenReader searches the text of every PDF by meaning and by keywords.</span>");

    m_query = new QLineEdit;
    m_query->setPlaceholderText("e.g. “what did the author say about monetary policy?”");
    m_query->setClearButtonEnabled(true);
    QFont qf = m_query->font();
    qf.setPointSizeF(qf.pointSizeF() * 1.2);
    m_query->setFont(qf);
    m_query->setMinimumHeight(36);
    connect(m_query, &QLineEdit::returnPressed, this, &SmartSearchView::runSearch);

    m_scope = new QComboBox;
    m_scope->setToolTip("Where to search");
    m_useAi = new QCheckBox("AI answer");
    m_useAi->setToolTip("Write an answer from the best passages with a local model (Ollama)");
    m_useAi->setChecked(QSettings().value("ai/answerEnabled", true).toBool());
    connect(m_useAi, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue("ai/answerEnabled", on); });
    m_go = new QPushButton(Theme::icon("sparkle"), "Search");
    m_go->setDefault(true);
    connect(m_go, &QPushButton::clicked, this, &SmartSearchView::runSearch);

    auto *queryRow = new QHBoxLayout;
    queryRow->addWidget(m_query, 1);
    queryRow->addWidget(m_scope);
    queryRow->addWidget(m_useAi);
    queryRow->addWidget(m_go);

    m_indexStatus = new QLabel(m_ctx.indexer->status().isEmpty() ? QString("Preparing the index…") : m_ctx.indexer->status());
    m_indexStatus->setStyleSheet("color: gray;");
    m_indexButton = new QPushButton("Index now");
    m_indexButton->setFlat(true);
    connect(m_indexButton, &QPushButton::clicked, this, [this] {
        if (m_ctx.indexer->isRunning()) m_ctx.indexer->stop();
        else m_ctx.indexer->start();
    });
    connect(m_ctx.indexer, &Indexer::statusChanged, this, [this](const QString &text, bool busy) {
        m_indexStatus->setText(text);
        m_indexButton->setText(busy ? "Pause" : "Index now");
    });
    auto *statusRow = new QHBoxLayout;
    statusRow->addWidget(m_indexStatus, 1);
    statusRow->addWidget(m_indexButton);

    m_answerTitle = new QLabel("<b>AI answer</b>");
    m_answerView = new QTextBrowser;
    m_answerView->setOpenLinks(false);
    m_answerView->setStyleSheet("QTextBrowser { background: palette(alternate-base); border: 1px solid palette(mid); border-radius: 6px; }");
    m_resultsView = new QTextBrowser;
    m_resultsView->setOpenLinks(false);
    auto openHit = [this](const QUrl &url) {
        if (url.scheme() != "hit") return;
        const int i = url.path().toInt();
        if (i >= 0 && i < m_hits.size()) emit openRequested(m_hits[i].docKey, m_hits[i].page, m_hits[i].pagePos);
    };
    connect(m_answerView, &QTextBrowser::anchorClicked, this, openHit);
    connect(m_resultsView, &QTextBrowser::anchorClicked, this, openHit);

    auto *answerBox = new QWidget;
    auto *answerLayout = new QVBoxLayout(answerBox);
    answerLayout->setContentsMargins(0, 0, 0, 0);
    answerLayout->addWidget(m_answerTitle);
    answerLayout->addWidget(m_answerView);
    auto *resultsBox = new QWidget;
    auto *resultsLayout = new QVBoxLayout(resultsBox);
    resultsLayout->setContentsMargins(0, 0, 0, 0);
    resultsLayout->addWidget(new QLabel("<b>Passages</b>"));
    resultsLayout->addWidget(m_resultsView);
    auto *splitter = new QSplitter(Qt::Vertical);
    splitter->addWidget(answerBox);
    splitter->addWidget(resultsBox);
    splitter->setSizes({220, 480});

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 16, 24, 16);
    layout->addWidget(title);
    layout->addWidget(subtitle);
    layout->addLayout(queryRow);
    layout->addLayout(statusRow);
    layout->addWidget(splitter, 1);

    connect(m_chat, &OllamaChat::token, this, [this](const QString &t) { m_answer += t; renderAnswer(false); });
    connect(m_chat, &OllamaChat::finished, this, [this] { renderAnswer(true); emit answerFinished(); });
    connect(m_chat, &OllamaChat::failed, this, [this](const QString &message) {
        m_answerView->setHtml("<p style='color:#c62828'>" + message.toHtmlEscaped() + "</p>"
                              "<p style='color:gray'>The passages below still work. Check Settings → Smart search.</p>");
        emit answerFinished();
    });
    connect(m_ctx.docs, &DocumentStore::libraryChanged, this, &SmartSearchView::refreshScopes);

    refreshScopes();
    m_answerTitle->hide();
    m_answerView->hide();
    m_resultsView->setHtml("<p style='color:gray'>Results appear here. Click one to open the PDF at that page.</p>"
                           "<p style='color:gray'>Tips: ask full questions (“how does photosynthesis store energy?”) "
                           "or type exact names and terms — both work.</p>");
}

void SmartSearchView::focusQuery()
{
    m_query->setFocus();
    m_query->selectAll();
}

QString SmartSearchView::query() const { return m_query->text(); }

void SmartSearchView::setQuery(const QString &text) { m_query->setText(text); }

void SmartSearchView::search(const QString &query)
{
    m_query->setText(query);
    runSearch();
}

void SmartSearchView::refreshScopes()
{
    const QString current = m_scope->currentData().toString();
    const QSignalBlocker block(m_scope);
    m_scope->clear();
    m_scope->addItem("All PDFs", QString());
    for (const Folder &f : m_ctx.docs->folders())
        m_scope->addItem("📁 " + m_ctx.docs->folderPath(f.id), f.id);
    const int i = m_scope->findData(current);
    m_scope->setCurrentIndex(i >= 0 ? i : 0);
}

QSet<QString> SmartSearchView::scopeDocs() const
{
    const QString folder = m_scope->currentData().toString();
    if (folder.isEmpty())
        return {};
    QSet<QString> docs;
    for (const Document &d : m_ctx.docs->library()) {
        int depth = 0;
        for (QString f = d.folderId; !f.isEmpty() && depth++ < 32;) { // folder or any subfolder
            if (f == folder) { docs.insert(d.docKey); break; }
            const auto parent = m_ctx.docs->folder(f);
            f = parent ? parent->parentId : QString();
        }
    }
    if (docs.isEmpty()) docs.insert("__none__"); // empty folder: match nothing
    return docs;
}

void SmartSearchView::runSearch()
{
    const QString query = m_query->text().trimmed();
    if (query.isEmpty())
        return;
    m_lastQuery = query;
    const int request = ++m_request;
    m_chat->cancel();
    m_answer.clear();
    m_resultsView->setHtml("<p style='color:gray'><i>Searching…</i></p>");
    m_answerTitle->setVisible(m_useAi->isChecked());
    m_answerView->setVisible(m_useAi->isChecked());
    m_answerView->setHtml("<p style='color:gray'><i>Waiting for passages…</i></p>");

    struct Result {
        QVector<SearchHit> hits;
        QString note;
    };
    auto *watcher = new QFutureWatcher<Result>(this);
    connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, request] {
        watcher->deleteLater();
        if (request != m_request) return;
        const Result r = watcher->result();
        m_hits = r.hits;
        m_searchNote = r.note;
        renderResults();
        emit searched();
        if (m_useAi->isChecked()) askAi();
    });
    SearchIndex *index = m_ctx.search.get();
    const QSet<QString> allowed = scopeDocs();
    watcher->setFuture(QtConcurrent::run([index, query, allowed] {
        Result r;
        QVector<float> vector;
        try {
            vector = Ollama::embed({query}, true).value(0);
        } catch (const HttpError &e) {
            r.note = e.status == 0 ? "Keyword results only — start Ollama for meaning-based search."
                                   : "Keyword results only — " + QString::fromStdString(e.what());
        }
        r.hits = index->search(query, vector, AiPrefs::embeddingModel(), allowed, kMaxHits);
        return r;
    }));
}

void SmartSearchView::renderResults()
{
    if (m_hits.isEmpty()) {
        m_resultsView->setHtml("<p>No passages match “" + m_lastQuery.toHtmlEscaped() + "”.</p>"
                               "<p style='color:gray'>New PDFs are indexed in the background; check the status line above.</p>"
                               + (m_searchNote.isEmpty() ? QString() : "<p style='color:gray'>" + m_searchNote.toHtmlEscaped() + "</p>"));
        if (m_useAi->isChecked()) m_answerView->setHtml("<p style='color:gray'>Nothing to answer from.</p>");
        return;
    }
    const QStringList terms = queryTerms(m_lastQuery);
    QString html;
    if (!m_searchNote.isEmpty())
        html += "<p style='color:#E65100'>" + m_searchNote.toHtmlEscaped() + "</p>";
    for (int i = 0; i < m_hits.size(); ++i) {
        const SearchHit &h = m_hits[i];
        const auto doc = m_ctx.docs->get(h.docKey);
        const QString title = doc ? doc->title : QString("(removed PDF)");
        QString meta = QStringLiteral("page %1").arg(h.page + 1);
        if (doc && m_ctx.docs->folderExists(doc->folderId)) meta += " · 📁 " + m_ctx.docs->folderPath(doc->folderId);
        meta += h.byMeaning && h.byKeyword ? " · meaning + keywords" : h.byMeaning ? " · by meaning" : " · keywords";
        html += QStringLiteral("<p style='margin-bottom:10px'><a href='hit:%1' style='text-decoration:none'>"
                               "<b>[%2] %3</b></a> <span style='color:gray'>%4</span><br>%5</p>")
                    .arg(i).arg(i + 1).arg(title.toHtmlEscaped(), meta.toHtmlEscaped(), snippet(h.text, terms));
    }
    m_resultsView->setHtml(html);
}

void SmartSearchView::askAi()
{
    if (m_hits.isEmpty())
        return;
    QString excerpts;
    for (int i = 0; i < m_hits.size() && i < kAnswerPassages; ++i) {
        const SearchHit &h = m_hits[i];
        const auto doc = m_ctx.docs->get(h.docKey);
        excerpts += QStringLiteral("[%1] (%2, page %3)\n%4\n\n")
                        .arg(i + 1).arg(doc ? doc->title : QString("PDF")).arg(h.page + 1).arg(h.text.left(1400));
    }
    m_answerView->setHtml(QStringLiteral("<p style='color:gray'><i>%1 is reading the top passages…</i></p>")
                              .arg(AiPrefs::answerModel().toHtmlEscaped()));
    const int n = std::min<int>(kAnswerPassages, int(m_hits.size()));
    m_chat->ask(kSystemPrompt, QStringLiteral("There %1 %2 excerpt%3, numbered 1 to %2. Cite only those numbers.\n\n")
                                   .arg(n == 1 ? "is" : "are").arg(n).arg(n == 1 ? "" : "s") +
                                   "Excerpts:\n\n" + excerpts + "Question: " + m_lastQuery);
}

void SmartSearchView::renderAnswer(bool done)
{
    // Citations like [2] become links that open the passage. Numbers that don't match a passage the
    // model was given (small models sometimes invent them) are dropped rather than shown as broken links.
    static const QRegularExpression cite("\\[(\\d{1,2})\\]");
    const int given = std::min<int>(kAnswerPassages, int(m_hits.size()));
    QString fixed;
    qsizetype pos = 0;
    for (auto it = cite.globalMatch(m_answer); it.hasNext();) {
        const auto m = it.next();
        const int n = m.captured(1).toInt();
        fixed += m_answer.mid(pos, m.capturedStart() - pos);
        if (n >= 1 && n <= given) fixed += QStringLiteral("[\\[%1\\]](hit:%2)").arg(n).arg(n - 1);
        pos = m.capturedEnd();
    }
    fixed += m_answer.mid(pos);
    if (!done) fixed += " ▍";
    m_answerView->setMarkdown(fixed);
    if (done)
        m_answerView->append(QStringLiteral("<p style='color:gray; font-size:small'>Written by %1 from your PDFs — "
                                            "check the cited pages.</p>").arg(AiPrefs::answerModel().toHtmlEscaped()));
}
