// Performance probe: rendering, recolouring and in-document search on a real PDF.
#include "ui/Theme.h"
#include "core/TextSearch.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QPdfDocument>
#include <QPdfSearchModel>
#include <QPdfPageRenderer>
#include <QTest>
#include <QPainter>
#include <QPixmap>
#include <cstdio>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QElapsedTimer t;
    QPdfDocument doc;
    t.start();
    doc.load(argv[1]);
    std::printf("load: %lld ms, %d pages\n", t.elapsed(), doc.pageCount());

    t.restart();
    QImage img;
    for (int p = 10; p < 20; ++p) img = doc.render(p, QSize(1400, 1800));
    std::printf("render 1400px: %.1f ms/page\n", t.elapsed() / 10.0);

    const auto colors = Theme::colorsFor("dark");
    t.restart();
    for (int i = 0; i < 10; ++i) Theme::recolor(img, colors);
    std::printf("recolor 1400px: %.1f ms/page\n", t.elapsed() / 10.0);

    {
        // One scroll frame: draw a page bitmap into a 1400x900 viewport, scaled vs 1:1.
        const QImage page = doc.render(12, QSize(1536, 1975)).convertToFormat(QImage::Format_ARGB32_Premultiplied);
        QImage viewport(1400, 900, QImage::Format_ARGB32_Premultiplied);
        QPainter p(&viewport);
        t.restart();
        for (int i = 0; i < 50; ++i) {
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            p.drawImage(QRectF(0, -i * 7, 1400, 1800), page);
        }
        std::printf("paint frame, scaled+smooth: %.2f ms\n", t.elapsed() / 50.0);
        const QImage exact = page.scaled(1400, 1800, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        t.restart();
        for (int i = 0; i < 50; ++i) p.drawImage(QPoint(0, -i * 7), exact);
        std::printf("paint frame, 1:1 blit:      %.2f ms\n", t.elapsed() / 50.0);
        const QPixmap pm = QPixmap::fromImage(exact);
        t.restart();
        for (int i = 0; i < 50; ++i) p.drawPixmap(QPoint(0, -i * 7), pm);
        std::printf("paint frame, 1:1 pixmap:    %.2f ms\n", t.elapsed() / 50.0);
    }
    {
        TextSearch ts(argv[1]);
        for (const char *word : {"theorem", "prime number", "zzqx", "theorem"}) {
            qint64 first = -1, finished = -1;
            int total = 0;
            QObject ctx;
            QObject::connect(&ts, &TextSearch::pageHits, &ctx, [&](quint64, int, const QList<TextHit> &h) { if (first < 0) first = t.elapsed(); total += h.size(); });
            qint64 counted = -1;
            QObject::connect(&ts, &TextSearch::counted, &ctx, [&](quint64, int) { counted = t.elapsed(); });
            QObject::connect(&ts, &TextSearch::finished, &ctx, [&](quint64, int) { finished = t.elapsed(); });
            t.restart();
            ts.search(word, 240);
            while (finished < 0 && t.elapsed() < 60000) QTest::qWait(5);
            std::printf("TextSearch '%s' from p.240: first hit %lld ms, total known %lld ms, all %d boxes %lld ms\n", word, first, counted, total, finished);
        }
    }
    if (argc > 2) return 0;
    for (const char *word : {"theorem", "zzqx"}) {
        QPdfSearchModel m;
        m.setDocument(&doc);
        t.restart();
        m.setSearchString(word);
        qint64 first = -1, settled = 0;
        int last = -1;
        while (t.elapsed() < 60000) {
            QTest::qWait(50);
            const int c = m.count();
            if (c > 0 && first < 0) first = t.elapsed();
            if (c != last) settled = t.elapsed();
            last = c;
        }
        std::printf("QPdfSearchModel '%s': first hit %lld ms, %d hits, last change at %lld ms\n", word, first, last, settled);

        t.restart();
        int pagesWithHit = 0;
        for (int p = 0; p < doc.pageCount(); ++p)
            if (doc.getAllText(p).text().contains(QLatin1String(word), Qt::CaseInsensitive)) ++pagesWithHit;
        std::printf("getAllText scan '%s': %lld ms, %d pages\n", word, t.elapsed(), pagesWithHit);
    }
    return 0;
}
