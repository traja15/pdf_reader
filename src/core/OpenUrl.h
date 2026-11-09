#pragma once

class QUrl;

/// Opens a URL in the user's default browser. Returns false if nothing could be launched.
bool openUrlInBrowser(const QUrl &url);
