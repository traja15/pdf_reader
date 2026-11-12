#include "ui/HomeDelegates.h"

#include "ui/Theme.h"

#include <QAbstractItemView>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>

namespace {

/// Up to [maxLines] lines of [text] wrapped into [width], the last one elided.
QStringList wrapLines(const QString &text, const QFont &font, int width, int maxLines)
{
    QStringList lines;
    QTextLayout layout(text, font);
    layout.beginLayout();
    for (;;) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        if (lines.size() == maxLines - 1) {
            lines << QFontMetrics(font).elidedText(text.mid(line.textStart()).simplified(), Qt::ElideRight, width);
            break;
        }
        lines << text.mid(line.textStart(), line.textLength()).trimmed();
    }
    layout.endLayout();
    return lines;
}

void softShadow(QPainter *p, const QRectF &r, qreal radius, const QColor &shadow)
{
    p->setPen(Qt::NoPen);
    for (int i = 4; i >= 1; --i) {
        QColor c = shadow;
        c.setAlphaF(shadow.alphaF() * (0.28 / i));
        p->setBrush(c);
        p->drawRoundedRect(r.adjusted(-i * 0.5, i * 0.6, i * 0.5, i * 1.4), radius + i, radius + i);
    }
}

} // namespace

void TileDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const Theme::Tokens &t = Theme::tokens();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setRenderHint(QPainter::SmoothPixmapTransform);
    const bool enabled = index.flags() & Qt::ItemIsEnabled;
    if (!enabled) p->setOpacity(0.5);

    const QRectF card = QRectF(option.rect).adjusted(5, 5, -5, -5);
    const bool selected = option.state & QStyle::State_Selected;
    const bool hover = option.state & QStyle::State_MouseOver;
    if (selected || hover) {
        p->setPen(selected ? QPen(t.accent, 1.5) : Qt::NoPen);
        p->setBrush(selected ? t.accentSoft : t.hover);
        p->drawRoundedRect(card, 12, 12);
    }

    const bool folder = index.data(HomeRoles::Kind).toString() == "folder";
    const qreal side = qMin(card.width() - 26, 150.0);
    const QRectF cover(card.center().x() - side / 2, card.top() + 12, side, side);
    softShadow(p, cover, 8, t.shadow);
    QPainterPath clip;
    clip.addRoundedRect(cover, 8, 8);
    if (folder) {
        QLinearGradient g(cover.topLeft(), cover.bottomRight());
        g.setColorAt(0, t.accentSoft);
        QColor end = t.accent2;
        end.setAlpha(t.accentSoft.alpha());
        g.setColorAt(1, end);
        p->fillPath(clip, t.card);
        p->fillPath(clip, g);
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        const int s = int(side * 0.5);
        icon.paint(p, QRect(int(cover.center().x() - s / 2), int(cover.center().y() - s / 2), s, s));
    } else {
        p->fillPath(clip, Qt::white);
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
        const QPixmap pm = icon.pixmap(QSize(int(side), int(side)), dpr);
        // Placeholder icons (in the cloud / not rendered yet) have transparent corners; covers don't.
        const bool placeholder = !pm.isNull() && qAlpha(pm.toImage().pixel(1, 1)) < 10;
        p->save();
        p->setClipPath(clip);
        if (placeholder) {
            p->fillPath(clip, t.card);
            p->fillPath(clip, t.accentSoft);
            const int s = int(side * 0.42);
            icon.paint(p, QRect(int(cover.center().x() - s / 2), int(cover.center().y() - s / 2), s, s));
        } else if (!pm.isNull()) {
            // Cover-fit the thumbnail into the rounded square.
            const QSizeF s = pm.deviceIndependentSize().scaled(cover.size(), Qt::KeepAspectRatioByExpanding);
            p->drawPixmap(QRectF(cover.center().x() - s.width() / 2, cover.top(), s.width(), s.height()), pm, pm.rect());
        }
        p->restore();
        // Glossy sheen across the top of the cover.
        QLinearGradient sheen(cover.topLeft(), QPointF(cover.left(), cover.top() + side * 0.45));
        sheen.setColorAt(0, QColor(255, 255, 255, 38));
        sheen.setColorAt(1, QColor(255, 255, 255, 0));
        p->fillPath(clip, sheen);
    }
    p->setPen(QPen(t.border, 1));
    p->setBrush(Qt::NoBrush);
    p->drawPath(clip);

    // Title (two lines) and status.
    const QStringList parts = index.data(Qt::DisplayRole).toString().split('\n');
    QFont titleFont = option.font;
    titleFont.setWeight(index.data(Qt::FontRole).value<QFont>().bold() ? QFont::Bold : QFont::DemiBold);
    const int textWidth = int(card.width() - 16);
    const QStringList title = wrapLines(parts.value(0), titleFont, textWidth, 2);
    p->setFont(titleFont);
    p->setPen(t.text);
    const QFontMetrics fm(titleFont);
    qreal y = cover.bottom() + 10;
    for (const QString &line : title) {
        p->drawText(QRectF(card.left() + 8, y, textWidth, fm.height()), Qt::AlignHCenter | Qt::AlignVCenter, line);
        y += fm.height();
    }

    QFont small = option.font;
    small.setPointSizeF(small.pointSizeF() * 0.88);
    const QFontMetrics sfm(small);
    const qreal bottom = card.bottom() - 8;
    const int percent = index.data(HomeRoles::Percent).isValid() ? index.data(HomeRoles::Percent).toInt() : -1;
    if (!folder && percent >= 0) {
        const QRectF track(cover.left() + 6, bottom - sfm.height() - 9, side - 12, 4);
        p->setPen(Qt::NoPen);
        p->setBrush(t.divider);
        p->drawRoundedRect(track, 2, 2);
        if (percent > 0) {
            QLinearGradient g(track.topLeft(), track.topRight());
            g.setColorAt(0, t.accent);
            g.setColorAt(1, t.accent2);
            p->setBrush(g);
            p->drawRoundedRect(QRectF(track.topLeft(), QSizeF(qMax(4.0, track.width() * percent / 100.0), 4)), 2, 2);
        }
    }
    p->setFont(small);
    p->setPen(t.muted);
    p->drawText(QRectF(card.left() + 8, bottom - sfm.height(), textWidth, sfm.height()), Qt::AlignHCenter | Qt::AlignVCenter,
                sfm.elidedText(parts.value(1), Qt::ElideRight, textWidth));
    p->restore();
}

QSize WordDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    if (!(index.flags() & Qt::ItemIsEnabled))
        return {option.rect.width(), 70};
    const QFontMetrics fm(option.font);
    return {1, fm.height() * 3 + 22}; // full width of the list; never wider (no sideways scrolling)
}

void WordDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    const Theme::Tokens &t = Theme::tokens();
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(option.rect).adjusted(2, 1, -2, -1);
    if (!(index.flags() & Qt::ItemIsEnabled)) {
        p->setPen(t.muted);
        p->drawText(r.adjusted(8, 0, -8, 0), Qt::AlignCenter | Qt::TextWordWrap, index.data().toString());
        p->restore();
        return;
    }
    if (option.state & (QStyle::State_MouseOver | QStyle::State_Selected)) {
        p->setPen(Qt::NoPen);
        p->setBrush(option.state & QStyle::State_Selected ? t.accentSoft : t.hover);
        p->drawRoundedRect(r, 9, 9);
    }
    const int x = int(r.left()) + 10, w = int(r.width()) - 20;
    QFont bold = option.font;
    bold.setWeight(QFont::DemiBold);
    bold.setPointSizeF(bold.pointSizeF() * 1.05);
    const QFontMetrics bfm(bold), fm(option.font);
    qreal y = r.top() + 8;
    const QString word = index.data(HomeRoles::Id).toString();
    p->setFont(bold);
    p->setPen(t.text);
    p->drawText(QPointF(x, y + bfm.ascent()), word);
    const QString tamil = index.data(HomeRoles::Tamil).toString();
    if (!tamil.isEmpty()) {
        p->setFont(option.font);
        p->setPen(t.accent);
        const int wx = x + bfm.horizontalAdvance(word) + 10;
        p->drawText(QPointF(wx, y + bfm.ascent()), fm.elidedText(tamil, Qt::ElideRight, qMax(0, x + w - wx)));
    }
    y += bfm.height() + 3;
    p->setFont(option.font);
    p->setPen(t.muted);
    for (const QString &line : wrapLines(index.data(HomeRoles::Detail).toString(), option.font, w, 2)) {
        p->drawText(QPointF(x, y + fm.ascent()), line);
        y += fm.height();
    }
    // Partition line between words.
    if (index.row() < index.model()->rowCount() - 1) {
        p->setPen(QPen(t.divider, 1));
        p->drawLine(QPointF(r.left() + 10, r.bottom()), QPointF(r.right() - 10, r.bottom()));
    }
    p->restore();
}
