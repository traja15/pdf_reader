#include "core/OpenUrl.h"

#include <QDesktopServices>
#include <QProcess>
#include <QUrl>

bool openUrlInBrowser(const QUrl &url)
{
#ifdef Q_OS_LINUX
    // Qt may route this through the XDG desktop portal, which silently does nothing on plain
    // window managers (dwm, i3, …). xdg-open uses the configured default browser directly.
    if (QProcess::startDetached("xdg-open", {url.toString(QUrl::FullyEncoded)}))
        return true;
#endif
    return QDesktopServices::openUrl(url);
}
