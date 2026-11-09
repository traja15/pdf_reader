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

#include <QApplication>
#include <QDir>
#include <QEventLoop>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QSettings>
#include <QLineEdit>
#include <QScrollBar>
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
        window.grab().save(outDir + "/shot-tabs.png");
        tabs->setCurrentIndex(0);
        wait(300);
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
    {
        SettingsDialog settings(&window);
        settings.show();
        QTest::qWait(200);
        settings.grab().save(outDir + "/shot-settings.png");
    }
    std::printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL PASSED");
    return failures;
}
