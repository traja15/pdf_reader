#include "ui/PageItem.h"

#include "ui/Theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionGraphicsItem>
#include <cmath>

namespace {
/// Bitmaps are re-rendered in steps so zooming doesn't re-render on every frame.
constexpr int kRenderStep = 256;
constexpr int kMaxRenderWidth = 4096;
} // namespace

PageItem::PageItem(int page, qreal aspectRatio, std::function<void(int, int)> requestRender)
    : m_page(page), m_aspect(aspectRatio), m_requestRender(std::move(requestRender))
{
}

QRectF PageItem::boundingRect() const { return {0, 0, kPageWidth, height()}; }

void PageItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *)
{
    const QRectF rect = boundingRect();
    painter->fillRect(rect.translated(4, 4), QColor(0, 0, 0, 40)); // drop shadow
    painter->fillRect(rect, Theme::pageColors().background); // shown until the page bitmap arrives

    // How many device pixels one scene unit covers right now -> the bitmap width we want.
    const qreal lod = option->levelOfDetailFromTransform(painter->worldTransform());
    const qreal dpr = painter->device() ? painter->device()->devicePixelRatioF() : 1.0;
    int wanted = int(std::ceil(kPageWidth * lod * dpr / kRenderStep)) * kRenderStep;
    wanted = qBound(kRenderStep, wanted, kMaxRenderWidth);
    if (wanted != m_requestedWidth && wanted != renderedWidth()) {
        m_requestedWidth = wanted;
        m_requestRender(m_page, wanted);
    }
    if (!m_image.isNull()) {
        painter->setRenderHint(QPainter::SmoothPixmapTransform);
        painter->drawImage(rect, m_image);
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->scale(kPageWidth, kPageWidth); // draw ink in normalised coordinates
    for (const Stroke &s : m_strokes)
        drawStroke(painter, s);
    if (m_live)
        drawStroke(painter, *m_live);
    if (m_eraser) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(128, 128, 128, 90));
        painter->drawEllipse(*m_eraser, m_eraserRadius, m_eraserRadius);
    }
    painter->restore();
}

void PageItem::drawStroke(QPainter *p, const Stroke &s)
{
    const int n = s.pointCount();
    if (n == 0)
        return;
    const QColor color = Theme::inkColor(QColor::fromRgba(s.color), s.tool == InkTool::Highlighter); // readable on dark pages
    if (s.tool == InkTool::Highlighter) {
        QPainterPath path(QPointF(s.x(0), s.y(0)));
        for (int i = 1; i < n; ++i)
            path.lineTo(s.x(i), s.y(i));
        p->setBrush(Qt::NoBrush);
        p->setPen(QPen(color, s.width, Qt::SolidLine, Qt::SquareCap, Qt::RoundJoin));
        p->drawPath(path);
        return;
    }
    if (n == 1) {
        p->setPen(Qt::NoPen);
        p->setBrush(color);
        p->drawEllipse(QPointF(s.x(0), s.y(0)), s.width / 2, s.width / 2);
        return;
    }
    // Pressure-sensitive width: each segment gets its own thickness (same formula as Android).
    QPen pen(color, s.width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    for (int i = 1; i < n; ++i) {
        const float pr = (s.pressure(i - 1) + s.pressure(i)) / 2.0f;
        pen.setWidthF(s.width * (0.35f + 0.9f * pr));
        p->setPen(pen);
        p->drawLine(QPointF(s.x(i - 1), s.y(i - 1)), QPointF(s.x(i), s.y(i)));
    }
}

void PageItem::setImage(const QImage &image)
{
    m_image = image;
    update();
}

void PageItem::clearImage()
{
    if (m_image.isNull())
        return;
    m_image = QImage();
    m_requestedWidth = 0;
}

void PageItem::setStrokes(const QVector<Stroke> &strokes)
{
    m_strokes = strokes;
    update();
}

void PageItem::setLiveStroke(std::optional<Stroke> stroke)
{
    m_live = std::move(stroke);
    update();
}

void PageItem::setEraserCursor(std::optional<QPointF> pos, qreal radius)
{
    m_eraser = pos;
    m_eraserRadius = radius;
    update();
}
