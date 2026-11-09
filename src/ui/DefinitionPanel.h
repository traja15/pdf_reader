#pragma once

#include "net/Dictionary.h"
#include "net/TamilDictionary.h"

#include <QWidget>
#include <optional>

class AppContext;
class QLabel;
class QPushButton;
class QTabWidget;
class QTextBrowser;
class QMediaPlayer;
class QAudioOutput;

/// Where a looked-up word came from (saved with it in My Words).
struct WordSource {
    QString docKey;
    QString title;
    int page = -1;
    QString sentence;
};

/// Dictionary entry with English and தமிழ் tabs, pronunciation, and "Save to My Words".
class DefinitionPanel : public QWidget {
    Q_OBJECT
public:
    explicit DefinitionPanel(AppContext &ctx, QWidget *parent = nullptr);

    /// Looks the word up (in the background) and shows it.
    void showWord(const QString &word, std::optional<WordSource> source = std::nullopt);
    QString word() const { return m_word; }

signals:
    /// "Open in document" was clicked (only shown for saved words with a source).
    void openSourceRequested(const QString &docKey, int page);

private:
    void lookup();
    void render();
    void updateSaveButton();
    void save();
    QString englishHtml() const;
    QString tamilHtml() const;

    AppContext &m_ctx;
    QString m_word;
    std::optional<WordSource> m_source;
    std::optional<DictResult> m_english;
    std::optional<TamilEntry> m_tamil;
    int m_request = 0;

    QLabel *m_title;
    QLabel *m_phonetic;
    QPushButton *m_play;
    QLabel *m_tamilSummary;
    QPushButton *m_save;
    QPushButton *m_retry;
    QPushButton *m_openSource;
    QTabWidget *m_tabs;
    QTextBrowser *m_englishView;
    QTextBrowser *m_tamilView;
    QLabel *m_context;
    QMediaPlayer *m_player = nullptr;
    QAudioOutput *m_audio = nullptr;
};
