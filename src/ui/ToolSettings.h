#pragma once

#include <QtGlobal>

/// Hand = the normal mouse pointer (the default): an arrow, which turns into a hand that drags
/// the page whenever the page is zoomed in. Pen tablets drag the same way.
enum class ReaderTool { Pen, Highlighter, Eraser, Lookup, Hand };

struct ToolSettings {
    ReaderTool tool = ReaderTool::Hand;
    quint32 penColor = 0xFF1A237E;
    quint32 highlighterColor = 0x66FFEB3B;
    float penWidth = 0.0028f;        ///< Fraction of page width (same units as Android).
    float highlighterWidth = 0.018f;
    /// Mouse (and touch) draw with the current tool. When off, only a pen tablet draws and the
    /// mouse pans, like the S Pen-only mode on Android.
    bool mouseDraws = true;
};
