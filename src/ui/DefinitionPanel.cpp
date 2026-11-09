#include "ui/DefinitionPanel.h"

#include "core/AppContext.h"
#include "core/VocabStore.h"

#include <QAudioOutput>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QMediaPlayer>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace {
QString esc(const QString &s) { return s.toHtmlEscaped(); }
}

DefinitionPanel::DefinitionPanel(AppContext &ctx, QWidget *parent) : QWidget(parent), m_ctx(ctx)
{
    m_title = new QLabel;
    QFont big = m_title->font();
    big.setPointSizeF(big.pointSizeF() * 1.8);
    big.setBold(true);
    m_title->setFont(big);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_phonetic = new QLabel;
    m_phonetic->setStyleSheet("color: palette(mid);");
    m_play = new QPushButton("🔊");
    m_play->setToolTip("Pronounce");
    m_play->setFlat(true);
    m_play->setFixedWidth(36);
    connect(m_play, &QPushButton::clicked, this, [this] {
        if (!m_english || m_english->audioUrl.isEmpty())
            return;
        if (!m_player) {
            m_player = new QMediaPlayer(this);
            m_audio = new QAudioOutput(this);
            m_player->setAudioOutput(m_audio);
        }
        m_player->setSource(QUrl(m_english->audioUrl));
        m_player->play();
    });

    m_tamilSummary = new QLabel;
    QFont tamilFont = m_tamilSummary->font();
    tamilFont.setPointSizeF(tamilFont.pointSizeF() * 1.25);
    m_tamilSummary->setFont(tamilFont);
    m_tamilSummary->setStyleSheet("color: #00897B;");
    m_tamilSummary->setWordWrap(true);

    m_save = new QPushButton;
    connect(m_save, &QPushButton::clicked, this, &DefinitionPanel::save);
    m_retry = new QPushButton("Retry");
    connect(m_retry, &QPushButton::clicked, this, &DefinitionPanel::lookup);
    m_openSource = new QPushButton("Open in document");
    connect(m_openSource, &QPushButton::clicked, this, [this] {
        if (const auto w = m_ctx.vocab->get(m_word); w && !w->sourceDocKey.isEmpty())
            emit openSourceRequested(w->sourceDocKey, w->sourcePage);
    });

    m_englishView = new QTextBrowser;
    m_tamilView = new QTextBrowser;
    m_englishView->setOpenLinks(false);
    m_tamilView->setOpenLinks(false);
    m_tabs = new QTabWidget;
    m_tabs->addTab(m_englishView, "English");
    m_tabs->addTab(m_tamilView, "தமிழ் Tamil");
    m_tabs->setCurrentIndex(QSettings().value("ui/dictTab", 0).toInt());
    connect(m_tabs, &QTabWidget::currentChanged, this, [](int i) { QSettings().setValue("ui/dictTab", i); });

    m_context = new QLabel;
    m_context->setWordWrap(true);
    m_context->setStyleSheet("color: palette(mid); font-style: italic;");

    auto *head = new QHBoxLayout;
    head->addWidget(m_title);
    head->addWidget(m_phonetic);
    head->addWidget(m_play);
    head->addStretch();

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(m_save);
    buttons->addWidget(m_retry);
    buttons->addWidget(m_openSource);
    buttons->addStretch();

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(head);
    layout->addWidget(m_tamilSummary);
    layout->addLayout(buttons);
    layout->addWidget(m_tabs, 1);
    layout->addWidget(m_context);

    connect(m_ctx.vocab, &VocabStore::changed, this, &DefinitionPanel::updateSaveButton);
    render();
}

void DefinitionPanel::showWord(const QString &word, std::optional<WordSource> source)
{
    m_word = Dictionary::normalize(word);
    m_source = std::move(source);
    lookup();
}

void DefinitionPanel::lookup()
{
    const int request = ++m_request;
    m_english.reset();
    m_tamil.reset();
    render();
    const QString word = m_word;
    if (word.isEmpty())
        return;

    AppContext *ctx = &m_ctx;
    auto *english = new QFutureWatcher<DictResult>(this);
    connect(english, &QFutureWatcher<DictResult>::finished, this, [this, english, request] {
        english->deleteLater();
        if (request == m_request) { m_english = english->result(); render(); }
    });
    english->setFuture(QtConcurrent::run([ctx, word] { return ctx->dictionary->lookup(word); }));

    auto *tamil = new QFutureWatcher<TamilEntry>(this);
    connect(tamil, &QFutureWatcher<TamilEntry>::finished, this, [this, tamil, request] {
        tamil->deleteLater();
        if (request == m_request) { m_tamil = tamil->result(); render(); }
    });
    tamil->setFuture(QtConcurrent::run([ctx, word] { return ctx->tamil->lookup(word); }));
}

void DefinitionPanel::render()
{
    m_title->setText(m_word.isEmpty() ? "Dictionary" : esc(m_word));
    m_phonetic->setText(m_english ? esc(m_english->phonetic) : QString());
    m_play->setVisible(m_english && !m_english->audioUrl.isEmpty());
    m_tamilSummary->setText(m_tamil ? m_tamil->summary() : QString());
    m_retry->setVisible(m_english && m_english->status == DictResult::Status::Error);
    m_englishView->setHtml(englishHtml());
    m_tamilView->setHtml(tamilHtml());

    const bool hasContext = m_source && !m_source->sentence.isEmpty();
    m_context->setVisible(hasContext);
    if (hasContext)
        m_context->setText(QStringLiteral("In context (page %1): “%2”").arg(m_source->page + 1).arg(esc(m_source->sentence)));
    updateSaveButton();
}

void DefinitionPanel::updateSaveButton()
{
    const bool found = m_english && m_english->status == DictResult::Status::Found;
    const auto saved = m_ctx.vocab->get(m_word);
    const bool isSaved = saved && !saved->deleted;
    m_save->setVisible(found);
    m_save->setText(isSaved ? "★ Saved in My Words (click to remove)" : QStringLiteral("☆ Save “%1” to My Words").arg(m_word));
    m_openSource->setVisible(isSaved && !saved->sourceDocKey.isEmpty() && !m_source);
}

void DefinitionPanel::save()
{
    if (!m_english || m_english->status != DictResult::Status::Found)
        return;
    if (m_ctx.vocab->isSaved(m_word)) {
        m_ctx.vocab->remove(m_word);
        return;
    }
    VocabWord w;
    w.word = m_word;
    w.phonetic = m_english->phonetic;
    w.partOfSpeech = m_english->firstPartOfSpeech();
    w.definition = m_english->firstDefinition();
    if (m_tamil)
        w.tamil = m_tamil->summary();
    if (m_source) {
        w.contextSentence = m_source->sentence;
        w.sourceDocKey = m_source->docKey;
        w.sourceTitle = m_source->title;
        w.sourcePage = m_source->page;
    }
    m_ctx.vocab->save(w);
}

QString DefinitionPanel::englishHtml() const
{
    if (m_word.isEmpty())
        return "<p>Right-click or double-click a word in the document, or use the dictionary tool.</p>";
    if (!m_english)
        return "<p><i>Looking up…</i></p>";
    switch (m_english->status) {
    case DictResult::Status::NotFound:
        return QStringLiteral("<p>No definition found for “%1”.</p>").arg(esc(m_word));
    case DictResult::Status::Error:
        return QStringLiteral("<p style='color:#c62828'>%1</p>").arg(esc(m_english->message));
    case DictResult::Status::Found:
        break;
    }
    QString html;
    for (const Meaning &m : m_english->meanings) {
        html += QStringLiteral("<p><i style='color:#3949AB'>%1</i></p><ol>").arg(esc(m.partOfSpeech));
        QStringList synonyms = m.synonyms;
        for (int i = 0; i < m.definitions.size() && i < 4; ++i) {
            const Definition &d = m.definitions[i];
            html += "<li>" + esc(d.text);
            if (!d.example.isEmpty())
                html += QStringLiteral("<br><span style='color:gray'>“%1”</span>").arg(esc(d.example));
            html += "</li>";
            synonyms += d.synonyms;
        }
        html += "</ol>";
        synonyms.removeDuplicates();
        if (!synonyms.isEmpty())
            html += QStringLiteral("<p style='color:#00897B'>Synonyms: %1</p>").arg(esc(synonyms.mid(0, 8).join(", ")));
    }
    return html;
}

QString DefinitionPanel::tamilHtml() const
{
    if (m_word.isEmpty())
        return {};
    if (!m_tamil)
        return "<p><i>தேடுகிறது… (looking up)</i></p>";
    if (m_tamil->isEmpty()) {
        return m_tamil->offlineOnly
            ? "<p>No Tamil meaning in the offline dictionary. Connect to the internet for more.</p>"
            : QStringLiteral("<p>No Tamil meaning found for “%1”.</p>").arg(esc(m_word));
    }
    QString html;
    for (const TamilSense &s : m_tamil->senses) {
        QStringList words;
        for (const TamilWord &w : s.words)
            words << esc(w.tamil) + (w.romanization.isEmpty() ? QString() : " <span style='color:gray'>(" + esc(w.romanization) + ")</span>");
        html += QStringLiteral("<p><i style='color:#3949AB'>%1</i> <span style='color:gray'>%2</span><br>"
                               "<span style='font-size:large'>%3</span></p>")
                    .arg(esc(s.partOfSpeech), esc(s.meaning), words.join(",&nbsp; "));
    }
    if (!m_tamil->related.isEmpty()) {
        if (!m_tamil->senses.isEmpty())
            html += "<p><b style='color:#3949AB'>More Tamil words</b></p>";
        for (int i = 0; i < m_tamil->related.size() && i < 12; ++i) {
            const TamilIndexHit &h = m_tamil->related[i];
            html += QStringLiteral("<p><span style='font-size:large'>%1</span> <span style='color:gray'>%2</span><br>"
                                   "<span style='color:gray'>%3 · %4</span></p>")
                        .arg(esc(h.tamil), h.romanization.isEmpty() ? QString() : "(" + esc(h.romanization) + ")",
                             esc(h.partOfSpeech), esc(h.gloss));
        }
    }
    if (m_tamil->offlineOnly)
        html += "<p><small>Offline: showing the built-in word list only.</small></p>";
    html += "<p><small style='color:gray'>Source: Wiktionary (CC BY-SA)</small></p>";
    return html;
}
