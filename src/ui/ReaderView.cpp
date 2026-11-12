#include "ui/ReaderView.h"

#include "core/AppContext.h"
#include "core/DocumentStore.h"
#include "core/InkCodec.h"
#include "core/OpenUrl.h"
#include "ui/PageItem.h"
#include "ui/Prefs.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QGuiApplication>
#include <QUrl>
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QPointingDevice>
#include <QGraphicsPathItem>
#include <QMenu>
#include <QNativeGestureEvent>
#include <QPdfDocument>
#include <QPdfPageRenderer>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>
#include <QPdfLinkModel>
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

// Mouse diagnostics, off by default: QT_LOGGING_RULES="penreader.mouse=true" penreader
Q_LOGGING_CATEGORY(lcMouse, "penreader.mouse", QtWarningMsg)

namespace {

QString describe(const QMouseEvent *e)
{
    const QPointingDevice *d = e->pointingDevice();
    return QStringLiteral("button=%1 buttons=%2 pos=%3,%4 device='%5' type=%6 source=%7")
        .arg(int(e->button())).arg(int(e->buttons())).arg(e->position().x()).arg(e->position().y())
        .arg(d ? d->name() : QString()).arg(d ? int(d->type()) : -1).arg(int(e->source()));
}

/// With mouse diagnostics on: logs every mouse / touch / tablet event in the app and who gets it.
class MouseSpy : public QObject
{
public:
    using QObject::QObject;
    bool eventFilter(QObject *watched, QEvent *e) override
    {
        switch (e->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick: {
            auto *m = static_cast<QMouseEvent *>(e);
            qCDebug(lcMouse).noquote() << "  [app]" << e->type() << watched->metaObject()->className()
                                       << watched->objectName() << describe(m);
            break;
        }
        case QEvent::MouseMove: {
            auto *m = static_cast<QMouseEvent *>(e);
            if (m->buttons() != Qt::NoButton && ++m_moves % 10 == 1) // every 10th, while a button is held
                qCDebug(lcMouse).noquote() << "  [app] move" << watched->metaObject()->className() << describe(m);
            break;
        }
        case QEvent::TouchBegin:
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
        case QEvent::TabletPress:
        case QEvent::TabletRelease:
            qCDebug(lcMouse).noquote() << "  [app]" << e->type() << watched->metaObject()->className()
                                       << static_cast<QPointerEvent *>(e)->pointingDevice()->name();
            break;
        default:
            break;
        }
        return false;
    }

private:
    int m_moves = 0;
};

/// Search highlight that tints like a highlighter pen instead of covering the letters:
/// it darkens-blends on light pages (black text stays black) and lightens-blends on dark pages
/// (light text stays light), so the matched word keeps full contrast.
class HighlighterPathItem : public QGraphicsPathItem
{
public:
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override
    {
        painter->save();
        const bool darkPage = !Theme::pageColors().original && Theme::pageColors().background.lightnessF() < 0.5;
        painter->setCompositionMode(darkPage ? QPainter::CompositionMode_Screen : QPainter::CompositionMode_Multiply);
        painter->setPen(Qt::NoPen);
        QColor tint = brush().color();
        if (darkPage) tint.setAlphaF(tint.alphaF() * 0.45f); // a deep tint keeps light letters crisp
        painter->setBrush(tint);
        painter->drawPath(path());
        painter->restore();
        if (pen().style() != Qt::NoPen) { // outline (current match) drawn normally on top
            painter->setPen(pen());
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(path());
        }
        Q_UNUSED(option);
        Q_UNUSED(widget);
    }
};

} // namespace

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
    ++m_renderGeneration;
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
    setFocusPolicy(Qt::StrongFocus);
    viewport()->setMouseTracking(true);
    if (lcMouse().isDebugEnabled()) {
        static MouseSpy *spy = nullptr;
        if (!spy) qApp->installEventFilter(spy = new MouseSpy(qApp));
    }
    m_selTimer.setSingleShot(true);
    m_selTimer.setInterval(16);
    connect(&m_selTimer, &QTimer::timeout, this, [this] {
        if (m_gesture == Gesture::Select) extendSelection(m_pendingSelPos);
    });
    m_longPress.setSingleShot(true);
    m_longPress.setInterval(350);
    connect(&m_longPress, &QTimer::timeout, this, &ReaderView::startTextSelection);
    // Right button: we decide on release whether it was a drag (move the page) or a click (menu).
    viewport()->setContextMenuPolicy(Qt::PreventContextMenu);
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
        if (item->requestedWidth() != image.width() && item->renderedWidth() != 0)
            return;
        const Theme::PageColors colors = Theme::pageColors();
        if (colors.original) {
            item->setImage(image);
            return;
        }
        // Dark / sepia / custom reading colours: recolour on a worker thread so scrolling stays smooth.
        auto *watcher = new QFutureWatcher<QImage>(this);
        connect(watcher, &QFutureWatcher<QImage>::finished, this, [this, watcher, page, gen = m_renderGeneration] {
            watcher->deleteLater();
            if (gen != m_renderGeneration || page >= m_pages.size())
                return; // document closed or colours changed meanwhile
            const QImage out = watcher->result();
            PageItem *item = m_pages[page];
            if (item->requestedWidth() == out.width() || item->renderedWidth() == 0)
                item->setImage(out);
        });
        watcher->setFuture(QtConcurrent::run([image, colors] { return Theme::recolor(image, colors); }));
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
    ++m_renderGeneration;
    flushProgress();
    m_hints.clear();
    m_keyMode = KeyMode::None;
    delete m_search;
    m_search = nullptr;
    m_searchText.clear();
    m_searchIndex = -1;
    m_hitsByPage.clear();
    m_hits.clear();
    m_currentHit.reset();
    m_sel = {};
    m_selecting = false;
    m_selItem = nullptr;
    m_lineBoxes.clear();
    m_visual = false;
    m_caretItem = nullptr;
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
    updateCursor(); // pointer <-> hand as the page starts/stops fitting
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
    updateCursor();
}

bool ReaderView::isZoomedIn() const
{
    // Wider than the window (or zoomed past fit-width): there's something to drag around.
    return m_zoom > 1.001 || horizontalScrollBar()->maximum() > 0;
}

void ReaderView::updateCursor()
{
    if (m_panning)
        return;
    const ToolSettings &t = m_tools;
    if (t.tool == ReaderTool::Hand)
        viewport()->setCursor(Qt::ArrowCursor); // the hand shows only while dragging, the I-beam while selecting
    else if (t.tool == ReaderTool::Lookup)
        viewport()->setCursor(Qt::PointingHandCursor);
    else
        viewport()->setCursor(t.mouseDraws ? Qt::CrossCursor : Qt::OpenHandCursor);
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
    emit highlightsChanged();
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
        // Compare the content, not just stroke ids: a comment edited on another device keeps the ids.
        if (InkCodec::encode(synced) != InkCodec::encode(item->strokes())) {
            item->setStrokes(synced);
            emit highlightsChanged();
        }
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
    qCDebug(lcMouse) << "opening menu at" << event->pos() << "selection" << hasSelection();
    if (hasSelection()) {
        // Right-click on (or near) the selection: act on it.
        const auto hit = pageAndNorm(event->pos());
        bool inside = false;
        if (hit && hit->first == m_sel.page) {
            const QPointF pt = hit->second * m_pagePoints[m_sel.page].width();
            for (const QRectF &r : std::as_const(m_sel.rects))
                inside = inside || r.adjusted(-12, -12, 12, 12).contains(pt);
        }
        if (inside) {
            showSelectionMenu(event->globalPos());
            return;
        }
        clearSelection();
    }
    const auto picked = wordAt(event->pos(), true);
    QMenu menu(this);
    if (const auto h = highlightAt(event->pos())) {
        if (!h->note.isEmpty()) {
            const QString shortNote = h->note.size() > 60 ? h->note.left(58) + "…" : h->note;
            menu.addAction(Theme::icon("words"), "💬 " + shortNote)->setEnabled(false);
        }
        menu.addAction(h->note.isEmpty() ? "Add comment…" : "Edit comment…", this, [this, h] { emit editHighlightNoteRequested(*h); });
        menu.addAction("Remove this highlight", this, [this, h] { removeHighlight(h->page, h->key); });
        menu.addSeparator();
    }
    if (picked) {
        menu.addAction(QStringLiteral("Define “%1”").arg(picked->word), this, [this, picked] { emit wordPicked(*picked); });
        if (!picked->context.isEmpty())
            menu.addAction("Translate this sentence to தமிழ்", this, [this, picked] { emit translateRequested(picked->context); });
        menu.addAction(QStringLiteral("Find “%1” in this PDF").arg(picked->word), this, [this, picked] { emit findRequested(picked->word); });
        menu.addSeparator();
        menu.addAction("Tip: drag over text to select it")->setEnabled(false);
    } else {
        menu.addAction("No selectable text here")->setEnabled(false);
    }
    menu.exec(event->globalPos());
}

// ---------------------------------------------------------------- input

bool ReaderView::viewportEvent(QEvent *event)
{
    switch (event->type()) {
    case QEvent::ContextMenu:
        qCDebug(lcMouse) << "system context-menu request, reason" << static_cast<QContextMenuEvent *>(event)->reason();
        // On Linux the system asks for the menu when the right button goes *down*; we open it on
        // release instead (only if the mouse didn't drag), so ignore that request.
        if (static_cast<QContextMenuEvent *>(event)->reason() == QContextMenuEvent::Mouse)
            return true;
        break;
    case QEvent::TabletPress: {
        qCDebug(lcMouse) << "tablet press" << static_cast<QTabletEvent *>(event)->pointingDevice()->name()
                         << static_cast<QTabletEvent *>(event)->buttons();
        auto *t = static_cast<QTabletEvent *>(event);
        const bool eraserEnd = t->pointerType() == QPointingDevice::PointerType::Eraser;
        const bool barrel = t->buttons() & (Qt::RightButton | Qt::MiddleButton);
        if (m_tools.tool == ReaderTool::Lookup && !eraserEnd && !barrel) {
            m_downPos = t->position();
        } else if (m_tools.tool == ReaderTool::Hand && !eraserEnd && !barrel) {
            if (!isZoomedIn()) { t->accept(); return true; } // nothing to drag at fit width
            m_panning = true; // the pen drags the page too
            m_lastPan = t->position();
            viewport()->setCursor(Qt::ClosedHandCursor);
        } else
            beginStroke(t->position(), float(t->pressure()), eraserEnd || barrel);
        t->accept();
        return true;
    }
    case QEvent::TabletMove: {
        auto *t = static_cast<QTabletEvent *>(event);
        if (m_drawing) {
            extendStroke(t->position(), float(t->pressure()));
        } else if (m_panning) {
            const QPointF d = t->position() - m_lastPan;
            horizontalScrollBar()->setValue(horizontalScrollBar()->value() - qRound(d.x()));
            verticalScrollBar()->setValue(verticalScrollBar()->value() - qRound(d.y()));
            m_lastPan = t->position();
        }
        t->accept();
        return true;
    }
    case QEvent::TabletRelease: {
        auto *t = static_cast<QTabletEvent *>(event);
        if (m_drawing) {
            endStroke();
        } else if (m_panning) {
            m_panning = false;
            setTools(m_tools);
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

void ReaderView::startTextSelection()
{
    // Long press with the text button: switch to text selection from where the button went down.
    if (m_gesture != Gesture::Pending || !(QGuiApplication::mouseButtons() & m_gestureButton))
        return;
    clearSelection();
    if (!beginSelection(m_gestureStart))
        return; // not on a page: a later drag still moves the page
    m_gesture = Gesture::Select;
    viewport()->setCursor(Qt::IBeamCursor);
    extendSelection(viewport()->mapFromGlobal(QCursor::pos()));
    emit message("Selecting text — drag over it, then let go");
}

void ReaderView::endGesture()
{
    m_longPress.stop();
    if (QWidget::mouseGrabber() == viewport())
        viewport()->releaseMouse();
    m_gesture = Gesture::None;
    m_gestureButton = Qt::NoButton;
    m_selecting = false;
    if (m_panning) {
        m_panning = false;
        updateCursor();
    }
}

void ReaderView::mousePressEvent(QMouseEvent *event)
{
    qCDebug(lcMouse).noquote() << "press" << describe(event) << "tool" << int(m_tools.tool) << "right=" << MouseScheme::load().right;
    const QPointF pos = event->position();
    const Qt::MouseButton b = event->button();
    const bool pointer = m_tools.tool == ReaderTool::Hand;
    if (m_gestureButton != Qt::NoButton)
        endGesture(); // a second button while one is held: start over
    m_mouse = MouseScheme::load(); // Settings → Reading → Mouse
    if (m_visual) exitVisualMode(false);
    if (b == Qt::RightButton && m_mouse.menuOnPress()) {
        // Right click: the menu for the selected text (or the word under the mouse), straight away.
        event->accept();
        QContextMenuEvent menu(QContextMenuEvent::Mouse, pos.toPoint(), event->globalPosition().toPoint());
        contextMenuEvent(&menu);
        return;
    }
    if (b == Qt::MiddleButton || b == Qt::RightButton || (pointer && b == Qt::LeftButton)) {
        // Decided once the mouse moves (drag: move the page or select text) or is held still
        // (select text from here). A plain click acts on release: right opens the menu, left
        // clears the selection.
        m_gestureButton = b;
        m_gestureStart = pos;
        m_lastPan = pos;
        m_gesture = Gesture::Pending;
        // Claim the mouse until this button is let go, so every move and the release come here
        // even if the app wrongly believes another button is still held (seen on a real laptop:
        // a right press arriving as "left + right").
        viewport()->grabMouse();
        if (m_mouse.holdSelects(b)) {
            m_longPress.setInterval(qBound(150, m_mouse.holdMs, 2000));
            m_longPress.start();
        }
        event->accept();
        return;
    }
    if (b == Qt::LeftButton && m_tools.tool == ReaderTool::Lookup) {
        m_downPos = pos;
    } else if (b == Qt::LeftButton && m_tools.mouseDraws) {
        beginStroke(pos, 0.5f, false);
    } else if (b == Qt::LeftButton) { // "mouse draws" off: the mouse drags the page
        m_gestureButton = b;
        m_gesture = Gesture::Pan;
        m_lastPan = pos;
        m_panning = true;
        viewport()->setCursor(Qt::ClosedHandCursor);
    } else {
        QGraphicsView::mousePressEvent(event);
        return;
    }
    event->accept();
}

void ReaderView::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    if (m_gestureButton != Qt::NoButton)
        qCDebug(lcMouse).noquote() << "move" << describe(event) << "gesture" << int(m_gesture);
    // The button was let go without us seeing it (e.g. a menu grabbed the mouse): stop dragging.
    if (m_gestureButton != Qt::NoButton && !(event->buttons() & m_gestureButton))
        endGesture();
    // A little more slack for the right button so a slightly shaky right click still opens the menu.
    const qreal dragStart = m_gestureButton == Qt::RightButton ? 8 : 4;
    if (m_gesture == Gesture::Pending && QLineF(m_gestureStart, pos).length() > dragStart) {
        // The text button (right by default; always right with the pen & co.) selects when the drag
        // starts on text and moves the page when it starts anywhere else. The other button moves the page.
        m_longPress.stop();
        const MouseScheme::Action act = m_mouse.drag(m_gestureButton, overText(m_gestureStart));
        if (act == MouseScheme::Action::Select) {
            clearSelection();
            beginSelection(m_gestureStart);
            m_gesture = m_selecting ? Gesture::Select : Gesture::None;
        } else if (act == MouseScheme::Action::None) {
            m_gesture = Gesture::None; // this drag does nothing (button still tracked until release)
        } else {
            m_gesture = Gesture::Pan;
            m_panning = true;
            viewport()->setCursor(Qt::ClosedHandCursor);
        }
    }
    if (m_gesture == Gesture::Select) {
        // Selecting asks the PDF engine for the text between two points, which takes a few ms:
        // do it at most once per frame with the latest position, so the mouse never lags behind.
        m_pendingSelPos = pos;
        if (!m_selTimer.isActive()) m_selTimer.start();
        return;
    }
    if (m_gesture == Gesture::Pan) {
        const QPointF d = pos - m_lastPan;
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - qRound(d.x()));
        verticalScrollBar()->setValue(verticalScrollBar()->value() - qRound(d.y()));
        m_lastPan = pos;
        return;
    }
    if (m_drawing) {
        if (!(event->buttons() & Qt::LeftButton)) endStroke(); // release missed
        else extendStroke(pos, 0.5f);
    } else {
        QGraphicsView::mouseMoveEvent(event);
    }
}

void ReaderView::mouseReleaseEvent(QMouseEvent *event)
{
    qCDebug(lcMouse).noquote() << "release" << describe(event) << "gesture" << int(m_gesture) << "held" << int(m_gestureButton);
    if (event->button() == m_gestureButton && m_gestureButton != Qt::NoButton) {
        const Gesture g = m_gesture;
        if (g == Gesture::Select) {
            m_selTimer.stop();
            extendSelection(event->position()); // exactly where the button was let go
        }
        endGesture();
        updateCursor();
        if (g == Gesture::Select) {
            if (m_sel.text.trimmed().isEmpty())
                clearSelection();
            else
                emit message(QStringLiteral("Selected %1 characters — right-click for highlight, translate, copy…")
                                 .arg(m_sel.text.size()));
        } else if (g == Gesture::Pending) {
            if (event->button() == Qt::RightButton) {
                QContextMenuEvent menu(QContextMenuEvent::Mouse, event->position().toPoint(),
                                       event->globalPosition().toPoint());
                contextMenuEvent(&menu);
            } else if (event->button() == Qt::LeftButton) {
                clearSelection(); // a plain click elsewhere
                if (const auto h = highlightAt(event->position()))
                    emit highlightClicked(*h, event->globalPosition().toPoint()); // show its comment
            }
        }
        return;
    }
    if (m_drawing) {
        endStroke();
    } else if (event->button() == Qt::LeftButton && m_tools.tool == ReaderTool::Lookup &&
               QLineF(m_downPos, event->position()).length() < 6) {
        if (const auto w = wordAt(event->position(), true)) emit wordPicked(*w);
    } else {
        QGraphicsView::mouseReleaseEvent(event);
    }
}

void ReaderView::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Double-click a word to define it, unless the mouse is busy drawing (or that's turned off).
    if (MouseScheme::load().doubleClick == "define" && event->button() == Qt::LeftButton &&
        (!m_tools.mouseDraws || m_tools.tool == ReaderTool::Lookup || m_tools.tool == ReaderTool::Hand)) {
        if (const auto w = wordAt(event->position(), true)) {
            emit wordPicked(*w);
            return;
        }
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

// ---------------------------------------------------------------- visual mode (vim)

namespace {
bool isLineBreak(QChar c) { return c == u'\r' || c == u'\n'; }
bool isWord(QChar c) { return c.isLetterOrNumber() || c == u'_' || c == u'\'' || c == u'’'; }
} // namespace

int ReaderView::vLineStart(int i) const
{
    while (i > 0 && !isLineBreak(m_vText[i - 1])) --i;
    return i;
}

int ReaderView::vLineEnd(int i) const // last character of the line
{
    const int n = int(m_vText.size());
    while (i + 1 < n && !isLineBreak(m_vText[i + 1])) ++i;
    return i;
}

void ReaderView::enterVisualMode(bool lineWise)
{
    if (!m_pdf)
        return;
    int page = m_position.page, start = -1, end = -1;
    if (hasSelection() && m_sel.start >= 0) {
        // Keep a mouse selection and continue it with the keyboard.
        page = m_sel.page;
        start = m_sel.start;
        end = qMax(m_sel.start, m_sel.end - 1);
    } else {
        // Start at the current search match, or the first text on screen.
        auto charAt = [this](int pg, const QPointF &pt) {
            QPdfSelection s = m_pdf->getSelection(pg, pt - QPointF(3, 0), pt + QPointF(3, 0));
            if (!s.isValid()) s = m_pdf->getSelection(pg, pt, pt + QPointF(12, 0));
            return s.isValid() ? s.startIndex() : -1;
        };
        if (m_searchIndex >= 0 && m_searchIndex < m_hits.size()) {
            const TextHit &hit = m_hits[m_searchIndex];
            page = hit.page;
            start = charAt(page, hit.rects.first().topLeft() + QPointF(1, hit.rects.first().height() / 2));
        }
        if (start < 0) {
            const QPointF top = pagePoint(page, QPointF(viewport()->width() * 0.08, 8));
            const QSizeF pts = m_pagePoints[page];
            for (qreal y = top.y(); y < pts.height() && start < 0; y += 6)
                for (qreal x = 20; x < pts.width() * 0.6 && start < 0; x += 30) start = charAt(page, QPointF(x, y));
        }
        end = start;
    }
    if (start < 0) {
        emit message("No text on this page to select (it may be a scanned image)");
        return;
    }
    clearSelection();
    m_vText = m_pdf->getAllText(page).text();
    m_visual = true;
    m_visualLine = lineWise;
    m_sel.page = page;
    m_vAnchor = qBound(0, start, int(m_vText.size()) - 1);
    m_vCaret = qBound(0, end, int(m_vText.size()) - 1);
    updateVisualSelection();
}

void ReaderView::exitVisualMode(bool keepSelection)
{
    if (!m_visual)
        return;
    m_visual = false;
    if (m_caretItem) m_caretItem->setRect({});
    if (!keepSelection) clearSelection();
    emit message({});
}

void ReaderView::updateVisualSelection()
{
    int a = qMin(m_vAnchor, m_vCaret), b = qMax(m_vAnchor, m_vCaret);
    if (m_visualLine) {
        a = vLineStart(a);
        b = vLineEnd(b);
    }
    showSelection(m_pdf->getSelectionAtIndex(m_sel.page, a, b - a + 1));
    // The caret: a bar at the edge of the character it's on.
    const QPdfSelection c = m_pdf->getSelectionAtIndex(m_sel.page, m_vCaret, 1);
    QRectF box = c.bounds().isEmpty() ? QRectF() : c.bounds().first().boundingRect();
    if (!box.isNull()) {
        const QRectF charBox = toScene(m_sel.page, box);
        const qreal x = m_vCaret >= m_vAnchor ? charBox.right() : charBox.left();
        const QRectF bar(x - 1.2, charBox.top() - 1, 2.4, charBox.height() + 2);
        if (!m_caretItem) {
            m_caretItem = scene()->addRect(bar, Qt::NoPen, Theme::tokens().accent);
            m_caretItem->setZValue(14);
        }
        m_caretItem->setBrush(Theme::tokens().accent);
        m_caretItem->setRect(bar);
        ensureVisible(bar, 60, 120);
    }
    emit message(QStringLiteral("-- %1 --   h j k l w b e 0 $ ( ) move · o other end · y copy · Enter / 1–5 highlight · "
                                "u underline · c comment · t translate (T sentence) · K define · / find · s smart search · Esc")
                     .arg(m_visualLine ? "VISUAL LINE" : "VISUAL"));
}

bool ReaderView::visualKey(QKeyEvent *e)
{
    const int n = int(m_vText.size());
    if (n == 0) { exitVisualMode(false); return true; }
    const QString t = e->text();
    const int key = e->key();
    int &c = m_vCaret;
    auto skipBreaksForward = [&](int i) { while (i < n - 1 && isLineBreak(m_vText[i])) ++i; return i; };
    auto skipBreaksBack = [&](int i) { while (i > 0 && isLineBreak(m_vText[i])) --i; return i; };
    auto moved = [&] { updateVisualSelection(); return true; };

    // ---- motions
    if (t == "l" || key == Qt::Key_Right) { if (c < n - 1) c = skipBreaksForward(c + 1); return moved(); }
    if (t == "h" || key == Qt::Key_Left) { if (c > 0) c = skipBreaksBack(c - 1); return moved(); }
    if (t == "j" || key == Qt::Key_Down || t == "k" || key == Qt::Key_Up) {
        const bool down = t == "j" || key == Qt::Key_Down;
        const int col = c - vLineStart(c);
        int target;
        if (down) {
            int next = vLineEnd(c) + 1;
            while (next < n && isLineBreak(m_vText[next])) ++next;
            if (next >= n) return moved();
            target = next;
        } else {
            const int ls = vLineStart(c);
            if (ls == 0) return moved();
            int prevEnd = ls - 1;
            while (prevEnd > 0 && isLineBreak(m_vText[prevEnd])) --prevEnd;
            target = vLineStart(prevEnd);
        }
        c = qMin(target + col, vLineEnd(target));
        return moved();
    }
    if (t == "w") { // next word start
        int i = c;
        while (i < n - 1 && isWord(m_vText[i])) ++i;
        while (i < n - 1 && !isWord(m_vText[i])) ++i;
        c = i;
        return moved();
    }
    if (t == "e") { // end of word
        int i = qMin(c + 1, n - 1);
        while (i < n - 1 && !isWord(m_vText[i])) ++i;
        while (i < n - 1 && isWord(m_vText[i + 1])) ++i;
        c = i;
        return moved();
    }
    if (t == "b") { // previous word start
        int i = qMax(c - 1, 0);
        while (i > 0 && !isWord(m_vText[i])) --i;
        while (i > 0 && isWord(m_vText[i - 1])) --i;
        c = i;
        return moved();
    }
    if (t == "0" || t == "^" || key == Qt::Key_Home) { c = vLineStart(c); return moved(); }
    if (t == "$" || key == Qt::Key_End) { c = vLineEnd(c); return moved(); }
    if (t == ")" || t == "(") { // sentence forward / back
        auto ends = [&](int i) { return m_vText[i] == u'.' || m_vText[i] == u'?' || m_vText[i] == u'!'; };
        int i = c;
        if (t == ")") {
            while (i < n - 1 && !ends(i)) ++i;
            if (i < n - 1) ++i;
            while (i < n - 1 && (m_vText[i].isSpace())) ++i;
        } else {
            i = qMax(0, i - 1);
            while (i > 0 && m_vText[i].isSpace()) --i;
            if (i > 0 && ends(i)) --i;
            while (i > 0 && !ends(i - 1)) --i;
            while (i < n - 1 && m_vText[i].isSpace()) ++i;
        }
        c = i;
        return moved();
    }
    if (t == "o") { std::swap(m_vAnchor, m_vCaret); return moved(); }
    if (t == "v") { if (m_visualLine) { m_visualLine = false; return moved(); } exitVisualMode(false); return true; }
    if (t == "V") { if (!m_visualLine) { m_visualLine = true; return moved(); } exitVisualMode(false); return true; }
    if (key == Qt::Key_Escape) { exitVisualMode(false); return true; }

    // ---- actions on the selection
    auto act = [&](auto f) { exitVisualMode(true); f(); return true; };
    if (t == "y" || e->matches(QKeySequence::Copy)) return act([this] { copySelection(); emit message("Copied"); clearSelection(); });
    if (t == "Y") return act([this] { QGuiApplication::clipboard()->setText(selectionSentence()); emit message("Sentence copied"); clearSelection(); });
    static const quint32 colours[] = {0x66FFEB3B, 0x6676FF03, 0x6640C4FF, 0x66FF4081, 0x66FFAB40};
    if (key == Qt::Key_Return || key == Qt::Key_Enter) return act([this] { highlightSelection(colours[0]); });
    if (t.size() == 1 && t[0] >= u'1' && t[0] <= u'5') {
        const quint32 colour = colours[t[0].unicode() - u'1'];
        return act([this, colour] { highlightSelection(colour); });
    }
    if (t == "u") return act([this] { underlineSelection(); });
    if (t == "c") return act([this] { commentSelection(); });
    if (t == "t") return act([this] { translateSelection(false); });
    if (t == "T") return act([this] { translateSelection(true); });
    if (t == "K") return act([this] { defineSelection(); });
    if (t == "/") return act([this] { findSelection(); });
    if (t == "s") return act([this] { smartSearchSelection(); });
    return true; // swallow other keys while in visual mode
}

// ---------------------------------------------------------------- selection actions (menu & keys)

bool ReaderView::needSelection()
{
    if (m_visual) exitVisualMode(true); // a shortcut used during visual mode acts on its selection
    if (hasSelection())
        return true;
    emit message("Select some text first: drag over it, or press v for visual mode");
    return false;
}

void ReaderView::translateSelection(bool sentence)
{
    if (!needSelection()) return;
    emit translateRequested(sentence ? selectionSentence() : m_sel.text.simplified());
}

void ReaderView::defineSelection()
{
    if (!needSelection()) return;
    const QString word = m_sel.text.simplified().section(' ', 0, 2);
    emit wordPicked(PickedWord{word, m_sel.page, selectionSentence()});
}

void ReaderView::findSelection()
{
    if (!needSelection()) return;
    emit findRequested(m_sel.text.simplified());
}

void ReaderView::smartSearchSelection()
{
    if (!needSelection()) return;
    emit smartSearchRequested(m_sel.text.simplified());
}

void ReaderView::commentSelection()
{
    if (!needSelection()) return;
    const int page = m_sel.page;
    const QString key = highlightSelection(0x66FFEB3B);
    for (const Highlight &h : highlights())
        if (h.page == page && h.key == key) { emit editHighlightNoteRequested(h); break; }
}

void ReaderView::gotoHighlight(bool next)
{
    const QVector<Highlight> list = highlights();
    if (list.isEmpty()) {
        emit message("No highlights in this PDF yet");
        return;
    }
    // Position of the top of the screen, as page + fraction of the page width (like ink coordinates).
    const qreal here = m_position.page + m_position.offset * 0.999;
    auto where = [this](const Highlight &h) {
        const qreal ph = m_pages[h.page]->height() / kPageWidth; // page height in page widths
        return h.page + qBound(0.0, (h.box.top() - 0.02) / ph, 0.999);
    };
    int idx = -1;
    // Pressed again without moving: step from the highlight we're on (several can share a line).
    if (!m_hlNavKey.isEmpty() && m_hlNavPos.page == m_position.page && qAbs(m_hlNavPos.offset - m_position.offset) < 0.002) {
        for (int i = 0; i < list.size(); ++i)
            if (list[i].key == m_hlNavKey) { idx = (i + (next ? 1 : -1) + int(list.size())) % int(list.size()); break; }
    }
    if (idx >= 0) {
        // found by stepping
    } else if (next) {
        for (int i = 0; i < list.size(); ++i)
            if (where(list[i]) > here + 0.002) { idx = i; break; }
        if (idx < 0) idx = 0;
    } else {
        for (int i = int(list.size()) - 1; i >= 0; --i)
            if (where(list[i]) < here - 0.002) { idx = i; break; }
        if (idx < 0) idx = int(list.size()) - 1;
    }
    const Highlight &h = list[idx];
    revealHighlight(h.page, h.key);
    m_hlNavKey = h.key;
    m_hlNavPos = m_position;
    QString text = h.text.size() > 60 ? h.text.left(58) + "…" : h.text;
    emit message(QStringLiteral("Highlight %1 of %2 · page %3 · “%4”%5").arg(idx + 1).arg(list.size()).arg(h.page + 1).arg(text,
                 h.note.isEmpty() ? QString() : QStringLiteral("  💬 ") + h.note.left(60)));
}

// ---------------------------------------------------------------- highlights & comments

QString ReaderView::textUnder(int page, const Stroke &s) const
{
    // A hand-drawn highlight: the text between its first and last point.
    if (!m_pdf || s.pointCount() == 0)
        return {};
    const QString cacheKey = s.id + QString::number(s.pointCount());
    if (const auto it = m_highlightText.find(cacheKey); it != m_highlightText.end())
        return *it;
    const qreal w = m_pagePoints[page].width();
    const QPdfSelection sel = m_pdf->getSelection(page, QPointF(s.x(0), s.y(0)) * w,
                                                  QPointF(s.x(s.pointCount() - 1), s.y(s.pointCount() - 1)) * w);
    const QString text = sel.isValid() ? sel.text().simplified() : QString();
    m_highlightText.insert(cacheKey, text);
    return text;
}

QVector<Highlight> ReaderView::highlights() const
{
    QVector<Highlight> out;
    for (int page = 0; page < m_pages.size(); ++page) {
        QHash<QString, int> index; // key -> position in out
        for (const Stroke &s : m_pages[page]->strokes()) {
            if (s.tool != InkTool::Highlighter || s.pointCount() == 0)
                continue;
            const QString key = s.group.isEmpty() ? s.id : s.group;
            int i = index.value(key, -1);
            if (i < 0) {
                i = int(out.size());
                index.insert(key, i);
                Highlight h;
                h.page = page;
                h.key = key;
                h.color = s.color;
                out.append(h);
            }
            Highlight &h = out[i];
            h.strokes.append(s);
            if (h.text.isEmpty() && !s.text.isEmpty()) h.text = s.text;
            if (h.note.isEmpty() && !s.note.isEmpty()) h.note = s.note;
            float minX = s.x(0), maxX = minX, minY = s.y(0), maxY = minY;
            for (int p = 1; p < s.pointCount(); ++p) {
                minX = qMin(minX, s.x(p)); maxX = qMax(maxX, s.x(p));
                minY = qMin(minY, s.y(p)); maxY = qMax(maxY, s.y(p));
            }
            const qreal half = s.width / 2;
            h.box = h.box.united(QRectF(QPointF(minX - half, minY - half), QPointF(maxX + half, maxY + half)));
        }
    }
    for (Highlight &h : out) {
        if (h.text.isEmpty()) {
            QStringList parts;
            for (const Stroke &s : std::as_const(h.strokes)) parts << textUnder(h.page, s);
            h.text = parts.join(' ').simplified();
        }
    }
    // Reading order: page, then top to bottom.
    std::stable_sort(out.begin(), out.end(), [](const Highlight &a, const Highlight &b) {
        return a.page != b.page ? a.page < b.page : a.box.top() < b.box.top();
    });
    return out;
}

std::optional<Highlight> ReaderView::highlightAt(const QPointF &viewportPos) const
{
    const auto hit = pageAndNorm(viewportPos);
    if (!hit)
        return std::nullopt;
    const auto [page, norm] = *hit;
    for (const Stroke &s : m_pages[page]->strokes()) {
        if (s.tool != InkTool::Highlighter)
            continue;
        const qreal half = s.width / 2 + 0.002;
        for (int i = 0; i < s.pointCount(); ++i) {
            const QPointF a(s.x(i), s.y(i));
            const QPointF b = i + 1 < s.pointCount() ? QPointF(s.x(i + 1), s.y(i + 1)) : a;
            // Distance from the point to the segment a-b.
            const QPointF ab = b - a, ap = norm - a;
            const qreal len2 = QPointF::dotProduct(ab, ab);
            const qreal t = len2 > 0 ? qBound(0.0, QPointF::dotProduct(ap, ab) / len2, 1.0) : 0.0;
            if (QLineF(norm, a + ab * t).length() <= half) {
                const QString key = s.group.isEmpty() ? s.id : s.group;
                for (const Highlight &h : highlights())
                    if (h.page == page && h.key == key) return h;
            }
        }
    }
    return std::nullopt;
}

void ReaderView::setHighlightNote(int page, const QString &key, const QString &note)
{
    if (page < 0 || page >= m_pages.size())
        return;
    const QVector<Stroke> before = m_pages[page]->strokes();
    QVector<Stroke> after = before;
    for (Stroke &s : after) {
        if ((s.group.isEmpty() ? s.id : s.group) == key) {
            s.note = note.trimmed(); // stored on the group's first stroke
            break;
        }
    }
    commit({page, before, after});
}

void ReaderView::removeHighlight(int page, const QString &key)
{
    if (page < 0 || page >= m_pages.size())
        return;
    const QVector<Stroke> before = m_pages[page]->strokes();
    QVector<Stroke> after;
    for (const Stroke &s : before)
        if (!(s.tool == InkTool::Highlighter && (s.group.isEmpty() ? s.id : s.group) == key)) after.append(s);
    if (after.size() != before.size()) {
        commit({page, before, after});
        emit message("Highlight removed — Undo (Ctrl+Z) brings it back");
    }
}

void ReaderView::revealHighlight(int page, const QString &key)
{
    for (const Highlight &h : highlights()) {
        if (h.page != page || h.key != key)
            continue;
        recordJump();
        const qreal ph = m_pages[page]->height();
        goToPage(page, ph > 0 ? qMax(0.0, (h.box.top() * kPageWidth - 80) / ph) : 0);
        // Flash an outline round it for a moment.
        QPainterPath path;
        path.addRoundedRect(QRectF(h.box.left() * kPageWidth, m_pageTops[page] + h.box.top() * kPageWidth,
                                   h.box.width() * kPageWidth, h.box.height() * kPageWidth).adjusted(-4, -4, 4, 4), 4, 4);
        auto *flash = scene()->addPath(path, QPen(Theme::tokens().accent, 2.5), Qt::NoBrush);
        flash->setZValue(13);
        QTimer::singleShot(1600, this, [this, flash] { if (m_pdf && scene()->items().contains(flash)) delete flash; });
        return;
    }
}

// ---------------------------------------------------------------- text selection

QPoint ReaderView::viewportPointFor(int page, const QPointF &pdfPoint) const
{
    const qreal scale = kPageWidth / m_pagePoints[page].width();
    return mapFromScene(QPointF(pdfPoint.x() * scale, m_pageTops[page] + pdfPoint.y() * scale));
}

QPointF ReaderView::pagePoint(int page, const QPointF &viewportPos) const
{
    const QPointF scenePos = viewportTransform().inverted().map(viewportPos);
    const QSizeF pts = m_pagePoints[page];
    const qreal scale = pts.width() / kPageWidth;
    const QPointF p((scenePos.x()) * scale, (scenePos.y() - m_pageTops[page]) * scale);
    return {qBound(0.0, p.x(), pts.width()), qBound(0.0, p.y(), pts.height())};
}

bool ReaderView::overText(const QPointF &viewportPos)
{
    const auto hit = pageAndNorm(viewportPos);
    if (!hit || !m_pdf)
        return false;
    const int page = hit->first;
    auto it = m_lineBoxes.find(page);
    if (it == m_lineBoxes.end()) {
        QList<QRectF> boxes;
        for (const QPolygonF &poly : m_pdf->getAllText(page).bounds())
            boxes.append(poly.boundingRect().adjusted(-1, -1, 1, 1));
        it = m_lineBoxes.insert(page, boxes);
    }
    const QPointF pt = hit->second * m_pagePoints[page].width();
    return std::any_of(it->begin(), it->end(), [&](const QRectF &r) { return r.contains(pt); });
}

bool ReaderView::beginSelection(const QPointF &viewportPos)
{
    const auto hit = pageAndNorm(viewportPos); // anywhere on a page: selects from the nearest text
    if (!hit)
        return false;
    m_sel = {};
    m_sel.page = hit->first;
    m_sel.from = hit->second * m_pagePoints[hit->first].width();
    m_selecting = true;
    m_downPos = viewportPos;
    viewport()->setCursor(Qt::IBeamCursor);
    return true;
}

void ReaderView::extendSelection(const QPointF &viewportPos)
{
    if (m_sel.page < 0 || !m_pdf)
        return;
    const QPointF to = pagePoint(m_sel.page, viewportPos); // stays on the page it started on
    showSelection(m_pdf->getSelection(m_sel.page, m_sel.from, to));
    // Auto-scroll when dragging past the top or bottom edge.
    const int edge = 24;
    if (viewportPos.y() < edge) verticalScrollBar()->setValue(verticalScrollBar()->value() - 20);
    else if (viewportPos.y() > viewport()->height() - edge) verticalScrollBar()->setValue(verticalScrollBar()->value() + 20);
}

void ReaderView::showSelection(const QPdfSelection &sel)
{
    m_sel.text = sel.isValid() ? sel.text() : QString();
    m_sel.start = sel.startIndex();
    m_sel.end = sel.endIndex();
    m_sel.rects.clear();
    QPainterPath path;
    for (const QPolygonF &poly : sel.bounds()) {
        m_sel.rects.append(poly.boundingRect());
        path.addRect(toScene(m_sel.page, poly.boundingRect()));
    }
    if (!m_selItem) {
        m_selItem = new HighlighterPathItem;
        m_selItem->setPen(Qt::NoPen);
        m_selItem->setBrush(QColor(66, 133, 244, 150));
        m_selItem->setZValue(12);
        scene()->addItem(m_selItem);
    }
    m_selItem->setPath(path);
}

void ReaderView::clearSelection()
{
    const bool had = hasSelection() || m_selecting;
    m_sel = {};
    m_selecting = false;
    if (m_selItem) m_selItem->setPath({});
    if (had) emit message({});
}

void ReaderView::copySelection()
{
    if (hasSelection())
        QGuiApplication::clipboard()->setText(m_sel.text.simplified());
}

QString ReaderView::selectionSentence() const
{
    if (!hasSelection() || !m_pdf)
        return {};
    const QString text = m_pdf->getAllText(m_sel.page).text();
    int s = qBound(0, m_sel.start, int(text.size())), e = qBound(s, m_sel.end, int(text.size()));
    auto stop = [](QChar c) { return c == '.' || c == '?' || c == '!'; };
    while (s > 0 && !stop(text[s - 1]) && m_sel.start - s < 600) --s;
    while (e < text.size() && !stop(text[e]) && e - m_sel.end < 600) ++e;
    if (e < text.size()) ++e; // keep the full stop
    return text.mid(s, e - s).simplified();
}

void ReaderView::addStrokes(int page, const QVector<Stroke> &strokes)
{
    if (strokes.isEmpty())
        return;
    const QVector<Stroke> before = m_pages[page]->strokes();
    commit({page, before, before + strokes});
    emit undoStateChanged();
}

QString ReaderView::highlightSelection(quint32 argb)
{
    if (m_visual) exitVisualMode(true);
    if (!hasSelection())
        return {};
    const qreal w = m_pagePoints[m_sel.page].width();
    const QString group = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVector<Stroke> strokes;
    for (const QRectF &r : std::as_const(m_sel.rects)) {
        // A highlighter stroke along the line, exactly as tall as the text (square ends).
        const qreal h = r.height();
        const qreal x0 = r.left() + h / 2, x1 = qMax(x0, r.right() - h / 2);
        Stroke s{QUuid::createUuid().toString(QUuid::WithoutBraces), InkTool::Highlighter, argb, float(h / w), {}};
        s.points = {float(x0 / w), float(r.center().y() / w), 0.5f, float(x1 / w), float(r.center().y() / w), 0.5f};
        s.group = group;
        if (strokes.isEmpty()) s.text = m_sel.text.simplified(); // the text and any comment live on the first stroke
        strokes.append(s);
    }
    addStrokes(m_sel.page, strokes);
    clearSelection();
    return group;
}

void ReaderView::underlineSelection()
{
    if (m_visual) exitVisualMode(true);
    if (!hasSelection())
        return;
    const qreal w = m_pagePoints[m_sel.page].width();
    QVector<Stroke> strokes;
    for (const QRectF &r : std::as_const(m_sel.rects)) {
        const float y = float((r.bottom() + 0.8) / w);
        Stroke s{QUuid::createUuid().toString(QUuid::WithoutBraces), InkTool::Pen, m_tools.penColor, 0.0022f, {}};
        s.points = {float(r.left() / w), y, 0.6f, float(r.right() / w), y, 0.6f};
        strokes.append(s);
    }
    addStrokes(m_sel.page, strokes);
    clearSelection();
}

QVector<Stroke> ReaderView::highlightsInSelection() const
{
    QVector<Stroke> found;
    if (!hasSelection())
        return found;
    const qreal w = m_pagePoints[m_sel.page].width();
    for (const Stroke &s : m_pages[m_sel.page]->strokes()) {
        if (s.tool != InkTool::Highlighter)
            continue;
        for (int i = 0; i < s.pointCount(); ++i) {
            const QPointF p(s.x(i) * w, s.y(i) * w);
            if (std::any_of(m_sel.rects.begin(), m_sel.rects.end(), [&](const QRectF &r) { return r.adjusted(-4, -4, 4, 4).contains(p); })) {
                found.append(s);
                break;
            }
        }
    }
    return found;
}

void ReaderView::showSelectionMenu(const QPoint &globalPos)
{
    const QString text = m_sel.text.simplified();
    const QString shortText = text.size() > 32 ? text.left(30) + "…" : text;
    const int words = int(text.split(' ', Qt::SkipEmptyParts).size());
    QMenu menu(this);

    menu.addAction(Theme::icon("highlighter"), "Highlight", this, [this] { highlightSelection(0x66FFEB3B); });
    QMenu *colours = menu.addMenu("Highlight in colour");
    const QList<std::pair<QString, quint32>> palette = {
        {"Yellow", 0x66FFEB3B}, {"Green", 0x6676FF03}, {"Blue", 0x6640C4FF}, {"Pink", 0x66FF4081}, {"Orange", 0x66FFAB40}};
    for (const auto &[name, argb] : palette) {
        QPixmap swatch(14, 14);
        swatch.fill(QColor::fromRgba(argb | 0xFF000000));
        colours->addAction(QIcon(swatch), name, this, [this, argb] { highlightSelection(argb); });
    }
    menu.addAction("Underline", this, &ReaderView::underlineSelection);
    const QVector<Stroke> existing = highlightsInSelection();
    if (!existing.isEmpty())
        menu.addAction("Remove highlight", this, [this, existing] {
            QVector<Stroke> kept = m_pages[m_sel.page]->strokes();
            for (const Stroke &s : existing) kept.removeAll(s);
            commit({m_sel.page, m_pages[m_sel.page]->strokes(), kept});
            emit undoStateChanged();
            clearSelection();
        });
    menu.addSeparator();

    menu.addAction("Translate selection to தமிழ்", this, [this, text] { emit translateRequested(text); });
    const QString sentence = selectionSentence();
    if (!sentence.isEmpty() && sentence != text)
        menu.addAction("Translate the whole sentence to தமிழ்", this, [this, sentence] { emit translateRequested(sentence); });
    if (words <= 3)
        menu.addAction(QStringLiteral("Define “%1”").arg(shortText), this, [this, text, sentence] {
            emit wordPicked(PickedWord{text, m_sel.page, sentence});
        });
    menu.addSeparator();

    menu.addAction("Copy", QKeySequence::Copy, this, &ReaderView::copySelection);
    menu.addAction("Copy the whole sentence", this, [sentence] { QGuiApplication::clipboard()->setText(sentence); })
        ->setEnabled(!sentence.isEmpty());
    menu.addSeparator();
    menu.addAction(Theme::icon("search"), QStringLiteral("Find “%1” in this PDF").arg(shortText), this,
                   [this, text] { emit findRequested(text); });
    menu.addAction(Theme::icon("sparkle"), "Smart search all PDFs", this, [this, text] { emit smartSearchRequested(text); });
    menu.addAction("Search the web", this, [text] {
        openUrlInBrowser(QUrl("https://www.google.com/search?q=" + QUrl::toPercentEncoding(text)));
    });
    menu.exec(globalPos);
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
    if (event->type() == QEvent::ShortcutOverride && m_visual) {
        // Visual mode takes plain keys (vim motions/actions); Ctrl/Alt shortcuts still work.
        auto *k = static_cast<QKeyEvent *>(event);
        if (!(k->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) || k->matches(QKeySequence::Copy)) {
            event->accept();
            return true;
        }
    }
    if (event->type() == QEvent::ShortcutOverride && m_keyMode != KeyMode::None) {
        event->accept();
        return true;
    }
    return QGraphicsView::event(event);
}

void ReaderView::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Menu) { // keyboard menu key: the right-click menu at the mouse
        const QPoint at = viewport()->mapFromGlobal(QCursor::pos());
        QContextMenuEvent menu(QContextMenuEvent::Keyboard, at, QCursor::pos());
        contextMenuEvent(&menu);
        return;
    }
    if (m_visual) {
        visualKey(event);
        return;
    }
    if (hasSelection() && event->matches(QKeySequence::Copy)) {
        copySelection();
        emit message("Copied");
        return;
    }
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
    if (text.trimmed().isEmpty()) {
        clearSearch();
        return;
    }
    if (!m_search) {
        // Own copy of the PDF on a background thread; page text is cached for later searches.
        m_search = new TextSearch(m_doc.localPath, this);
        connect(m_search, &TextSearch::pageHits, this, [this](quint64 id, int page, const QList<TextHit> &hits) {
            if (id != m_searchId) return;
            m_hitsByPage[page] = hits;
            if (!m_searchTimer.isActive()) m_searchTimer.start();
        });
        connect(m_search, &TextSearch::counted, this, [this](quint64 id, int total) {
            if (id != m_searchId) return;
            m_searchTotal = total;
            if (total == 0) m_searchPending = false;
            emit searchStatus(m_searchIndex + 1, total);
        });
        connect(m_search, &TextSearch::finished, this, [this](quint64 id, int) {
            if (id != m_searchId) return;
            m_searchDone = true;
            onSearchResults();
        });
    }
    m_searchFrom = m_position;
    m_searchIndex = -1;
    m_currentHit.reset();
    m_searchPending = true;
    m_searchDone = false;
    m_searchTotal = 0;
    m_hitsByPage.clear();
    m_hits.clear();
    // Highlight-only refresh as results stream in; the first one near you is shown immediately.
    m_searchTimer.setInterval(40);
    m_searchId = m_search->search(text, m_position.page);
    emit searchStatus(0, 0);
}

void ReaderView::clearSearch()
{
    m_searchText.clear();
    m_searchPending = false;
    m_searchIndex = -1;
    m_currentHit.reset();
    m_hitsByPage.clear();
    m_hits.clear();
    m_searchId = 0;
    if (m_search)
        m_search->cancel();
    if (m_searchItem) m_searchItem->setPath({});
    if (m_currentMatchItem) m_currentMatchItem->setPath({});
    emit searchStatus(0, 0);
}

int ReaderView::searchIndexFrom(const Position &from, bool forward, bool wrap) const
{
    const int count = int(m_hits.size());
    auto after = [&](const TextHit &h) {
        const qreal ph = m_pagePoints[h.page].height();
        const qreal off = ph > 0 && !h.rects.isEmpty() ? h.rects.first().top() / ph : 0;
        return h.page > from.page || (h.page == from.page && off >= from.offset - 0.001);
    };
    if (forward) {
        for (int i = 0; i < count; ++i)
            if (after(m_hits[i])) return i;
        return wrap && count > 0 ? 0 : -1;
    }
    for (int i = count - 1; i >= 0; --i)
        if (!after(m_hits[i])) return i;
    return wrap && count > 0 ? count - 1 : -1;
}

void ReaderView::onSearchResults()
{
    if (!m_search)
        return;
    // Flatten in reading order (pages arrive starting at the current one, then wrap round).
    m_hits.clear();
    for (const QList<TextHit> &page : std::as_const(m_hitsByPage))
        m_hits.append(page);
    if (m_currentHit) {
        m_searchIndex = -1;
        for (int i = 0; i < m_hits.size(); ++i)
            if (m_hits[i].page == m_currentHit->first && m_hits[i].rects.first() == m_currentHit->second) {
                m_searchIndex = i;
                break;
            }
    }
    const int count = int(m_hits.size());
    QPainterPath all;
    for (int i = 0; i < count && i < 5000; ++i)
        for (const QRectF &r : m_hits[i].rects)
            all.addRect(toScene(m_hits[i].page, r));
    if (!m_searchItem) {
        m_searchItem = new HighlighterPathItem;
        m_searchItem->setPen(Qt::NoPen);
        m_searchItem->setBrush(QColor(255, 202, 40, 150));
        scene()->addItem(m_searchItem);
        m_searchItem->setZValue(9);
    }
    m_searchItem->setPath(all);

    if (m_searchPending && count > 0) {
        // Jump to the first match after where the search started, as soon as it's found;
        // once every page is searched, wrap round to the start.
        const int i = searchIndexFrom(m_searchFrom, !m_searchBackward, m_searchDone);
        if (i >= 0 && (m_searchDone || !m_searchBackward)) {
            m_searchPending = false;
            recordJump();
            showSearchResult(i);
            return;
        }
    }
    if (m_searchDone) m_searchPending = false;
    emit searchStatus(m_searchIndex + 1, qMax(count, m_searchTotal));
}

void ReaderView::showSearchResult(int index)
{
    if (index < 0 || index >= m_hits.size())
        return;
    m_searchIndex = index;
    const TextHit &hit = m_hits[index];
    m_currentHit = std::make_pair(hit.page, hit.rects.first());
    QPainterPath current;
    QRectF bounds;
    for (const QRectF &r : hit.rects) {
        // A thin outline just outside the letters, with a light tint, so the word stays readable.
        const QRectF sr = toScene(hit.page, r.adjusted(-1.5, -1.5, 1.5, 1.5));
        current.addRoundedRect(sr, 2, 2);
        bounds = bounds.united(sr);
    }
    if (!m_currentMatchItem) {
        QPen pen(QColor(255, 140, 0), 1.5);
        pen.setCosmetic(true); // same thin line at every zoom level
        m_currentMatchItem = new HighlighterPathItem;
        m_currentMatchItem->setPen(pen);
        m_currentMatchItem->setBrush(QColor(255, 120, 0, 190));
        scene()->addItem(m_currentMatchItem);
        m_currentMatchItem->setZValue(11);
    }
    m_currentMatchItem->setPath(current);
    ensureVisible(bounds, 80, 160);
    emit searchStatus(index + 1, qMax(int(m_hits.size()), m_searchTotal));
}

void ReaderView::findNext()
{
    if (m_hits.isEmpty())
        return;
    const bool forward = !m_searchBackward;
    const int count = int(m_hits.size());
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
