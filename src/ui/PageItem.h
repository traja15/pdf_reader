#pragma once

#include "core/Models.h"

#include <QGraphicsItem>
#include <QImage>
#include <functional>
#include <optional>

/// Scene width of every page. Ink is stored normalised to page width, so scene = norm * kPageWidth.
inline constexpr qreal kPageWidth = 1000.0;

/// One PDF page in the reader scene: the rendered bitmap plus its ink.
class PageItem : public QGraphicsItem {
public:
    /// [requestRender] is called from paint() when a sharper (or first) bitmap is needed.
    PageItem(int page, qreal aspectRatio, std::function<void(int page, int widthPx)> requestRender);

    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

    int page() const { return m_page; }
    qreal height() const { return kPageWidth * m_aspect; }

    void setImage(const QImage &image);
    void clearImage();
    int renderedWidth() const { return m_image.isNull() ? 0 : m_image.width(); }
    int requestedWidth() const { return m_requestedWidth; }

    const QVector<Stroke> &strokes() const { return m_strokes; }
    void setStrokes(const QVector<Stroke> &strokes);
    void setLiveStroke(std::optional<Stroke> stroke);
    /// Eraser cursor position in normalised page coordinates (nullopt hides it).
    void setEraserCursor(std::optional<QPointF> pos, qreal radius);

    static void drawStroke(QPainter *p, const Stroke &s);

private:
    int m_page;
    qreal m_aspect;
    std::function<void(int, int)> m_requestRender;
    QImage m_image;
    int m_requestedWidth = 0;
    QVector<Stroke> m_strokes;
    std::optional<Stroke> m_live;
    std::optional<QPointF> m_eraser;
    qreal m_eraserRadius = 0;
};
