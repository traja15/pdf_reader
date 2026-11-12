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
Theme::Tokens s_tokens;

Theme::Tokens makeTokens(bool dark)
{
    if (dark)
        return {QColor("#16171a"), QColor("#1e1f23"), QColor("#26282d"), QColor("#2f3237"), QColor("#2a2c31"),
                QColor(255, 255, 255, 16), QColor("#e8eaed"), QColor("#9aa0a6"), QColor("#8ab4f8"), QColor("#b39ddb"),
                QColor(138, 180, 248, 46), QColor("#0d1117"), QColor(0, 0, 0, 110)};
    return {QColor("#f2f3f6"), QColor("#ffffff"), QColor("#ffffff"), QColor("#e1e3e8"), QColor("#e9ebef"),
            QColor(26, 35, 126, 12), QColor("#1f2124"), QColor("#5f6368"), QColor("#3f51b5"), QColor("#7e57c2"),
            QColor(63, 81, 181, 30), QColor("#ffffff"), QColor(31, 38, 64, 40)};
}

QString rgba(const QColor &c) { return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha()); }

QString styleSheet(const Theme::Tokens &t, bool dark)
{
    // A soft top-lit gradient on bars and buttons gives the glossy look without hurting legibility.
    const QString barTop = dark ? "#24262b" : "#ffffff", barBottom = dark ? "#1b1c20" : "#f4f5f8";
    const QString btnTop = dark ? "#2d3036" : "#ffffff", btnBottom = dark ? "#24262b" : "#eef0f4";
    const QString input = dark ? "#141518" : "#ffffff";
    QString css = QStringLiteral(R"(
QMainWindow, QDialog { background: %window; }
QToolBar { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %barTop, stop:1 %barBottom);
           border: none; border-bottom: 1px solid %divider; padding: 3px 6px; spacing: 2px; }
QToolBar::separator { background: %divider; width: 1px; margin: 6px 6px; }
QToolBar QToolButton { border: 1px solid transparent; border-radius: 8px; padding: 4px; margin: 1px; color: %text; }
QToolBar QToolButton:hover { background: %hover; border-color: %border; }
QToolBar QToolButton:pressed { background: %accentSoft; }
QToolBar QToolButton:checked { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %accentSoft, stop:1 %accentSoftEnd);
                               border: 1px solid %accentLine; }
QTabWidget::pane { border: none; border-top: 1px solid %divider; top: -1px; }
QTabWidget::tab-bar { left: 8px; }
QTabBar { qproperty-drawBase: 0; }
QTabBar::tab { background: transparent; color: %muted; padding: 6px 14px; margin: 5px 2px 0 2px; min-width: 60px;
               border: 1px solid transparent; border-bottom: none; border-top-left-radius: 9px; border-top-right-radius: 9px; }
QTabBar::tab:selected { color: %text; border-color: %border;
                        background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %cardTop, stop:1 %window); }
QTabBar::tab:hover:!selected { background: %hover; color: %text; }
QPushButton { color: %text; border: 1px solid %border; border-radius: 8px; padding: 5px 14px;
              background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %btnTop, stop:1 %btnBottom); }
QPushButton:hover { border-color: %accentLine; }
QPushButton:pressed { background: %btnBottom; }
QPushButton:disabled { color: %muted; }
QPushButton:default, QPushButton[accent="true"] { color: %onAccent; border: none; font-weight: 600;
              background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 %accent, stop:1 %accent2); }
QPushButton:flat { background: transparent; border: none; color: %accent; }
QPushButton:flat:hover { text-decoration: underline; }
QLineEdit, QSpinBox, QDoubleSpinBox, QPlainTextEdit, QTextEdit, QTextBrowser {
              background: %input; color: %text; border: 1px solid %border; border-radius: 8px; padding: 4px 8px;
              selection-background-color: %accentSel; }
QLineEdit:focus, QSpinBox:focus, QPlainTextEdit:focus, QTextEdit:focus { border: 1px solid %accent; }
QComboBox { background: %input; color: %text; border: 1px solid %border; border-radius: 8px; padding: 4px 8px; }
QComboBox:focus { border-color: %accent; }
QComboBox QAbstractItemView { background: %card; border: 1px solid %border; selection-background-color: %accentSel; }
QMenu { background: %card; color: %text; border: 1px solid %border; border-radius: 10px; padding: 6px; }
QMenu::item { padding: 6px 26px 6px 10px; border-radius: 6px; background: transparent; }
QMenu::item:selected { background: %accentSel; }
QMenu::item:disabled { color: %muted; }
QMenu::separator { height: 1px; background: %divider; margin: 5px 8px; }
QMenu::icon { padding-left: 8px; }
QToolTip { background: %card; color: %text; border: 1px solid %border; border-radius: 6px; padding: 4px 8px; }
QStatusBar { background: %window; color: %muted; border-top: 1px solid %divider; }
QStatusBar::item { border: none; }
QScrollBar:vertical { background: transparent; width: 11px; margin: 2px; }
QScrollBar:horizontal { background: transparent; height: 11px; margin: 2px; }
QScrollBar::handle { background: %scroll; border-radius: 4px; min-height: 32px; min-width: 32px; }
QScrollBar::handle:hover { background: %scrollHover; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QSplitter::handle { background: transparent; }
QDockWidget { color: %text; }
QDockWidget::title { background: %card; padding: 7px 10px; border-bottom: 1px solid %divider; }
QTreeView, QListView { background: transparent; border: none; outline: 0; color: %text; show-decoration-selected: 0; }
QTreeView::item, QListView::item { border-radius: 7px; padding: 4px 2px; }
QTreeView::item:hover, QListView::item:hover { background: %hover; }
QTreeView::item:selected, QListView::item:selected { background: %accentSel; color: %text; }
QTreeView::branch, QTreeView::branch:selected, QTreeView::branch:hover { background: transparent; }
QTabBar::close-button { image: url(:/resources/close-%closeTone.svg); subcontrol-position: right; margin: 2px; border-radius: 5px; }
QTabBar::close-button:hover { background: %hover; }
QFrame#card { border: 1px solid %border; border-radius: 14px;
              background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %cardTop, stop:1 %card); }
QFrame#divider { background: %divider; border: none; min-height: 1px; max-height: 1px; }
QFrame#banner { border: 1px solid %accentLine; border-radius: 14px;
                background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 %accentSoft, stop:1 %accent2Soft); }
QLabel#cardTitle { color: %text; font-weight: 600; }
QLabel#muted { color: %muted; }
QLabel#count { color: %accent; background: %accentSoft; border-radius: 9px; padding: 1px 8px; font-weight: 600; }
QLabel#pageTitle { color: %text; font-weight: 700; }
)");
    QColor soft2 = t.accent2;
    soft2.setAlpha(t.accentSoft.alpha());
    QColor softEnd = t.accentSoft;
    softEnd.setAlpha(t.accentSoft.alpha() / 2);
    QColor line = t.accent;
    line.setAlpha(dark ? 110 : 120);
    QColor sel = t.accent;
    sel.setAlpha(dark ? 70 : 45);
    const QColor scroll = dark ? QColor(255, 255, 255, 46) : QColor(0, 0, 0, 50);
    const QColor scrollHover = dark ? QColor(255, 255, 255, 90) : QColor(0, 0, 0, 90);
    const QList<std::pair<QString, QString>> vars = {
        {"%accentSoftEnd", rgba(softEnd)}, {"%accentSoft", rgba(t.accentSoft)}, {"%accent2Soft", rgba(soft2)},
        {"%accentLine", rgba(line)}, {"%accentSel", rgba(sel)}, {"%accent2", t.accent2.name()}, {"%accent", t.accent.name()},
        {"%onAccent", t.onAccent.name()}, {"%window", t.window.name()}, {"%cardTop", t.cardTop.name()},
        {"%card", t.card.name()}, {"%border", t.border.name()}, {"%divider", t.divider.name()}, {"%hover", rgba(t.hover)},
        {"%text", t.text.name()}, {"%muted", t.muted.name()}, {"%barTop", barTop}, {"%barBottom", barBottom},
        {"%btnTop", btnTop}, {"%btnBottom", btnBottom}, {"%input", input}, {"%scrollHover", rgba(scrollHover)},
        {"%scroll", rgba(scroll)}, {"%closeTone", dark ? "light" : "dark"}};
    for (const auto &[k, v] : vars) css.replace(k, v);
    return css;
}

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
    const bool dark = isDark();
    s_tokens = makeTokens(dark);
    QPalette palette = dark ? darkPalette() : QApplication::style()->standardPalette();
    palette.setColor(QPalette::Window, s_tokens.window);
    palette.setColor(QPalette::Highlight, dark ? QColor(0x3C, 0x5A, 0x99) : s_tokens.accent);
    QApplication::setPalette(palette);
    qApp->setStyleSheet(styleSheet(s_tokens, dark));
}

const Tokens &tokens()
{
    if (!s_tokens.window.isValid()) s_tokens = makeTokens(isDark());
    return s_tokens;
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
