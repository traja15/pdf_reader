#pragma once

#include <QColor>
#include <QIcon>
#include <QImage>
#include <QString>

/// App appearance: the window theme (dark by default) and the colours PDF pages are shown in.
namespace Theme {

enum class Mode { Dark, Light, System };
Mode mode();
void setMode(Mode m);
/// True when the window theme in effect is dark.
bool isDark();
/// Applies the window theme (Fusion palette). Call at start-up and after changing the mode.
void apply();

/// Design colours for the current theme (cards, dividers, accent…), used by the stylesheet and
/// by custom-drawn views such as the Home tiles.
struct Tokens {
    QColor window, card, cardTop, border, divider, hover, text, muted, accent, accent2, accentSoft, onAccent, shadow;
};
const Tokens &tokens();

/// Toolbar icon tinted for the current theme.
QIcon icon(const QString &name);

/// How PDF pages are coloured: "dark" (default), "sepia", "original" or "custom".
QString pageMode();
void setPageMode(const QString &mode);
/// Custom text (front) and background (back) colours.
void setCustomPageColors(const QColor &text, const QColor &background);

struct PageColors {
    bool original = false; ///< show the PDF as it is
    QColor text;
    QColor background;
};
/// The colours in effect (cached; cheap to call while painting).
const PageColors &pageColors();
/// Colours for a preset ("dark", "sepia", "custom"…).
PageColors colorsFor(const QString &mode);

/// Recolours a rendered page: white paper becomes the background colour, black text the text colour.
QImage recolor(const QImage &page, const PageColors &colors);
/// Ink colour adjusted so pen strokes stay readable on a dark page (lightness flipped, hue kept).
QColor inkColor(const QColor &ink, bool highlighter);

} // namespace Theme
