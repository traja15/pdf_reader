#pragma once

#include <QSettings>
#include <QString>

/// Non-shortcut preferences shown on the Settings page (stored in QSettings).
namespace Prefs {

/// Pixels moved by one j/k (or arrow key) press.
inline int scrollStep() { return QSettings().value("reading/scrollStep", 60).toInt(); }
/// Letters used for link hints after pressing f.
inline QString hintChars() { return QSettings().value("reading/hintChars", "asdfghjkl").toString(); }
/// Reopen the tabs that were open when the app was closed.
inline bool restoreTabs() { return QSettings().value("reading/restoreTabs", true).toBool(); }
/// Minutes between automatic syncs while the app is open (0 = off).
inline int syncInterval() { return QSettings().value("sync/intervalMinutes", 5).toInt(); }
/// Push changes to the cloud before the app closes.
inline bool syncOnClose() { return QSettings().value("sync/onClose", true).toBool(); }

} // namespace Prefs
