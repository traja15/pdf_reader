#pragma once

#include <QtGlobal>

enum class ReaderTool { Pen, Highlighter, Eraser, Lookup };

struct ToolSettings {
    ReaderTool tool = ReaderTool::Pen;
    quint32 penColor = 0xFF1A237E;
    quint32 highlighterColor = 0x66FFEB3B;
    float penWidth = 0.0028f;        ///< Fraction of page width (same units as Android).
    float highlighterWidth = 0.018f;
    /// Mouse (and touch) draw with the current tool. When off, only a pen tablet draws and the
    /// mouse pans, like the S Pen-only mode on Android.
    bool mouseDraws = true;
};
