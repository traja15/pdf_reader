#include "ui/Theme.h"

#include <QApplication>
#include <QFile>
#include <QHash>
#include <QPainter>
#include <QPalette>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QSvgRenderer>
#include <optional>

namespace {

std::optional<Theme::PageColors> s_page; // cache of the colours in effect
QHash<QString, QIcon> s_icons;          // cache of tinted icons for the current theme

QPalette darkPalette()
{
    QPalette p;
    const QColor window(0x20, 0x21, 0x24), base(0x17, 0x18, 0x1A), alt(0x2A, 0x2C, 0x2F);
    const QColor text(0xE8, 0xEA, 0xED), disabled(0x6E, 0x72, 0x77), accent(0x8A, 0xB4, 0xF8);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, alt);
    p.setColor(QPalette::ToolTipBase, alt);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, QColor(0x9A, 0xA0, 0xA6));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, alt);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, QColor(0x3C, 0x5A, 0x99));
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::LinkVisited, QColor(0xC5, 0x8A, 0xF9));
    p.setColor(QPalette::Light, QColor(0x3A, 0x3D, 0x41));
    p.setColor(QPalette::Midlight, QColor(0x32, 0x35, 0x39));
    p.setColor(QPalette::Mid, QColor(0x5F, 0x63, 0x68));
    p.setColor(QPalette::Dark, QColor(0x12, 0x13, 0x14));
    p.setColor(QPalette::Shadow, Qt::black);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, role, disabled);
    return p;
}

} // namespace

namespace Theme {

Mode mode()
{
    const QString m = QSettings().value("appearance/theme", "dark").toString();
    return m == "light" ? Mode::Light : m == "system" ? Mode::System : Mode::Dark;
}

void setMode(Mode m)
{
    QSettings().setValue("appearance/theme", m == Mode::Light ? "light" : m == Mode::System ? "system" : "dark");
}

bool isDark()
{
    switch (mode()) {
    case Mode::Dark: return true;
    case Mode::Light: return false;
    case Mode::System: return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    }
    return true;
}

void apply()
{
    s_icons.clear();
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QApplication::setPalette(isDark() ? darkPalette() : QApplication::style()->standardPalette());
}

QIcon icon(const QString &name)
{
    if (const auto it = s_icons.find(name); it != s_icons.end())
        return *it;
    QFile f(":/resources/icons/" + name + ".svg");
    if (!f.open(QIODevice::ReadOnly))
        return {};
    QByteArray svg = f.readAll();
    if (isDark())
        svg.replace("#455A64", "#C9D1D6").replace("#3c4043", "#C9D1D6"); // dark line icons → light
    QSvgRenderer renderer(svg);
    QImage image(96, 96, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    renderer.render(&p);
    p.end();
    const QIcon result(QPixmap::fromImage(image));
    s_icons.insert(name, result);
    return result;
}

QString pageMode() { return QSettings().value("appearance/pageMode", "dark").toString(); }

void setPageMode(const QString &mode)
{
    QSettings().setValue("appearance/pageMode", mode);
    s_page.reset();
}

void setCustomPageColors(const QColor &text, const QColor &background)
{
    QSettings s;
    s.setValue("appearance/customText", text.name());
    s.setValue("appearance/customBackground", background.name());
    s_page.reset();
}

PageColors colorsFor(const QString &mode)
{
    if (mode == "original") return {true, Qt::black, Qt::white};
    if (mode == "sepia") return {false, QColor(0x5B, 0x46, 0x36), QColor(0xF4, 0xEC, 0xD8)};
    if (mode == "custom") {
        QSettings s;
        return {false, QColor(s.value("appearance/customText", "#E6E1D6").toString()),
                QColor(s.value("appearance/customBackground", "#1C1B1A").toString())};
    }
    return {false, QColor(0xDA, 0xDC, 0xE0), QColor(0x1E, 0x1F, 0x22)}; // dark
}

const PageColors &pageColors()
{
    if (!s_page) s_page = colorsFor(pageMode());
    return *s_page;
}

QImage recolor(const QImage &page, const PageColors &colors)
{
    if (colors.original || page.isNull())
        return page;
    // Map lightness through a 256-entry table: white paper → background, black ink → text colour.
    QRgb lut[256];
    const QColor fg = colors.text, bg = colors.background;
    for (int l = 0; l < 256; ++l) {
        const double t = l / 255.0; // 1 = paper, 0 = ink
        lut[l] = qRgb(qRound(fg.red() + (bg.red() - fg.red()) * t), qRound(fg.green() + (bg.green() - fg.green()) * t),
                      qRound(fg.blue() + (bg.blue() - fg.blue()) * t));
    }
    // PDF pages render with a transparent background, so composite onto white paper first;
    // otherwise empty paper (transparent) would read as black and turn into the text colour.
    const QImage src = page.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage out(src.size(), QImage::Format_RGB32);
    for (int y = 0; y < src.height(); ++y) {
        const auto *in = reinterpret_cast<const QRgb *>(src.constScanLine(y));
        auto *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < src.width(); ++x) {
            const QRgb c = in[x];
            const int paper = 255 - qAlpha(c); // premultiplied: colour + (1 − alpha) × white
            const int r = qRed(c) + paper, g = qGreen(c) + paper, b = qBlue(c) + paper;
            line[x] = lut[(r * 77 + g * 150 + b * 29) >> 8];
        }
    }
    return out;
}

QColor inkColor(const QColor &ink, bool highlighter)
{
    const PageColors &c = pageColors();
    if (c.original || highlighter || c.background.lightnessF() >= 0.5)
        return ink;
    // Dark page: flip lightness so black ink shows as light, navy as light blue, red stays red-ish.
    float h, s, l, a;
    ink.getHslF(&h, &s, &l, &a);
    QColor out;
    out.setHslF(h, s, qMax(1.0f - l, 0.55f), a);
    return out;
}

} // namespace Theme
