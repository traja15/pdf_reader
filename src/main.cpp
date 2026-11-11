#include "core/AppContext.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QMessageBox>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("PenReader");
    QApplication::setApplicationName("PenReader");
    QApplication::setApplicationVersion("0.1.0");
    Theme::apply(); // dark by default; Settings → Appearance
    QApplication::setWindowIcon(QIcon(":/resources/icons/app.svg"));

    std::unique_ptr<AppContext> ctx;
    try {
        ctx = std::make_unique<AppContext>();
    } catch (const std::exception &e) {
        QMessageBox::critical(nullptr, "PenReader", QString::fromStdString(e.what()));
        return 1;
    }

    MainWindow window(*ctx);
    window.show();
    // PDFs passed on the command line ("Open with PenReader").
    const QStringList args = app.arguments();
    if (args.size() > 1)
        window.openFile(args.last());
    return app.exec();
}
