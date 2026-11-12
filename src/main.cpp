#include "core/AppContext.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QMessageBox>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("PenReader");
    QApplication::setApplicationName("PenReader");
    QApplication::setApplicationVersion("0.1.0");
    // Mouse diagnostics: if ~/.cache/PenReader/mouse-debug exists, log mouse events to mouse.log there.
    {
        static QString logPath;
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/PenReader";
        if (QFile::exists(dir + "/mouse-debug")) {
            logPath = dir + "/mouse.log";
            QFile::remove(logPath);
            QLoggingCategory::setFilterRules("penreader.mouse=true");
            static QtMessageHandler previous = qInstallMessageHandler(
                [](QtMsgType type, const QMessageLogContext &ctx, const QString &msg) {
                    if (ctx.category && QByteArray(ctx.category) == "penreader.mouse") {
                        QFile f(logPath);
                        if (f.open(QIODevice::Append | QIODevice::Text))
                            f.write((QDateTime::currentDateTime().toString("HH:mm:ss.zzz ") + msg + "\n").toUtf8());
                        return;
                    }
                    if (previous) previous(type, ctx, msg);
                });
        }
    }
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
