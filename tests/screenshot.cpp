// Renders the real main window with whatever settings/data XDG_* point at, and saves screenshots.
// Run on a virtual display (xvfb-run) to see exactly what the X11 build draws, without a visible window.
//   xvfb-run -a env XDG_DATA_HOME=… XDG_CONFIG_HOME=… ./penreader_screenshot out-dir

#include "core/AppContext.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QTabWidget>
#include <QTest>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("PenReader");
    QApplication::setApplicationName("PenReader");
    Theme::apply();
    const QString out = argc > 1 ? argv[1] : ".";
    AppContext ctx;
    MainWindow window(ctx);
    window.resize(1400, 900);
    window.show();
    QTest::qWait(4000); // session restore + rendering
    auto *tabs = qobject_cast<QTabWidget *>(window.centralWidget());
    for (int i = 0; i < tabs->count(); ++i) {
        tabs->setCurrentIndex(i);
        QTest::qWait(1500);
        window.grab().save(QStringLiteral("%1/tab%2.png").arg(out).arg(i));
    }
    return 0;
}
