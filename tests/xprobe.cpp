// Opens a PDF in the real main window and prints, every 100 ms, the reader's scroll position and
// whether a popup menu is open — so real X11 input (sent from outside with XTest) can be checked.
#include "core/AppContext.h"
#include "core/DocumentStore.h"
#include "ui/MainWindow.h"
#include "ui/ReaderView.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QScrollBar>
#include <QTimer>
#include <cstdio>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("PenReader");
    QApplication::setApplicationName("PenReader");
    Theme::apply();
    AppContext ctx;
    const Document doc = ctx.docs->importFile(argv[1], {}, {});
    MainWindow window(ctx);
    window.resize(1200, 800);
    window.move(0, 0);
    window.show();
    window.openDocument(doc.docKey);
    QTimer t;
    QObject::connect(&t, &QTimer::timeout, [&] {
        ReaderView *r = nullptr;
        for (ReaderView *c : window.findChildren<ReaderView *>()) if (c->isVisible()) r = c;
        if (!r) return;
        const QPoint vp = r->viewport()->mapToGlobal(QPoint(0, 0));
        QWidget *popup = QApplication::activePopupWidget();
        std::printf("vp=%d,%d,%d,%d scroll=%d sel=%d popup=%d\n", vp.x(), vp.y(), r->viewport()->width(),
                    r->viewport()->height(), r->verticalScrollBar()->value(), r->hasSelection() ? 1 : 0, popup ? 1 : 0);
        std::fflush(stdout);
        if (popup && popup->isVisible()) QTimer::singleShot(300, popup, &QWidget::close);
    });
    t.start(100);
    QTimer::singleShot(20000, &app, &QApplication::quit);
    return app.exec();
}
