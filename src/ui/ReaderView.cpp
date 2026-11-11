#include "ui/ReaderView.h"

#include "core/AppContext.h"
#include "core/DocumentStore.h"
#include "core/OpenUrl.h"
#include "ui/PageItem.h"
#include "ui/Prefs.h"
#include "ui/Theme.h"

#include <QContextMenuEvent>
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>
#include <QKeyEvent>
#include <QGraphicsPathItem>
#include <QMenu>
#include <QNativeGestureEvent>
#include <QPdfDocument>
#include <QPdfPageRenderer>
#include <QPdfLinkModel>
#include <QPdfSearchModel>
#include <QPdfSelection>
#include <QSettings>
#include <QScrollBar>
#include <QTabletEvent>
#include <QToolTip>
#include <QTouchEvent>
#include <QUuid>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace {
constexpr qreal kGap = 24;
constexpr qreal kMargin = 30;
constexpr float kEraserRadius = 0.012f;
constexpr qreal kMinZoom = 0.25;
constexpr qreal kMaxZoom = 6.0;

bool isWordChar(QChar c) { return c.isLetterOrNumber() || c == '\'' || c == u'’' || c == '-'; }
bool isSentenceEnd(QChar c) { return c == '.' || c == '?' || c == '!' || c == '\n' || c == '\r'; }
} // namespace

ReaderView::ReaderView(AppContext &ctx, QWidget *parent) : QGraphicsView(parent), m_ctx(ctx)
{
    setScene(new QGraphicsScene(this));
    setBackgroundBrush(Theme::isDark() ? QColor(0x12, 0x13, 0x14) : QColor(0x5f, 0x63, 0x68));
    setFrameShape(QFrame::NoFrame);
    setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);
    setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
    setTransformationAnchor(QGraphicsView::NoAnchor);
    setResizeAnchor(QGraphicsView::NoAnchor);
    setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    viewport()->setAttribute(Qt::WA_AcceptTouchEvents);

    m_saveTimer.setSingleShot(true);
    m_saveTimer.setInterval(500);
    connect(&m_saveTimer, &QTimer::timeout, this, &ReaderView::flushProgress);
    m_evictTimer.setSingleShot(true);
    m_evictTimer.setInterval(300);
    connect(&m_evictTimer, &QTimer::timeout, this, &ReaderView::evictFarPages);
    // Search results arrive progressively; redraw highlights in batches.
    m_searchTimer.setSingleShot(true);
    m_searchTimer.setInterval(120);
    connect(&m_searchTimer, &QTimer::timeout, this, &ReaderView::onSearchResults);
    m_searchSettle.setSingleShot(true);
    m_searchSettle.setInterval(1200);
    connect(&m_searchSettle, &QTimer::timeout, this, [this] {
        // Nothing after the starting point once results stopped coming: wrap around.
        if (m_searchPending && m_search && m_search->count() > 0) {
            const int i = searchIndexFrom(m_searchFrom, !m_searchBackward, true);
            m_searchPending = false;
            if (i >= 0) { recordJump(); showSearchResult(i); }
        }
    });
    setFocusPolicy(Qt::StrongFocus);
    setTools(m_tools);

    // Always-visible scroll bar; dragging it shows which page you'll land on.
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    verticalScrollBar()->setStyleSheet("QScrollBar:vertical { width: 14px; }");
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!verticalScrollBar()->isSliderDown() || m_pages.isEmpty())
            return;
        QToolTip::showText(QCursor::pos() - QPoint(150, 0),
                           QStringLiteral("Page %1 of %2").arg(m_position.page + 1).arg(m_pages.size()), this);
    });
    connect(verticalScrollBar(), &QScrollBar::sliderReleased, this, [] { QToolTip::hideText(); });
}

ReaderView::~ReaderView() { close(); }

void ReaderView::refreshColors()
{
    setBackgroundBrush(Theme::isDark() ? QColor(0x12, 0x13, 0x14) : QColor(0x5f, 0x63, 0x68));
    for (PageItem *item : std::as_const(m_pages)) {
        item->clearImage(); // re-rendered in the new colours as they come into view
        item->update();
    }
}

// ---------------------------------------------------------------- open / close

bool ReaderView::open(const Document &doc, int initialPage, double initialOffset)
{
    close();
    auto *pdf = new QPdfDocument(this);
    if (pdf->load(doc.localPath) != QPdfDocument::Error::None || pdf->pageCount() == 0) {
        delete pdf;
        return false;
    }
    m_pdf = pdf;
    m_doc = doc;
    m_renderer = new QPdfPageRenderer(this);
    m_renderer->setRenderMode(QPdfPageRenderer::RenderMode::MultiThreaded);
    m_renderer->setDocument(m_pdf);
    connect(m_renderer, &QPdfPageRenderer::pageRendered, this, [this](int page, QSize, const QImage &image) {
        if (page < 0 || page >= m_pages.size())
            return;
        PageItem *item = m_pages[page];
        // Ignore stale lower/higher-resolution renders that finished late.
        if (item->requestedWidth() == image.width() || item->renderedWidth() == 0)
            item->setImage(Theme::recolor(image, Theme::pageColors())); // dark / sepia / custom reading colours
    });

    for (int i = 0; i < m_pdf->pageCount(); ++i)
        m_pagePoints.append(m_pdf->pagePointSize(i));
    layoutPages();
    const auto ink = m_ctx.docs->loadInk(doc.docKey);
    for (auto it = ink.begin(); it != ink.end(); ++it)
        if (it.key() >= 0 && it.key() < m_pages.size())
            m_pages[it.key()]->setStrokes(it.value());

    const int last = m_pages.size() - 1;
    m_start = initialPage >= 0 ? Position{qBound(0, initialPage, last), qBound(0.0, initialOffset, 1.0)}
                               : Position{qBound(0, doc.lastPage, last), qBound(0.0, doc.lastOffset, 1.0)};
    m_position = m_start;
    m_userMoved = false;
    m_undo.clear();
    m_redo.clear();
    m_zoom = qBound(kMinZoom, QSettings().value("zoom/" + doc.docKey, 1.0).toDouble(), kMaxZoom); // per-PDF zoom
    m_restoring = true; // layout changes below aren't the user scrolling
    applyTransform();
    m_restoring = false;
    // Wait for the viewport to have its final size before scrolling to the saved spot.
    QTimer::singleShot(0, this, [this] { if (m_pdf) jumpTo(m_start.page, m_start.offset); });
    emit undoStateChanged();
    emit pageChanged(m_position.page, m_pages.size());
    return true;
}

void ReaderView::close()
{
    if (!m_pdf)
        return;
    flushProgress();
    m_hints.clear();
    m_keyMode = KeyMode::None;
    delete m_search; // before the document it searches
    m_search = nullptr;
    m_searchText.clear();
    m_searchIndex = -1;
    m_searchItem = nullptr;
    m_currentMatchItem = nullptr;
    m_jumpBack.clear();
    m_jumpForward.clear();
    scene()->clear();
    m_pages.clear();
    m_pageTops.clear();
    m_pagePoints.clear();
    m_highlight = nullptr;
    delete m_renderer;
    m_renderer = nullptr;
    delete m_pdf;
    m_pdf = nullptr;
    m_undo.clear();
    m_redo.clear();
    m_drawing = false;
}

// ---------------------------------------------------------------- layout & rendering

void ReaderView::layoutPages()
{
    qreal y = kMargin;
    for (int i = 0; i < m_pagePoints.size(); ++i) {
        const QSizeF pts = m_pagePoints[i];
        const qreal aspect = pts.width() > 0 ? pts.height() / pts.width() : 1.414;
        auto *item = new PageItem(i, aspect, [this](int page, int w) { requestRender(page, w); });
        item->setPos(0, y);
        scene()->addItem(item);
        m_pages.append(item);
        m_pageTops.append(y);
        y += item->height() + kGap;
    }
    scene()->setSceneRect(-kMargin, 0, kPageWidth + 2 * kMargin, y + kMargin);
}

void ReaderView::applyTransform()
{
    if (m_pages.isEmpty())
        return;
    const bool sideways = m_rotation % 180 != 0;
    const qreal contentWidth = sideways ? m_pages[m_position.page]->height() + 2 * kMargin : kPageWidth + 2 * kMargin;
    const qreal fit = qMax(0.05, viewport()->width() / contentWidth);
    QTransform t;
    t.rotate(m_rotation);
    t.scale(fit * m_zoom, fit * m_zoom);
    setTransform(t);
    if (m_pdf) QSettings().setValue("zoom/" + m_doc.docKey, m_zoom);
    emit zoomChanged(qRound(m_zoom * 100));
}

void ReaderView::requestRender(int page, int widthPx)
{
    if (!m_renderer)
        return;
    const QSizeF pts = m_pagePoints[page];
    m_renderer->requestPage(page, QSize(widthPx, qRound(widthPx * pts.height() / pts.width())));
    m_evictTimer.start();
}

void ReaderView::evictFarPages()
{
    // Keep bitmaps only for pages near the screen; a long book at full resolution won't fit in RAM.
    const QRectF visible = mapToScene(viewport()->rect()).boundingRect();
    const QRectF keep = visible.adjusted(0, -visible.height() * 1.5, 0, visible.height() * 1.5);
    for (PageItem *item : std::as_const(m_pages))
        if (!item->sceneBoundingRect().intersects(keep))
            item->clearImage();
}

int ReaderView::pageAt(qreal sceneY) const
{
    if (m_pageTops.isEmpty())
        return 0;
    const auto it = std::upper_bound(m_pageTops.begin(), m_pageTops.end(), sceneY);
    return qBound(0, int(it - m_pageTops.begin()) - 1, int(m_pageTops.size()) - 1);
}

std::optional<std::pair<int, QPointF>> ReaderView::pageAndNorm(const QPointF &viewportPos) const
{
    if (m_pages.isEmpty())
        return std::nullopt;
    const QPointF scenePos = viewportTransform().inverted().map(viewportPos);
    const int page = pageAt(scenePos.y());
    const QRectF bounds = m_pages[page]->sceneBoundingRect();
    if (!bounds.contains(scenePos))
        return std::nullopt;
    return std::make_pair(page, (scenePos - bounds.topLeft()) / kPageWidth);
}

ReaderView::Position ReaderView::visiblePosition() const
{
    if (m_pages.isEmpty())
        return {};
    // Same meaning as on Android: which page is at the top of the screen and how far down it.
    const QRectF visible = mapToScene(viewport()->rect()).boundingRect();
    const qreal y = qMax(visible.top(), m_pageTops.first());
    const int page = pageAt(y);
    const qreal offset = (y - m_pageTops[page]) / m_pages[page]->height();
    return {page, qBound(0.0, offset, 1.0)};
}

void ReaderView::updatePosition()
{
    if (!m_pdf)
        return;
    const Position pos = visiblePosition();
    if (!m_restoring && (pos.page != m_start.page || std::abs(pos.offset - m_start.offset) > 0.05))
        m_userMoved = true;
    const bool pageChangedNow = pos.page != m_position.page;
    m_position = pos;
    if (pageChangedNow)
        emit pageChanged(pos.page, m_pages.size());
    m_saveTimer.start();
    m_evictTimer.start();
}

void ReaderView::scrollContentsBy(int dx, int dy)
{
    QGraphicsView::scrollContentsBy(dx, dy);
    updatePosition();
}

void ReaderView::jumpTo(int page, double offset)
{
    if (m_pages.isEmpty())
        return;
    page = qBound(0, page, int(m_pages.size()) - 1);
    m_restoring = true;
    const QRectF visible = mapToScene(viewport()->rect()).boundingRect();
    const qreal y = m_pageTops[page] + offset * m_pages[page]->height();
    centerOn(QPointF(visible.center().x(), y + visible.height() / 2));
    updatePosition();
    m_restoring = false;
    m_position = {page, offset};
    emit pageChanged(page, m_pages.size());
}

void ReaderView::flushProgress()
{
    m_saveTimer.stop();
    if (m_pdf)
        m_ctx.docs->saveProgress(m_doc.docKey, m_position.page, m_position.offset);
}

void ReaderView::resizeEvent(QResizeEvent *event)
{
    // A resize (or a background tab being shown for the first time) isn't the user scrolling.
    const Position keep = m_position;
    m_restoring = true;
    QGraphicsView::resizeEvent(event);
    applyTransform();
    if (m_pdf)
        jumpTo(keep.page, keep.offset);
    m_restoring = false;
}

// ---------------------------------------------------------------- zoom & rotation

void ReaderView::setZoom(qreal zoom, const QPointF &anchor)
{
    zoom = qBound(kMinZoom, zoom, kMaxZoom);
    if (qFuzzyCompare(zoom, m_zoom))
        return;
    const QPointF sceneAnchor = viewportTransform().inverted().map(anchor);
    m_zoom = zoom;
    applyTransform();
    // Keep the content under the cursor/fingers in place.
    const QPointF delta = viewportTransform().map(sceneAnchor) - anchor;
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() + qRound(delta.x()));
    verticalScrollBar()->setValue(verticalScrollBar()->value() + qRound(delta.y()));
}

void ReaderView::zoomIn() { setZoom(m_zoom * 1.25, viewport()->rect().center()); }
void ReaderView::zoomOut() { setZoom(m_zoom / 1.25, viewport()->rect().center()); }

void ReaderView::fitWidth()
{
    const Position keep = m_position;
    m_zoom = 1.0;
    m_restoring = true;
    applyTransform();
    jumpTo(keep.page, keep.offset);
}

void ReaderView::rotateClockwise() { setRotation((m_rotation + 90) % 360); }

void ReaderView::setRotation(int degrees)
{
    const Position keep = m_position;
    m_rotation = ((degrees % 360) + 360) % 360;
    m_restoring = true;
    applyTransform();
    if (m_pdf)
        jumpTo(keep.page, keep.offset);
    m_restoring = false;
}

void ReaderView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        setZoom(m_zoom * std::pow(1.0015, event->angleDelta().y()), event->position());
        event->accept();
        return;
    }
    QGraphicsView::wheelEvent(event);
}

// ---------------------------------------------------------------- tools & ink

void ReaderView::setTools(const ToolSettings &t)
{
    m_tools = t;
    const bool drawsWithMouse = t.mouseDraws && t.tool != ReaderTool::Lookup;
    viewport()->setCursor(t.tool == ReaderTool::Lookup ? Qt::PointingHandCursor
                          : drawsWithMouse             ? Qt::CrossCursor
                                                       : Qt::OpenHandCursor);
}

void ReaderView::beginStroke(const QPointF &pos, float pressure, bool erase)
{
    const auto hit = pageAndNorm(pos);
    if (!hit)
        return;
    const auto [page, norm] = *hit;
    m_drawing = true;
    m_strokePage = page;
    m_erasing = erase || m_tools.tool == ReaderTool::Eraser;
    if (m_erasing) {
        m_eraseBefore = m_pages[page]->strokes();
        eraseAt(page, norm);
        return;
    }
    const bool highlighter = m_tools.tool == ReaderTool::Highlighter;
    m_live = Stroke{QUuid::createUuid().toString(QUuid::WithoutBraces),
                    highlighter ? InkTool::Highlighter : InkTool::Pen,
                    highlighter ? m_tools.highlighterColor : m_tools.penColor,
                    highlighter ? m_tools.highlighterWidth : m_tools.penWidth,
                    {float(norm.x()), float(norm.y()), qBound(0.05f, pressure, 1.0f)}};
    m_pages[page]->setLiveStroke(m_live);
}

void ReaderView::extendStroke(const QPointF &pos, float pressure)
{
    if (!m_drawing || m_strokePage < 0)
        return;
    const QPointF scenePos = viewportTransform().inverted().map(pos);
    const QPointF norm = (scenePos - m_pages[m_strokePage]->sceneBoundingRect().topLeft()) / kPageWidth;
    if (m_erasing) {
        eraseAt(m_strokePage, norm);
        return;
    }
    m_live.points << float(norm.x()) << float(norm.y()) << qBound(0.05f, pressure, 1.0f);
    m_pages[m_strokePage]->setLiveStroke(m_live);
}

void ReaderView::endStroke()
{
    if (!m_drawing)
        return;
    m_drawing = false;
    PageItem *item = m_pages.value(m_strokePage);
    if (!item)
        return;
    if (m_erasing) {
        item->setEraserCursor(std::nullopt, 0);
        if (item->strokes().size() != m_eraseBefore.size())
            commit({m_strokePage, m_eraseBefore, item->strokes()});
    } else {
        item->setLiveStroke(std::nullopt);
        commit({m_strokePage, item->strokes(), item->strokes() + QVector<Stroke>{m_live}});
    }
    m_strokePage = -1;
}

void ReaderView::eraseAt(int page, const QPointF &norm)
{
    PageItem *item = m_pages[page];
    item->setEraserCursor(norm, kEraserRadius);
    QVector<Stroke> kept;
    bool removed = false;
    for (const Stroke &s : item->strokes()) {
        const float reach = kEraserRadius + s.width / 2;
        bool hit = false;
        for (int i = 0; i < s.pointCount() && !hit; ++i)
            hit = std::hypot(s.x(i) - norm.x(), s.y(i) - norm.y()) < reach;
        if (hit) removed = true;
        else kept.append(s);
    }
    if (removed)
        item->setStrokes(kept); // committed as one undo step when the eraser lifts
}

void ReaderView::commit(const InkEdit &edit)
{
    m_undo.append(edit);
    if (m_undo.size() > 200)
        m_undo.removeFirst();
    m_redo.clear();
    applyInk(edit.page, edit.after);
}

void ReaderView::applyInk(int page, const QVector<Stroke> &strokes)
{
    m_pages[page]->setStrokes(strokes);
    m_ctx.docs->saveInk(m_doc.docKey, page, strokes);
    emit undoStateChanged();
}

void ReaderView::undo()
{
    if (m_undo.isEmpty())
        return;
    const InkEdit e = m_undo.takeLast();
    m_redo.append(e);
    applyInk(e.page, e.before);
}

void ReaderView::redo()
{
    if (m_redo.isEmpty())
        return;
    const InkEdit e = m_redo.takeLast();
    m_undo.append(e);
    applyInk(e.page, e.after);
}

void ReaderView::clearPage()
{
    PageItem *item = m_pages.value(m_position.page);
    if (item && !item->strokes().isEmpty())
        commit({m_position.page, item->strokes(), {}});
}

void ReaderView::reloadInkIfUntouched()
{
    if (!m_pdf || !m_undo.isEmpty())
        return;
    const auto ink = m_ctx.docs->loadInk(m_doc.docKey);
    for (PageItem *item : std::as_const(m_pages)) {
        const QVector<Stroke> synced = ink.value(item->page());
        if (synced != item->strokes())
            item->setStrokes(synced);
    }
}

// ---------------------------------------------------------------- dictionary

std::optional<PickedWord> ReaderView::wordAt(const QPointF &viewportPos, bool highlight)
{
    const auto hit = pageAndNorm(viewportPos);
    if (!hit)
        return std::nullopt;
    return wordAtPage(hit->first, hit->second, highlight);
}

std::optional<PickedWord> ReaderView::wordAtPage(int page, const QPointF &norm, bool highlight)
{
    if (!m_pdf || page < 0 || page >= m_pages.size())
        return std::nullopt;
    const QSizeF pts = m_pagePoints[page];
    const QPointF pt = norm * pts.width();

    // QtPdf selects between two points; probe a few small spans around the click.
    QPdfSelection sel = m_pdf->getSelection(page, pt - QPointF(4, 0), pt + QPointF(4, 0));
    if (!sel.isValid()) sel = m_pdf->getSelection(page, pt, pt + QPointF(8, 0));
    if (!sel.isValid()) sel = m_pdf->getSelection(page, pt - QPointF(8, 0), pt);
    if (!sel.isValid())
        return std::nullopt;

    const QString text = m_pdf->getAllText(page).text();
    int idx = (sel.startIndex() + sel.endIndex()) / 2;
    if (idx < 0 || idx >= text.size())
        return std::nullopt;
    if (!isWordChar(text[idx])) {
        if (idx > 0 && isWordChar(text[idx - 1])) --idx;
        else if (idx + 1 < text.size() && isWordChar(text[idx + 1])) ++idx;
        else return std::nullopt;
    }
    int a = idx, b = idx + 1;
    while (a > 0 && isWordChar(text[a - 1])) --a;
    while (b < text.size() && isWordChar(text[b])) ++b;
    while (a < b && !text[a].isLetterOrNumber()) ++a;
    while (b > a && !text[b - 1].isLetterOrNumber()) --b;
    const QString word = text.mid(a, b - a);
    if (!std::any_of(word.begin(), word.end(), [](QChar c) { return c.isLetter(); }))
        return std::nullopt;

    int s = a, e = b;
    while (s > 0 && !isSentenceEnd(text[s - 1]) && a - s < 200) --s;
    while (e < text.size() && !isSentenceEnd(text[e]) && e - b < 200) ++e;
    if (e < text.size() && text[e] != '\n' && text[e] != '\r') ++e;

    if (highlight) {
        QPainterPath path;
        const qreal scale = kPageWidth / pts.width();
        for (const QPolygonF &poly : m_pdf->getSelectionAtIndex(page, a, b - a).bounds()) {
            QPolygonF scenePoly;
            for (const QPointF &p : poly)
                scenePoly << QPointF(p.x() * scale, m_pageTops[page] + p.y() * scale);
            path.addPolygon(scenePoly);
            path.closeSubpath();
        }
        if (!m_highlight) {
            m_highlight = scene()->addPath(path, Qt::NoPen, QColor(57, 73, 171, 70));
            m_highlight->setZValue(10);
        } else {
            m_highlight->setPath(path);
        }
    }
    return PickedWord{word, page, text.mid(s, e - s).simplified()};
}

void ReaderView::clearHighlight()
{
    if (m_highlight)
        m_highlight->setPath({});
}

void ReaderView::contextMenuEvent(QContextMenuEvent *event)
{
    const auto picked = wordAt(event->pos(), true);
    QMenu menu(this);
    if (picked) {
        menu.addAction(QStringLiteral("Define “%1”").arg(picked->word), this, [this, picked] { emit wordPicked(*picked); });
    } else {
        menu.addAction("No selectable text here")->setEnabled(false);
    }
    menu.exec(event->globalPos());
}

// ---------------------------------------------------------------- input

bool ReaderView::viewportEvent(QEvent *event)
{
    switch (event->type()) {
    case QEvent::TabletPress: {
        auto *t = static_cast<QTabletEvent *>(event);
        const bool eraserEnd = t->pointerType() == QPointingDevice::PointerType::Eraser;
        const bool barrel = t->buttons() & (Qt::RightButton | Qt::MiddleButton);
        if (m_tools.tool == ReaderTool::Lookup && !eraserEnd && !barrel)
            m_downPos = t->position();
        else
            beginStroke(t->position(), float(t->pressure()), eraserEnd || barrel);
        t->accept();
        return true;
    }
    case QEvent::TabletMove: {
        auto *t = static_cast<QTabletEvent *>(event);
        if (m_drawing)
            extendStroke(t->position(), float(t->pressure()));
        t->accept();
        return true;
    }
    case QEvent::TabletRelease: {
        auto *t = static_cast<QTabletEvent *>(event);
        if (m_drawing) {
            endStroke();
        } else if (m_tools.tool == ReaderTool::Lookup && QLineF(m_downPos, t->position()).length() < 8) {
            if (const auto w = wordAt(t->position(), true)) emit wordPicked(*w);
        }
        t->accept();
        return true;
    }
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel: {
        auto *te = static_cast<QTouchEvent *>(event);
        const auto &points = te->points();
        if (points.size() >= 2) {
            const QPointF a = points[0].position(), b = points[1].position();
            const qreal dist = QLineF(a, b).length();
            if (m_pinchStartDist <= 0) {
                m_pinchStartDist = dist;
                m_pinchStartZoom = m_zoom;
            } else if (dist > 0) {
                setZoom(m_pinchStartZoom * dist / m_pinchStartDist, (a + b) / 2);
            }
            m_lastPan = (a + b) / 2;
        } else if (points.size() == 1) {
            const QPointF p = points[0].position();
            if (te->type() == QEvent::TouchBegin || m_pinchStartDist > 0) {
                m_pinchStartDist = 0;
            } else {
                const QPointF d = p - m_lastPan;
                horizontalScrollBar()->setValue(horizontalScrollBar()->value() - qRound(d.x()));
                verticalScrollBar()->setValue(verticalScrollBar()->value() - qRound(d.y()));
            }
            m_lastPan = p;
        }
        if (te->type() == QEvent::TouchEnd || te->type() == QEvent::TouchCancel)
            m_pinchStartDist = 0;
        te->accept();
        return true;
    }
    case QEvent::NativeGesture: {
        // Touchpad pinch (Wayland/X11 via libinput).
        auto *g = static_cast<QNativeGestureEvent *>(event);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            setZoom(m_zoom * (1.0 + g->value()), g->position());
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QGraphicsView::viewportEvent(event);
}

void ReaderView::mousePressEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    if (event->button() == Qt::LeftButton && m_tools.tool == ReaderTool::Lookup) {
        m_downPos = pos;
    } else if (event->button() == Qt::LeftButton && m_tools.mouseDraws) {
        beginStroke(pos, 0.5f, false);
    } else if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
        m_panning = true;
        m_lastPan = pos;
        viewport()->setCursor(Qt::ClosedHandCursor);
    } else {
        QGraphicsView::mousePressEvent(event);
        return;
    }
    event->accept();
}

void ReaderView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_drawing) {
        extendStroke(event->position(), 0.5f);
    } else if (m_panning) {
        const QPointF d = event->position() - m_lastPan;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - qRound(d.x()));
        verticalScrollBar()->setValue(verticalScrollBar()->value() - qRound(d.y()));
        m_lastPan = event->position();
    } else {
        QGraphicsView::mouseMoveEvent(event);
    }
}

void ReaderView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_drawing) {
        endStroke();
    } else if (m_panning) {
        m_panning = false;
        setTools(m_tools); // restore the tool's cursor
    } else if (event->button() == Qt::LeftButton && m_tools.tool == ReaderTool::Lookup &&
               QLineF(m_downPos, event->position()).length() < 6) {
        if (const auto w = wordAt(event->position(), true)) emit wordPicked(*w);
    } else {
        QGraphicsView::mouseReleaseEvent(event);
    }
}

void ReaderView::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Double-click a word to define it, unless the mouse is busy drawing.
    if (!m_tools.mouseDraws || m_tools.tool == ReaderTool::Lookup) {
        if (const auto w = wordAt(event->position(), true)) {
            emit wordPicked(*w);
            return;
        }
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

// ---------------------------------------------------------------- keyboard navigation

void ReaderView::scrollLines(int dx, int dy)
{
    const int step = Prefs::scrollStep();
    horizontalScrollBar()->setValue(horizontalScrollBar()->value() + dx * step);
    verticalScrollBar()->setValue(verticalScrollBar()->value() + dy * step);
}

void ReaderView::scrollScreens(qreal fraction)
{
    // Keep a little overlap so the line you were reading stays on screen.
    const int amount = qRound(viewport()->height() * fraction * (std::abs(fraction) >= 1 ? 0.92 : 1.0));
    verticalScrollBar()->setValue(verticalScrollBar()->value() + amount);
}

void ReaderView::nextPage()
{
    if (m_position.page + 1 < m_pages.size())
        jumpTo(m_position.page + 1, 0);
}

void ReaderView::prevPage()
{
    // Like zathura: first go to the top of the current page, then to the previous one.
    if (m_position.offset > 0.02)
        jumpTo(m_position.page, 0);
    else if (m_position.page > 0)
        jumpTo(m_position.page - 1, 0);
}

void ReaderView::goToFirstPage() { goToPage(0, 0); }

void ReaderView::goToLastPage()
{
    if (m_pages.isEmpty())
        return;
    recordJump();
    jumpTo(m_pages.size() - 1, 0);
    if (m_rotation == 0)
        verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}

void ReaderView::goToPage(int page, double offset)
{
    if (m_pages.isEmpty())
        return;
    recordJump();
    jumpTo(qBound(0, page, int(m_pages.size()) - 1), qBound(0.0, offset, 1.0));
}

void ReaderView::recordJump()
{
    m_jumpBack.append(m_position);
    if (m_jumpBack.size() > 100)
        m_jumpBack.removeFirst();
    m_jumpForward.clear();
}

void ReaderView::jumpToPosition(const Position &p) { jumpTo(p.page, p.offset); }

void ReaderView::jumpBack()
{
    if (m_jumpBack.isEmpty()) {
        emit message("No earlier position");
        return;
    }
    m_jumpForward.append(m_position);
    jumpToPosition(m_jumpBack.takeLast());
}

void ReaderView::jumpForward()
{
    if (m_jumpForward.isEmpty())
        return;
    m_jumpBack.append(m_position);
    jumpToPosition(m_jumpForward.takeLast());
}

void ReaderView::beginSetMark()
{
    if (!m_pdf) return;
    cancelKeyMode();
    m_keyMode = KeyMode::SetMark;
    setFocus();
    emit message("Set mark: press a letter");
}

void ReaderView::beginJumpToMark()
{
    if (!m_pdf) return;
    cancelKeyMode();
    m_keyMode = KeyMode::JumpToMark;
    setFocus();
    emit message("Go to mark: press its letter");
}

void ReaderView::cancelKeyMode()
{
    for (const Hint &h : std::as_const(m_hints))
        delete h.item;
    m_hints.clear();
    m_hintTyped.clear();
    if (m_keyMode != KeyMode::None)
        emit message({});
    m_keyMode = KeyMode::None;
}

bool ReaderView::event(QEvent *event)
{
    // While waiting for a mark letter or a link hint, every key goes to keyPressEvent instead of
    // triggering shortcuts (so "f" then "j" picks hint "j" rather than scrolling).
    if (event->type() == QEvent::ShortcutOverride && m_keyMode != KeyMode::None) {
        event->accept();
        return true;
    }
    return QGraphicsView::event(event);
}

void ReaderView::keyPressEvent(QKeyEvent *event)
{
    if (m_keyMode == KeyMode::None) {
        QGraphicsView::keyPressEvent(event);
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        cancelKeyMode();
        return;
    }
    const QString text = event->text();
    if (m_keyMode == KeyMode::Hints) {
        if (event->key() == Qt::Key_Backspace) {
            m_hintTyped.chop(1);
            filterHints();
        } else if (!text.isEmpty() && text[0].isPrint()) {
            m_hintTyped += text.toLower();
            filterHints();
        }
        return;
    }
    if (text.isEmpty() || !text[0].isLetter())
        return; // ignore Shift etc. while waiting for the letter
    const QChar letter = text[0];
    const QString key = "marks/" + m_doc.docKey + "/" + letter;
    const KeyMode mode = m_keyMode;
    cancelKeyMode();
    if (mode == KeyMode::SetMark) {
        QSettings().setValue(key, QStringLiteral("%1:%2").arg(m_position.page).arg(m_position.offset));
        emit message(QStringLiteral("Mark '%1' set on page %2").arg(letter).arg(m_position.page + 1));
    } else {
        const QStringList parts = QSettings().value(key).toString().split(':');
        if (parts.size() != 2) {
            emit message(QStringLiteral("No mark '%1' in this document").arg(letter));
            return;
        }
        goToPage(parts[0].toInt(), parts[1].toDouble());
        emit message({});
    }
}

// ---------------------------------------------------------------- link hints

QRectF ReaderView::toScene(int page, const QRectF &r) const
{
    const qreal scale = kPageWidth / m_pagePoints[page].width();
    return {r.x() * scale, m_pageTops[page] + r.y() * scale, r.width() * scale, r.height() * scale};
}

void ReaderView::showLinkHints()
{
    if (!m_pdf)
        return;
    cancelKeyMode();
    const QRectF visible = mapToScene(viewport()->rect()).boundingRect();
    QList<std::pair<QPdfLink, QRectF>> links;
    QPdfLinkModel model;
    model.setDocument(m_pdf);
    for (int page = 0; page < m_pages.size(); ++page) {
        if (!m_pages[page]->sceneBoundingRect().intersects(visible))
            continue;
        model.setPage(page);
        for (int row = 0; row < model.rowCount({}); ++row) {
            const auto link = model.data(model.index(row), int(QPdfLinkModel::Role::Link)).value<QPdfLink>();
            if (link.rectangles().isEmpty())
                continue;
            const QRectF r = toScene(page, link.rectangles().first());
            if (visible.intersects(r))
                links.append({link, r});
        }
    }
    if (links.isEmpty()) {
        emit message("No links on screen");
        return;
    }

    // Labels like vimium/zathura: single letters while they suffice, then two letters.
    QString chars;
    for (const QChar c : Prefs::hintChars().toLower())
        if (c.isPrint() && !c.isSpace() && !chars.contains(c)) chars += c;
    if (chars.size() < 2)
        chars = "asdfghjkl";
    QStringList labels;
    if (links.size() <= chars.size()) {
        for (int i = 0; i < links.size(); ++i) labels << QString(chars[i]);
    } else {
        for (const QChar a : chars)
            for (const QChar b : chars) labels << QString(a) + b;
    }

    QFont font = this->font();
    font.setBold(true);
    font.setPointSizeF(font.pointSizeF() * 0.95);
    for (int i = 0; i < links.size() && i < labels.size(); ++i) {
        auto *box = new QGraphicsRectItem;
        auto *label = new QGraphicsSimpleTextItem(labels[i].toUpper(), box);
        label->setFont(font);
        label->setBrush(QColor(0x21, 0x21, 0x21));
        const QRectF tr = label->boundingRect();
        box->setRect(tr.adjusted(-3, -1, 3, 1));
        box->setBrush(QColor(0xFF, 0xD5, 0x4F));
        box->setPen(QPen(QColor(0xC7, 0x9A, 0x00), 1));
        box->setFlag(QGraphicsItem::ItemIgnoresTransformations); // readable at any zoom
        box->setPos(links[i].second.topLeft());
        box->setZValue(20);
        scene()->addItem(box);
        m_hints.append({labels[i], links[i].first, box});
    }
    m_keyMode = KeyMode::Hints;
    setFocus();
    emit message("Type a link's letters to follow it (Esc cancels)");
}

void ReaderView::filterHints()
{
    int visibleCount = 0;
    const Hint *exact = nullptr;
    for (const Hint &h : std::as_const(m_hints)) {
        const bool match = h.label.startsWith(m_hintTyped);
        h.item->setVisible(match);
        if (match) ++visibleCount;
        if (h.label == m_hintTyped) exact = &h;
    }
    if (exact) {
        const QPdfLink link = exact->link;
        followLink(link);
    } else if (visibleCount == 0) {
        cancelKeyMode();
        emit message("No link with those letters");
    }
}

void ReaderView::followLink(const QPdfLink &link)
{
    cancelKeyMode();
    const QUrl url = link.url();
    if (url.isValid() && !url.isEmpty()) {
        emit message("Opening " + url.toString());
        openUrlInBrowser(url);
        return;
    }
    if (link.page() >= 0 && link.page() < m_pages.size()) {
        const qreal height = m_pagePoints[link.page()].height();
        goToPage(link.page(), height > 0 ? link.location().y() / height : 0);
    }
}

// ---------------------------------------------------------------- search

void ReaderView::setSearchText(const QString &text, bool backward)
{
    if (!m_pdf)
        return;
    m_searchBackward = backward;
    if (text == m_searchText && m_search)
        return;
    m_searchText = text;
    if (text.isEmpty()) {
        clearSearch();
        return;
    }
    if (!m_search) {
        m_search = new QPdfSearchModel(this);
        m_search->setDocument(m_pdf);
        connect(m_search, &QPdfSearchModel::countChanged, &m_searchTimer, qOverload<>(&QTimer::start));
    }
    m_searchFrom = m_position;
    m_searchIndex = -1;
    m_searchPending = true;
    m_search->setSearchString(text);
    m_searchTimer.start();
}

void ReaderView::clearSearch()
{
    m_searchText.clear();
    m_searchPending = false;
    m_searchIndex = -1;
    if (m_search)
        m_search->setSearchString({});
    if (m_searchItem) m_searchItem->setPath({});
    if (m_currentMatchItem) m_currentMatchItem->setPath({});
    emit searchStatus(0, 0);
}

int ReaderView::searchIndexFrom(const Position &from, bool forward, bool wrap) const
{
    const int count = m_search ? m_search->count() : 0;
    auto after = [&](const QPdfLink &l) {
        const qreal h = m_pagePoints[l.page()].height();
        const qreal off = h > 0 ? l.location().y() / h : 0;
        return l.page() > from.page || (l.page() == from.page && off >= from.offset - 0.001);
    };
    if (forward) {
        for (int i = 0; i < count; ++i)
            if (after(m_search->resultAtIndex(i))) return i;
        return wrap && count > 0 ? 0 : -1;
    }
    for (int i = count - 1; i >= 0; --i)
        if (!after(m_search->resultAtIndex(i))) return i;
    return wrap && count > 0 ? count - 1 : -1;
}

void ReaderView::onSearchResults()
{
    if (!m_search)
        return;
    const int count = m_search->count();
    QPainterPath all;
    for (int i = 0; i < count && i < 5000; ++i) {
        const QPdfLink l = m_search->resultAtIndex(i);
        for (const QRectF &r : l.rectangles())
            all.addRect(toScene(l.page(), r));
    }
    if (!m_searchItem) {
        m_searchItem = scene()->addPath({}, Qt::NoPen, QColor(255, 213, 79, 120));
        m_searchItem->setZValue(9);
    }
    m_searchItem->setPath(all);

    if (m_searchPending && count > 0) {
        // Jump to the first match after where the search started, as soon as it's found.
        const int i = searchIndexFrom(m_searchFrom, !m_searchBackward, false);
        if (i >= 0) {
            m_searchPending = false;
            recordJump();
            showSearchResult(i);
            return;
        }
        m_searchSettle.start();
    }
    emit searchStatus(m_searchIndex + 1, count);
}

void ReaderView::showSearchResult(int index)
{
    if (!m_search || index < 0 || index >= m_search->count())
        return;
    m_searchIndex = index;
    const QPdfLink l = m_search->resultAtIndex(index);
    QPainterPath current;
    QRectF bounds;
    for (const QRectF &r : l.rectangles()) {
        const QRectF sr = toScene(l.page(), r);
        current.addRect(sr);
        bounds = bounds.united(sr);
    }
    if (!m_currentMatchItem) {
        m_currentMatchItem = scene()->addPath({}, QPen(QColor(230, 81, 0), 1.5), QColor(255, 111, 0, 110));
        m_currentMatchItem->setZValue(11);
    }
    m_currentMatchItem->setPath(current);
    ensureVisible(bounds, 80, 160);
    emit searchStatus(index + 1, m_search->count());
}

void ReaderView::findNext()
{
    if (!m_search || m_search->count() == 0)
        return;
    const bool forward = !m_searchBackward;
    const int count = m_search->count();
    const int i = m_searchIndex < 0 ? searchIndexFrom(m_position, forward, true)
                                    : (m_searchIndex + (forward ? 1 : -1) + count) % count;
    m_searchPending = false;
    recordJump();
    showSearchResult(i);
}

void ReaderView::findPrevious()
{
    m_searchBackward = !m_searchBackward;
    findNext();
    m_searchBackward = !m_searchBackward;
}
