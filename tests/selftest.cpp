// Self-test for PenReader desktop. Exercises the real code paths that don't need a person:
// Android-compatible ink JSON, English + Tamil dictionaries (network), PDF word picking, and
// screenshots of the main window. Run with an isolated data dir, e.g.:
//   XDG_DATA_HOME=$PWD/t/data XDG_CONFIG_HOME=$PWD/t/config QT_QPA_PLATFORM=offscreen \
//     ./penreader_selftest some.pdf out-dir

#include "core/AppContext.h"
#include "core/DocumentStore.h"
#include "core/InkCodec.h"
#include "net/Dictionary.h"
#include "net/TamilDictionary.h"
#include "ui/Commands.h"
#include "ui/MainWindow.h"
#include "ui/ReaderView.h"
#include "ui/SettingsDialog.h"
#include "ui/Theme.h"
#include "ui/SmartSearchView.h"
#include "core/SearchIndex.h"
#include "net/Ollama.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QSettings>
#include <QLineEdit>
#include <QScrollBar>
#include <QDockWidget>
#include <QMenuBar>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>
#include <QToolBar>
#include <QTimer>
#include <QtConcurrent>
#include <cstdio>

namespace {

int failures = 0;

void check(bool ok, const QString &what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", qUtf8Printable(what));
    if (!ok) ++failures;
}

void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

template <typename F> auto offThread(F f)
{
    auto future = QtConcurrent::run(f);
    while (!future.isFinished()) wait(50);
    return future.result();
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("PenReader");
    QApplication::setApplicationName("PenReader");
    const QString pdfPath = argc > 1 ? argv[1] : QString();
    const QString outDir = argc > 2 ? argv[2] : QDir::currentPath();
    const QString secondPdf = argc > 3 ? argv[3] : QString();

    // 1. Ink JSON exactly as the Android app writes it (kotlinx.serialization).
    {
        const QString android = R"([{"id":"a1","tool":"HIGHLIGHTER","color":1728047931,"width":0.018,)"
                                R"("points":[0.1,0.2,0.5,0.3,0.4,0.6]},{"id":"b2","tool":"PEN","color":4279903102,)"
                                R"("width":0.0028,"points":[0.5,0.5,1.0]}])";
        const QVector<Stroke> s = InkCodec::decode(android);
        check(s.size() == 2 && s[0].tool == InkTool::Highlighter && s[0].color == 0x66FFEB3Bu && s[0].pointCount() == 2,
              "decode Android highlighter stroke");
        check(s.size() == 2 && s[1].tool == InkTool::Pen && s[1].color == 0xFF1A237Eu && qFuzzyCompare(s[1].width, 0.0028f),
              "decode Android pen stroke (ARGB colour as unsigned long)");
        const QVector<Stroke> round = InkCodec::decode(InkCodec::encode(s));
        check(round.size() == 2 && round[0].points == s[0].points && round[1].color == s[1].color, "encode/decode round trip");
    }

    Theme::apply(); // as main() does
    // 0. Appearance: dark by default, page recolouring and ink visibility.
    {
        check(Theme::isDark() && QApplication::palette().color(QPalette::Window).lightness() < 80, "theme: dark by default");
        const Theme::PageColors dark = Theme::pageColors();
        check(!dark.original && Theme::pageMode() == "dark", "reading colours: dark pages by default");
        QImage page(2, 1, QImage::Format_RGB32);
        page.setPixel(0, 0, qRgb(255, 255, 255)); // paper
        page.setPixel(1, 0, qRgb(0, 0, 0));       // text
        const QImage out = Theme::recolor(page, dark);
        check(QColor(out.pixel(0, 0)) == dark.background && QColor(out.pixel(1, 0)) == dark.text,
              "recolour: white paper → background colour, black text → text colour");
        // Real PDF renders have a transparent background: empty paper must become the background too.
        QImage transparent(2, 1, QImage::Format_ARGB32_Premultiplied);
        transparent.setPixel(0, 0, qRgba(0, 0, 0, 0));   // empty (transparent) paper
        transparent.setPixel(1, 0, qRgba(0, 0, 0, 255)); // black text
        const QImage out2 = Theme::recolor(transparent, dark);
        check(QColor(out2.pixel(0, 0)) == dark.background && QColor(out2.pixel(1, 0)) == dark.text,
              "recolour: transparent paper → background (text stays visible)");
        check(Theme::inkColor(Qt::black, false).lightness() > 128, "ink: black pen shows light on a dark page");
        check(Theme::inkColor(QColor(0x66FFEB3B), true) == QColor(0x66FFEB3B), "ink: highlighter colours unchanged");
    }

    AppContext ctx;

    // 1b. Shortcut registry: vim keys, and vim winning conflicts.
    {
        Commands::setVimEnabled(true);
        auto keys = Commands::effectiveKeys();
        check(keys["scrollDown"].contains(QKeySequence("J")) && keys["scrollDown"].contains(QKeySequence(Qt::Key_Down)),
              "vim on: j and Down scroll down");
        check(keys["redo"].contains(QKeySequence("Ctrl+R")) && !keys["rotate"].contains(QKeySequence("Ctrl+R")) &&
                  keys["rotate"].contains(QKeySequence("R")),
              "vim on: Ctrl+R is redo (vim), rotate keeps r");
        check(keys["firstPage"].contains(QKeySequence("G, G")) && keys["lastPage"].contains(QKeySequence("Shift+G")), "vim on: gg / G");
        Commands::setVimEnabled(false);
        keys = Commands::effectiveKeys();
        check(!keys["scrollDown"].contains(QKeySequence("J")) && keys["rotate"].contains(QKeySequence("Ctrl+R")),
              "vim off: plain keys free, Ctrl+R rotates");
        Commands::setCustomKeys("scrollDown", {QKeySequence("Ctrl+J")});
        check(Commands::effectiveKeys()["scrollDown"] == QList<QKeySequence>{QKeySequence("Ctrl+J")}, "custom shortcut overrides defaults");
        Commands::resetAll();
        Commands::setVimEnabled(true);
    }

    // 2. Dictionaries (need network).
    {
        const DictResult r = offThread([&] { return ctx.dictionary->lookup("Absolutism,"); });
        check(r.status == DictResult::Status::Found && !r.firstDefinition().isEmpty(),
              "English: absolutism → " + r.firstDefinition().left(60) + (r.message.isEmpty() ? "" : " [" + r.message + "]"));
        const DictResult cached = offThread([&] { return ctx.dictionary->lookup("absolutism"); });
        check(cached.fromCache, "English: second lookup served from offline cache");

        const TamilEntry t = offThread([&] { return ctx.tamil->lookup("abandon"); });
        check(!t.senses.isEmpty() && t.summary().contains(QStringLiteral("கைவிடு")), "Tamil (Wiktionary): abandon → " + t.summary());
        const TamilEntry b = offThread([&] { return ctx.tamil->lookup("beautiful"); });
        check(!b.related.isEmpty(), "Tamil (offline index): beautiful → " + b.summary());
        const TamilEntry inflected = offThread([&] { return ctx.tamil->lookup("abandoned"); });
        check(!inflected.isEmpty(), "Tamil: inflected 'abandoned' falls back to 'abandon' → " + inflected.summary());
    }

    if (pdfPath.isEmpty()) {
        std::printf("(no PDF given: skipping reader tests)\n");
        return failures;
    }

    // 3. Import + reader + word picking on a real PDF.
    const Document doc = ctx.docs->importFile(pdfPath);
    check(doc.pageCount > 0 && doc.docKey.size() == 32, QStringLiteral("import: %1 pages, key %2").arg(doc.pageCount).arg(doc.docKey));

    // 3b. Catalogue folders.
    {
        const Folder books = ctx.docs->createFolder("Books");
        const Folder physics = ctx.docs->createFolder("Physics", books.id);
        check(ctx.docs->folderPath(physics.id) == "Books / Physics", "folders: nested path “Books / Physics”");
        ctx.docs->moveDocument(doc.docKey, physics.id);
        check(ctx.docs->get(doc.docKey)->folderId == physics.id, "folders: move a PDF into a folder");
        check(!ctx.docs->moveFolder(books.id, physics.id), "folders: refuse to move a folder inside its own subfolder");
        const Folder temp = ctx.docs->createFolder("Temp", physics.id);
        const Folder inner = ctx.docs->createFolder("Inner", temp.id);
        ctx.docs->deleteFolder(temp.id);
        check(!ctx.docs->folderExists(temp.id) && ctx.docs->folder(inner.id)->parentId == physics.id,
              "folders: deleting a folder moves its subfolders up a level");
        ctx.docs->createFolder("Research papers");
        ctx.docs->createFolder("Novels", books.id);
        const Document reimported = ctx.docs->importFile(pdfPath, {}, books.id);
        check(reimported.folderId == physics.id, "folders: re-opening a filed PDF keeps its folder");
    }

    MainWindow window(ctx);
    window.resize(1280, 860);
    window.show();
    window.openDocument(doc.docKey);
    wait(2500); // let pages render
    auto *tabs = qobject_cast<QTabWidget *>(window.centralWidget()); // not the dictionary panel's tabs
    ReaderView *reader = nullptr;
    for (ReaderView *r : window.findChildren<ReaderView *>())
        if (r->document().docKey == doc.docKey) reader = r;
    check(reader && reader->isOpen(), "reader opened the document");
    check(tabs && tabs->count() == 2 && tabs->tabText(0) == "Home", "tabs: Home + one document");

    if (!secondPdf.isEmpty() && tabs) {
        const Document second = ctx.docs->importFile(secondPdf);
        check(second.folderId.isEmpty(), "folders: a new PDF opened from All PDFs stays Unfiled");
        window.openDocument(second.docKey);
        wait(1500);
        check(tabs->count() == 3 && tabs->currentIndex() == 2, "tabs: second PDF opens in a new, active tab");
        window.openDocument(doc.docKey);
        check(tabs->count() == 3 && tabs->currentWidget() == reader, "tabs: reopening a PDF switches to its tab (no duplicate)");
        const QStringList session = QSettings().value("session/openDocs").toStringList();
        check(session == QStringList{doc.docKey, second.docKey}, "tabs: session remembers both open tabs");
        // Alt+number switches tabs (Alt+1 = Home, Alt+9 = last).
        QTest::keyClick(&window, Qt::Key_1, Qt::AltModifier);
        check(tabs->currentIndex() == 0, "Alt+1 goes to Home");
        QTest::keyClick(&window, Qt::Key_2, Qt::AltModifier);
        check(tabs->currentIndex() == 1, "Alt+2 goes to the second tab");
        QTest::keyClick(&window, Qt::Key_9, Qt::AltModifier);
        check(tabs->currentIndex() == tabs->count() - 1, "Alt+9 goes to the last tab");

        // Focus mode: F8 hides everything but the PDF; Esc brings it back.
        tabs->setCurrentWidget(reader);
        reader->setFocus();
        QTest::qWait(100);
        auto *readerBar = window.findChild<QToolBar *>("readerBar");
        check(!window.menuBar()->isVisible() && readerBar->isVisible(), "slim layout: menu bar hidden by default, reader toolbar shown");
        window.grab().save(outDir + "/shot-toolbar.png");
        QTest::keyClick(reader, Qt::Key_F8);
        QTest::qWait(150);
        check(!readerBar->isVisible() && !tabs->tabBar()->isVisible() && !window.statusBar()->isVisible(),
              "focus mode (F8): toolbar, tabs and status bar hidden");
        const int vbefore = reader->verticalScrollBar()->value();
        QTest::keyClick(reader, Qt::Key_J);
        check(reader->verticalScrollBar()->value() > vbefore, "focus mode: keys still work (j scrolls)");
        window.grab().save(outDir + "/shot-focus.png");
        QTest::keyClick(reader, Qt::Key_Escape);
        QTest::qWait(150);
        check(readerBar->isVisible() && tabs->tabBar()->isVisible(), "Esc leaves focus mode");

        // Rename: library, tab title, and ".pdf" stripped.
        ctx.docs->renameDocument(second.docKey, "  Second document.pdf ");
        QTest::qWait(100);
        check(ctx.docs->get(second.docKey)->title == "Second document", "rename: new title saved (\".pdf\" stripped)");
        check(tabs->tabText(2) == "Second document", "rename: open tab title updates");
        check(reader->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOn, "reader scroll bar always visible");
        window.grab().save(outDir + "/shot-tabs.png");
        tabs->setCurrentIndex(0);
        wait(2500); // thumbnails render in the background
        window.grab().save(outDir + "/shot-home.png");
        tabs->setCurrentWidget(reader);
        wait(500);
    }

    if (reader) {
        // Pick the first real word on page 1 by clicking the centre of its box.
        QPdfDocument pdf;
        pdf.load(doc.localPath);
        const QString text = pdf.getAllText(0).text();
        int a = 0;
        while (a < text.size() && !text[a].isLetter()) ++a;
        int b = a;
        while (b < text.size() && text[b].isLetter()) ++b;
        const QString expected = text.mid(a, b - a);
        const QRectF box = pdf.getSelectionAtIndex(0, a, b - a).boundingRectangle();
        const QPointF norm = box.center() / pdf.pagePointSize(0).width();
        const auto picked = reader->wordAtPage(0, norm, true);
        check(picked && picked->word == expected,
              QStringLiteral("word pick: expected “%1”, got “%2”").arg(expected, picked ? picked->word : QString("nothing")));

        // ---- Keyboard: real key presses through Qt's shortcut system ----
        window.activateWindow();
        QTest::qWait(200);
        reader->setFocus();
        auto *vbar = reader->verticalScrollBar();
        reader->goToPage(0, 0);
        QTest::qWait(100);
        const int top = vbar->value();
        QTest::keyClick(reader, Qt::Key_J);
        QTest::qWait(50);
        check(vbar->value() > top, QStringLiteral("key j scrolls down (%1 → %2)").arg(top).arg(vbar->value()));
        QTest::keyClick(reader, Qt::Key_K);
        QTest::qWait(50);
        check(vbar->value() == top, "key k scrolls back up");
        QTest::keyClick(reader, Qt::Key_G, Qt::ShiftModifier);
        QTest::qWait(50);
        check(reader->currentPage() == doc.pageCount - 1, QStringLiteral("key G goes to the last page (page %1)").arg(reader->currentPage() + 1));
        QTest::keyClick(reader, Qt::Key_G);
        QTest::keyClick(reader, Qt::Key_G);
        QTest::qWait(50);
        check(reader->currentPage() == 0 && vbar->value() <= top + 2, "keys g g go to the first page");
        QTest::keyClick(reader, Qt::Key_O, Qt::ControlModifier);
        QTest::qWait(50);
        check(reader->currentPage() == doc.pageCount - 1, "Ctrl+O jumps back to where G went");

        // Marks: m a, move away, ' a returns.
        reader->goToPage(1, 0.3);
        QTest::keyClick(reader, Qt::Key_M);
        QTest::keyClick(reader, Qt::Key_A);
        reader->goToPage(0, 0);
        QTest::keyClick(reader, Qt::Key_Apostrophe);
        QTest::keyClick(reader, Qt::Key_A);
        QTest::qWait(50);
        check(reader->currentPage() == 1, "marks: m a … ' a returns to the marked page");

        // Search: / opens the bar, typing searches, matches are found.
        QTest::keyClick(reader, Qt::Key_Slash);
        QTest::qWait(50);
        auto *findEdit = window.findChild<QToolBar *>("findBar")->findChild<QLineEdit *>();
        check(findEdit && findEdit->isVisible() && findEdit->hasFocus(), "key / opens the search bar");
        if (findEdit) {
            QTest::keyClicks(findEdit, expected.left(5));
            QTest::qWait(2000);
            check(reader->hasCurrentMatch(), "search finds and jumps to a match for “" + expected.left(5) + "”");
            QTest::keyClick(findEdit, Qt::Key_Return);
            QTest::qWait(50);
            check(!findEdit->isVisible() && reader->hasFocus(), "Enter closes the bar and returns to the page (vim)");
        }

        // Link hints: f shows hints (or says there are none), Escape cancels.
        QString lastMessage;
        QObject::connect(reader, &ReaderView::message, [&](const QString &m) { if (!m.isEmpty()) lastMessage = m; });
        reader->goToPage(0, 0);
        QTest::keyClick(reader, Qt::Key_F);
        QTest::qWait(50);
        check(!lastMessage.isEmpty(), "key f: " + lastMessage);
        window.grab().save(outDir + "/shot-hints.png");
        QTest::keyClick(reader, Qt::Key_Escape);

        window.grab().save(outDir + "/shot-reader.png");
        if (picked)
            emit reader->wordPicked(*picked);
        wait(3000); // dictionary lookups
        window.grab().save(outDir + "/shot-dictionary.png");
    }
    // ---- Smart search (needs Ollama with the embedding + answer models for the AI parts) ----
    {
        const QVector<Chunk> chunks = SearchIndex::chunkPages({QString("word ").repeated(400), "too short", ""});
        check(chunks.size() == 3 && chunks[0].page == 0 && chunks[1].pagePos > 0, "chunking: 400 words → 3 overlapping passages");

        ctx.indexer->start();
        QElapsedTimer t;
        t.start();
        while ((ctx.indexer->isRunning() || ctx.indexer->status().isEmpty()) && t.elapsed() < 300000) QTest::qWait(200);
        std::printf("      index: %s (%lld ms)\n", qUtf8Printable(ctx.indexer->status()), t.elapsed());
        check(ctx.search->countChunks() > 0, QStringLiteral("index has %1 passages").arg(ctx.search->countChunks()));
        const int embedded = ctx.search->countEmbedded(AiPrefs::embeddingModel());
        check(embedded == ctx.search->countChunks(), QStringLiteral("all passages embedded (%1)").arg(embedded));

        // Keyword: an exact word from the PDF.
        const QString keyword = "Functionalities";
        const auto keywordHits = ctx.search->search(keyword, {}, AiPrefs::embeddingModel(), {}, 5);
        check(!keywordHits.isEmpty() && keywordHits[0].docKey == doc.docKey && keywordHits[0].byKeyword,
              "keyword search finds “" + keyword + "”");

        // Meaning: a paraphrase that shares no exact words with the target passage.
        const QString question = "Which microcontrollers and wireless chips has this person programmed?";
        QVector<float> qv;
        try { qv = Ollama::embed({question}, true).value(0); } catch (...) {}
        const auto meaningHits = ctx.search->search(question, qv, AiPrefs::embeddingModel(), {}, 5);
        check(!qv.isEmpty() && !meaningHits.isEmpty() && meaningHits[0].byMeaning,
              "meaning search answers a paraphrased question (top: page " +
                  QString::number(meaningHits.value(0).page + 1) + ", " + meaningHits.value(0).text.left(50) + "…)");

        // The Smart search tab with an AI answer.
        window.showSmartSearch();
        auto *smart = window.findChild<SmartSearchView *>();
        bool answered = false;
        QObject::connect(smart, &SmartSearchView::answerFinished, [&] { answered = true; });
        smart->search(question);
        t.restart();
        while (!answered && t.elapsed() < 240000) QTest::qWait(250);
        std::printf("      AI answer (%lld ms): %s\n", t.elapsed(), qUtf8Printable(smart->answer().left(300).replace('\n', ' ')));
        check(smart->hits().size() >= 3, QStringLiteral("Smart search tab shows passages (%1)").arg(smart->hits().size()));
        check(answered && smart->answer().contains('['), "AI answer arrives with citations like [1]");
        window.grab().save(outDir + "/shot-smart.png");
    }

    // ---- Session: quit and reopen, and everything should be where it was ----
    if (reader && !secondPdf.isEmpty()) {
        auto *tabs = qobject_cast<QTabWidget *>(window.centralWidget());
        tabs->setCurrentWidget(reader);
        reader->goToPage(1, 0.4);
        reader->zoomIn();
        const qreal zoom = reader->zoom();
        emit reader->wordPicked({"ephemeral", 1, {}}); // dictionary panel open on a word
        QTest::qWait(300);
        window.close(); // like quitting the app

        MainWindow reopened(ctx);
        reopened.resize(1280, 860);
        reopened.show();
        QTest::qWait(2500); // session restore runs right after start-up
        auto *tabs2 = qobject_cast<QTabWidget *>(reopened.centralWidget());
        QStringList titles;
        for (int i = 0; i < tabs2->count(); ++i) titles << tabs2->tabText(i);
        std::printf("      reopened tabs: %s\n", qUtf8Printable(titles.join(" | ")));
        check(titles.size() == 4 && titles[0] == "Home" && titles[1] == "Smart search" && titles.contains("mozilla") &&
                  titles.contains("Second document"),
              "session: Home, Smart search and both PDF tabs come back");
        auto *r2 = qobject_cast<ReaderView *>(tabs2->currentWidget());
        check(r2 && r2->document().docKey == doc.docKey, "session: the PDF that was showing is the active tab again");
        check(r2 && r2->currentPage() == 1, QStringLiteral("session: same page (page %1)").arg(r2 ? r2->currentPage() + 1 : 0));
        check(r2 && qFuzzyCompare(r2->zoom(), zoom), QStringLiteral("session: same zoom (%1%)").arg(qRound((r2 ? r2->zoom() : 0) * 100)));
        auto *smart2 = reopened.findChild<SmartSearchView *>();
        check(smart2 && smart2->query().startsWith("Which microcontrollers"), "session: Smart search keeps its question");
        auto *dict = reopened.findChild<QDockWidget *>("dictionaryDock");
        check(dict && dict->isVisible(), "session: dictionary panel reopens on its word");
        reopened.grab().save(outDir + "/shot-reopened.png");
    }

    {
        SettingsDialog settings(ctx, &window);
        settings.show();
        QTest::qWait(200);
        settings.grab().save(outDir + "/shot-settings.png");
    }
    std::printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL PASSED");
    return failures;
}
