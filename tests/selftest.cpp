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
#include "core/TextSearch.h"
#include <QPdfSearchModel>
#include "net/Ollama.h"
#include "net/GoogleAuth.h"
#include "net/Translator.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPdfDocument>
#include <QPdfSelection>
#include <QSettings>
#include <QLineEdit>
#include <QScrollBar>
#include <QDockWidget>
#include <QListWidget>
#include <QMenuBar>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTest>
#include <QClipboard>
#include <QLabel>
#include <QRegularExpression>
#include <QMouseEvent>
#include <QContextMenuEvent>
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

    // 0b. Find-in-document: fast background search must find what Qt's (slow) search finds, in the same place.
    if (!pdfPath.isEmpty()) {
        check(TextSearch::normalize("Exam-\r\nple  TEXT\r\nhere") == "example text here",
              "find: line-break hyphens joined, spaces/case normalised");
        QPdfDocument doc;
        doc.load(pdfPath);
        for (const QString word : {QStringLiteral("and"), QStringLiteral("low power modes")}) {
            QPdfSearchModel qt;
            qt.setDocument(&doc);
            qt.setSearchString(word);
            int last = -1, stable = 0;
            while (stable < 8) { wait(100); stable = qt.count() == last ? stable + 1 : 0; last = qt.count(); }
            TextSearch ours(pdfPath);
            QList<TextHit> hits;
            bool done = false;
            QObject::connect(&ours, &TextSearch::pageHits, [&](quint64, int, const QList<TextHit> &h) { hits += h; });
            QObject::connect(&ours, &TextSearch::finished, [&](quint64, int) { done = true; });
            ours.search(word, 0);
            for (int i = 0; i < 100 && !done; ++i) wait(50);
            bool same = done && hits.size() == qt.count();
            for (int i = 0; same && i < hits.size(); ++i) {
                const QPdfLink l = qt.resultAtIndex(i);
                const QRectF a = hits[i].rects.first(), b = l.rectangles().first();
                same = hits[i].page == l.page() && qAbs(a.top() - b.top()) < 2 && qAbs(a.left() - b.left()) < 2;
            }
            check(same, QStringLiteral("find '%1': same %2 matches and positions as Qt's search").arg(word).arg(qt.count()));
        }
    }

    // Google client built in with -DPENREADER_GOOGLE_CLIENT: sign-in works without any file.
    if (QFile::exists(":/google/google_oauth_client.json"))
        check(GoogleAuth().isConfigured(), "embedded Google client is used when no client file is present");

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
            {
                for (int i = 0; i < 3; ++i) reader->zoomIn();
                QTest::qWait(1500);
                reader->findNext(); reader->findPrevious(); // re-centre the current match
                QTest::qWait(800);
                reader->viewport()->grab().save(outDir + "/shot-find-match.png");
                for (int i = 0; i < 3; ++i) reader->zoomOut();
            }
            QTest::keyClick(findEdit, Qt::Key_Return);
            QTest::qWait(50);
            auto *status = window.findChild<QToolBar *>("findBar")->findChildren<QLabel *>().last();
            check(findEdit->isVisible() && reader->hasFocus() && status->text().contains(" of "),
                  "Enter returns to the page and the bar stays, showing “" + QString(status->text()).remove(QRegularExpression("<[^>]*>")).simplified() + "”");
            QTest::keyClick(reader, Qt::Key_N);
            QTest::qWait(50);
            check(status->text().contains("<b>2</b>") || status->text().contains("<b>1</b> of 1"), "n moves to the next match and the count follows");
            QTest::keyClick(reader, Qt::Key_Escape);
            QTest::qWait(50);
            check(!findEdit->isVisible() && reader->hasFocus(), "Esc closes the search bar");
        }

        // Pointer (default): plain arrow at fit width; zooming in turns it into a hand that drags the page.
        {
            reader->goToPage(0, 0.2);
            QTest::keyClick(reader, Qt::Key_6);
            check(reader->viewport()->cursor().shape() == Qt::ArrowCursor && !reader->isZoomedIn(),
                  "pointer: normal arrow when the page fits");
            for (int i = 0; i < 4; ++i) reader->zoomIn();
            QTest::qWait(100);
            const bool openHand = reader->viewport()->cursor().shape() == Qt::ArrowCursor; // left is for text
            const bool hadUndo = reader->canUndo();
            auto *h = reader->horizontalScrollBar();
            auto *v = reader->verticalScrollBar();
            reader->goToPage(0, 0);
            h->setValue(0);
            QTest::qWait(50);
            const int h0 = h->value(), v0 = v->value();
            QWidget *vp = reader->viewport();
            const QPoint c = reader->viewportPointFor(0, QPointF(12, 30)); // page margin, no text
            bool menuShown = false;
            QTest::mousePress(vp, Qt::RightButton, {}, c);
            for (int i = 1; i <= 3; ++i) {
                const QPoint p = c + QPoint(-40, -30) * i;
                QMouseEvent move(QEvent::MouseMove, p, vp->mapToGlobal(p), Qt::NoButton, Qt::RightButton, Qt::NoModifier);
                QApplication::sendEvent(vp, &move);
            }
            const bool closedHand = vp->cursor().shape() == Qt::ClosedHandCursor;
            QTimer::singleShot(300, [&] { if (QWidget *m = QApplication::activePopupWidget()) { menuShown = true; m->close(); } });
            QTest::mouseRelease(vp, Qt::RightButton, {}, c + QPoint(-120, -90));
            QTest::qWait(400);
            check(closedHand && !menuShown, "right-button drag moves the page (closed hand), no menu");
            check(h->value() > h0 && v->value() > v0 && reader->canUndo() == hadUndo,
                  QStringLiteral("right-drag moved the page by %1, %2 without drawing").arg(h->value() - h0).arg(v->value() - v0));
            // Left drag from a margin selects (from the nearest text) and never moves the page.
            const int hL = h->value(), vL = v->value();
            QTest::mousePress(vp, Qt::LeftButton, {}, c);
            for (int i = 1; i <= 3; ++i) {
                const QPoint p = c + QPoint(60, 20) * i;
                QMouseEvent move(QEvent::MouseMove, p, vp->mapToGlobal(p), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(vp, &move);
            }
            QTest::mouseRelease(vp, Qt::LeftButton, {}, c + QPoint(180, 60));
            check(h->value() == hL && v->value() == vL, "left drag never moves the page (it selects)");
            reader->clearSelection();
            bool clickMenu = false;
            QTimer::singleShot(300, [&] { if (QWidget *m = QApplication::activePopupWidget()) { clickMenu = true; m->close(); } });
            QTest::mouseClick(vp, Qt::RightButton, {}, c + QPoint(3, 2)); // a slightly shaky click
            QTest::qWait(400);
            check(clickMenu, "right click without dragging opens the menu");

            // Seen on a real laptop: the app believes the LEFT button is still held (a press whose
            // release it never got, here on the status bar) when the right button goes down.
            // The right drag and the right click must still work.
            {
                QWidget *bar = window.statusBar();
                QTest::mousePress(bar, Qt::LeftButton, {}, QPoint(5, 5)); // never released
                const int vs = v->value();
                QTest::mousePress(vp, Qt::RightButton, {}, c);
                for (int i = 1; i <= 4; ++i) QTest::mouseMove(vp, c + QPoint(0, -40 * i));
                QTest::mouseRelease(vp, Qt::RightButton, {}, c + QPoint(0, -160));
                check(v->value() > vs, QStringLiteral("right drag works even with a stuck left button (moved %1)").arg(v->value() - vs));
                bool stuckMenu = false;
                QTimer::singleShot(300, [&] { if (QWidget *m = QApplication::activePopupWidget()) { stuckMenu = true; m->close(); } });
                QTest::mouseClick(vp, Qt::RightButton, {}, c);
                QTest::qWait(400);
                check(stuckMenu, "right click opens the menu even with a stuck left button");
                QTest::mouseRelease(bar, Qt::LeftButton, {}, QPoint(5, 5));
                reader->setFocus();
            }

            // The bug: the button's release never arrives (a menu grabbed the mouse). Moving the mouse
            // afterwards with no button held must not drag the page.
            {
                bool popup = false;
                QTimer::singleShot(150, [&] { if (QWidget *m = QApplication::activePopupWidget()) { popup = true; m->close(); } });
                QTest::mousePress(vp, Qt::RightButton, {}, c); // the menu opens and swallows the release
                QContextMenuEvent sysMenu(QContextMenuEvent::Mouse, c, vp->mapToGlobal(c)); // Linux's own request: ignored
                QApplication::sendEvent(vp, &sysMenu);
                QTest::qWait(250);
                const bool firstMenu = popup;
                popup = false;
                const int hb = h->value(), vb = v->value();
                for (int i = 1; i <= 4; ++i) {
                    const QPoint p = c + QPoint(-30, -25) * i;
                    QMouseEvent mv(QEvent::MouseMove, p, vp->mapToGlobal(p), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
                    QApplication::sendEvent(vp, &mv);
                }
                check(!firstMenu && !popup && h->value() == hb && v->value() == vb,
                      "no menu on right press (Linux's request ignored), nothing drags once the button is let go");
            }
            for (int i = 0; i < 4; ++i) reader->zoomOut();
            QTest::qWait(50);
            check(reader->viewport()->cursor().shape() == Qt::ArrowCursor, "pointer: back to an arrow at fit width");
            QTest::keyClick(reader, Qt::Key_1); // the pen whenever you want to write
            check(reader->viewport()->cursor().shape() == Qt::CrossCursor, "pen tool can be picked any time");

            // With the pen, the right button selects text (drag from text) or moves the page (drag from a margin).
            auto rightDrag = [&](QPoint from, QPoint delta, int holdMs = 0) {
                QTest::mousePress(vp, Qt::RightButton, {}, from);
                if (holdMs) QTest::qWait(holdMs);
                for (int i = 1; i <= 4; ++i) {
                    const QPoint p = from + delta * i / 4;
                    QMouseEvent mv(QEvent::MouseMove, p, vp->mapToGlobal(p), Qt::NoButton, Qt::RightButton, Qt::NoModifier);
                    QApplication::sendEvent(vp, &mv);
                }
                QTest::mouseRelease(vp, Qt::RightButton, {}, from + delta);
            };
            reader->goToPage(0, 0.05);
            QTest::qWait(100);
            QPdfDocument pdf;
            pdf.load(pdfPath);
            QRectF line;
            for (const QPolygonF &poly : pdf.getAllText(0).bounds())
                if (poly.boundingRect().width() > 150 && poly.boundingRect().top() > 80) { line = poly.boundingRect(); break; }
            const bool undoBefore = reader->canUndo();
            // Settings → Mouse: right button "click = menu · drag = move page · hold = select".
            const QPoint onText = reader->viewportPointFor(0, QPointF(line.left() + 2, line.center().y()));
            const int vQuick = v->value();
            rightDrag(onText, QPoint(0, -60));
            check(!reader->hasSelection() && v->value() > vQuick, "pen mode: right drag moves the page (even on text)");
            reader->goToPage(0, 0.05);
            QTest::qWait(100);
            const int vHold = v->value();
            rightDrag(reader->viewportPointFor(0, QPointF(line.left() + 2, line.center().y())), QPoint(0, -80), 450);
            check(!reader->hasSelection() && v->value() > vHold && reader->canUndo() == undoBefore,
                  "holding the right button before dragging still moves the page (never selects)");
            check(vp->cursor().shape() == Qt::CrossCursor, "cursor back to the pen's after dragging");
            bool penMenu = false;
            QTimer::singleShot(450, [&] { if (QWidget *m = QApplication::activePopupWidget()) { penMenu = true; m->close(); } });
            QTest::mousePress(vp, Qt::RightButton, {}, onText);
            QTest::qWait(300);
            QTest::mouseRelease(vp, Qt::RightButton, {}, onText);
            QTest::qWait(300);
            check(penMenu && reader->canUndo() == undoBefore, "pen mode: right click opens the menu");

            QTest::keyClick(reader, Qt::Key_Escape);
            QTest::qWait(50);
            check(reader->viewport()->cursor().shape() != Qt::CrossCursor && reader->tools().tool == ReaderTool::Hand,
                  "Esc leaves the pen and brings back the mouse pointer");
            QTest::keyClick(reader, Qt::Key_1);
            QTest::keyClick(reader, Qt::Key_1);
            check(reader->tools().tool == ReaderTool::Hand, "clicking the pen again goes back to the mouse pointer");
        }

        // Text selection with the mouse pointer: drag across a line, then highlight / translate.
        {
            QTest::keyClick(reader, Qt::Key_6); // pointer
            reader->goToPage(0, 0);
            QTest::qWait(300);
            QPdfDocument pdf;
            pdf.load(pdfPath);
            // A line of body text a little down the page.
            QRectF line;
            for (const QPolygonF &poly : pdf.getAllText(0).bounds()) {
                const QRectF r = poly.boundingRect();
                if (r.width() > 150 && r.top() > 80) { line = r; break; }
            }
            QWidget *vp = reader->viewport();
            const QPoint a = reader->viewportPointFor(0, QPointF(line.left() + 2, line.center().y()));
            const QPoint b = reader->viewportPointFor(0, QPointF(line.left() + line.width() * 0.6, line.center().y()));
            QMouseEvent hover(QEvent::MouseMove, a, vp->mapToGlobal(a), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
            QApplication::sendEvent(vp, &hover);
            const int vSel = reader->verticalScrollBar()->value();
            QTest::mousePress(vp, Qt::LeftButton, {}, a);
            for (int i = 1; i <= 5; ++i) {
                const QPoint p = a + (b - a) * i / 5;
                QMouseEvent mv(QEvent::MouseMove, p, vp->mapToGlobal(p), Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(vp, &mv);
            }
            const bool ibeam = vp->cursor().shape() == Qt::IBeamCursor;
            QTest::mouseRelease(vp, Qt::LeftButton, {}, b);
            check(reader->verticalScrollBar()->value() == vSel, "left drag on text doesn't move the page");
            const QString selected = reader->selectedText();
            check(ibeam && vp->cursor().shape() == Qt::ArrowCursor, "text cursor only while selecting, arrow afterwards");
            check(reader->hasSelection() && selected.size() > 5, "left drag on text selects it: “" + selected.simplified() + "”");
            check(reader->selectionSentence().contains(selected.simplified().left(8)),
                  "whole sentence around the selection: “" + reader->selectionSentence().left(60) + "…”");
            QTest::qWait(200);
            window.grab().save(outDir + "/shot-selection.png");
            const bool undoBefore = reader->canUndo();
            reader->highlightSelection(0x66FFEB3B);
            Q_UNUSED(undoBefore);
            check(reader->canUndo() && !reader->hasSelection(), "Highlight turns the selection into (undoable) highlighter ink");
            QTest::qWait(200);
            window.grab().save(outDir + "/shot-highlighted.png");

            // Highlights & comments.
            {
                const QVector<Highlight> hl = reader->highlights();
                check(hl.size() == 1 && hl[0].text == selected.simplified(),
                      "highlights list shows the highlighted text: “" + (hl.isEmpty() ? QString() : hl[0].text) + "”");
                const QString key = hl.isEmpty() ? QString() : hl[0].key;
                reader->setHighlightNote(0, key, "Remember: the address line\nfor the form");
                const auto saved = InkCodec::decode(InkCodec::encode(ctx.docs->loadInk(doc.docKey).value(0)));
                const bool persisted = std::any_of(saved.begin(), saved.end(), [](const Stroke &st) { return st.note.startsWith("Remember"); });
                check(reader->highlights().value(0).note.startsWith("Remember") && persisted, "comment saved on the highlight (and in the synced ink)");
                // Click on the highlight → its comment card.
                const QPoint mid = (a + b) / 2;
                bool clicked = false;
                QString clickedNote;
                auto conn = QObject::connect(reader, &ReaderView::highlightClicked, [&](const Highlight &h, const QPoint &) { clicked = true; clickedNote = h.note; });
                QTimer::singleShot(400, [&] {
                    window.grab().save(outDir + "/shot-highlight-card.png");
                    if (QWidget *pop = QApplication::activePopupWidget()) {
                        pop->grab().save(outDir + "/shot-highlight-popup.png");
                        pop->close();
                    }
                });
                QTest::mouseClick(vp, Qt::LeftButton, {}, mid);
                QTest::qWait(600);
                QObject::disconnect(conn);
                check(clicked && clickedNote.startsWith("Remember"), "clicking a highlight shows its comment");
                // The panel (F7).
                QTest::keyClick(reader, Qt::Key_F7);
                QTest::qWait(300);
                auto *dock = window.findChild<QDockWidget *>("highlightsDock");
                auto *list = dock ? dock->findChild<QListWidget *>() : nullptr;
                check(dock && dock->isVisible() && list && list->count() == 1 && list->item(0)->data(Qt::UserRole + 1).toString() == key,
                      "F7 opens the Highlights & comments panel with the highlight");
                window.grab().save(outDir + "/shot-highlights-panel.png");
                reader->removeHighlight(0, key);
                check(reader->highlights().isEmpty() && list->item(0)->flags() == Qt::NoItemFlags, "remove highlight (panel updates)");
                reader->undo();
                check(reader->highlights().size() == 1, "undo brings the highlight back");
                QTest::keyClick(reader, Qt::Key_F7);
                reader->undo(); // the comment
            }
            reader->undo();

            // Vim visual mode: v, motions, actions.
            {
                QTest::keyClick(reader, Qt::Key_6); // pointer
                reader->goToPage(0, 0);
                QTest::qWait(200);
                QTest::keyClick(reader, Qt::Key_V);
                check(reader->inVisualMode() && reader->hasSelection(), "v starts visual mode with a caret");
                QTest::keyClick(reader, Qt::Key_E);
                const QString w1 = reader->selectedText().simplified();
                QTest::keyClick(reader, Qt::Key_W);
                QTest::keyClick(reader, Qt::Key_E);
                const QString w2 = reader->selectedText().simplified();
                QTest::qWait(150);
                window.grab().save(outDir + "/shot-visual.png");
                check(!w1.isEmpty() && w2.startsWith(w1) && w2.contains(' '), "e / w / e extend by words: “" + w1 + "” → “" + w2 + "”");
                QTest::keyClick(reader, Qt::Key_Dollar, Qt::ShiftModifier);
                const QString line = reader->selectedText().simplified();
                check(line.size() >= w2.size() && line.endsWith(QStringLiteral("THOTHATHRI")), "$ extends to the end of the line: “" + line + "”");
                QTest::keyClick(reader, Qt::Key_Y);
                check(!reader->inVisualMode() && QGuiApplication::clipboard()->text() == line, "y copies and leaves visual mode");
                const int before = int(reader->highlights().size());
                QTest::keyClick(reader, Qt::Key_V);
                QTest::keyClick(reader, Qt::Key_E);
                QTest::keyClick(reader, Qt::Key_2);
                const QVector<Highlight> hl = reader->highlights();
                check(hl.size() == before + 1 && hl.last().color == 0x6676FF03u && !reader->inVisualMode(), "v e 2 highlights the word in green");
                QTest::keyClick(reader, Qt::Key_V, Qt::ShiftModifier);
                check(reader->inVisualMode() && reader->selectedText().simplified() == line, "V selects the whole line");
                QTest::keyClick(reader, Qt::Key_H, Qt::ControlModifier | Qt::ShiftModifier); // the shortcut works in visual mode
                check(!reader->inVisualMode() && reader->highlights().size() == before + 2, "Ctrl+Shift+H highlights a visual selection");
                reader->goToPage(0, 0.9);
                QTest::qWait(100);
                QString msg;
                auto conn = QObject::connect(reader, &ReaderView::message, [&](const QString &m) { if (!m.isEmpty()) msg = m; });
                QTest::keyClick(reader, Qt::Key_BracketRight);
                check(msg.startsWith("Highlight 1 of 2"), "] goes to the next highlight (wraps): " + msg.left(40));
                QTest::keyClick(reader, Qt::Key_BracketRight);
                check(msg.startsWith("Highlight 2 of 2"), "] again: the next one");
                QObject::disconnect(conn);
                QTest::keyClick(reader, Qt::Key_V);
                QTest::keyClick(reader, Qt::Key_Escape);
                check(!reader->inVisualMode() && !reader->hasSelection(), "Esc leaves visual mode");
                reader->undo();
                reader->undo();
            }

            const Translator::Result tr = offThread([&] { return Translator::translate(selected); });
            const bool tamil = std::any_of(tr.text.begin(), tr.text.end(), [](QChar c) { return c.unicode() >= 0x0B80 && c.unicode() <= 0x0BFF; });
            check(tamil, "translate to Tamil: “" + tr.text.left(60) + "”" + (tr.error.isEmpty() ? "" : " (" + tr.error + ")"));
            check(Translator::translate(selected).cached, "translation cached for offline use");
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
        if (auto *tabs = settings.findChild<QTabWidget *>()) {
            tabs->setCurrentIndex(2); // Reading (mouse settings)
            QTest::qWait(100);
            settings.grab().save(outDir + "/shot-settings-reading.png");
            tabs->setCurrentIndex(0);
        }
    }
    std::printf("%s\n", failures ? "SOME TESTS FAILED" : "ALL PASSED");
    return failures;
}
