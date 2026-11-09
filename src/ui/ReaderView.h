#pragma once

#include "core/Models.h"
#include "ui/ToolSettings.h"

#include <QGraphicsView>
#include <QPdfLink>
#include <QMap>
#include <QTimer>
#include <optional>

class AppContext;
class PageItem;
class QPdfDocument;
class QPdfSearchModel;
class QPdfPageRenderer;
class QGraphicsPathItem;

/// A word picked on a page (for the dictionary).
struct PickedWord {
    QString word;
    int page = 0;
    QString context; ///< The sentence/line it sits in.
};

/**
 * Continuous-scroll PDF view with ink.
 *
 * Input:
 *  - Pen tablets (Wacom, Windows Ink, S Pen on Galaxy Books…): pressure-sensitive ink. The eraser
 *    end or a held barrel button erases, like the S Pen side button on Android.
 *  - Mouse: draws with the current tool when "mouse draws" is on, otherwise drags to pan.
 *    Right-click a word to define it; Ctrl+wheel zooms.
 *  - Touch: one finger pans, two fingers pinch-zoom.
 */
class ReaderView : public QGraphicsView {
    Q_OBJECT
public:
    ReaderView(AppContext &ctx, QWidget *parent = nullptr);
    ~ReaderView() override;

    /// Opens a document that is on this device. [initialPage] >= 0 overrides the saved position.
    bool open(const Document &doc, int initialPage = -1);
    void close();
    bool isOpen() const { return m_pdf != nullptr; }
    const Document &document() const { return m_doc; }

    ToolSettings tools() const { return m_tools; }
    void setTools(const ToolSettings &t);

    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    void undo();
    void redo();
    void clearPage();

    void zoomIn();
    void zoomOut();
    void fitWidth();
    void rotateClockwise();
    int rotation() const { return m_rotation; }
    void setRotation(int degrees);

    int currentPage() const { return m_position.page; }
    void jumpTo(int page, double offset);
    /// Saves the reading position now (e.g. before closing).
    void flushProgress();
    /// Reloads ink from the database unless the user has drawn since opening (after a sync).
    void reloadInkIfUntouched();
    bool userMoved() const { return m_userMoved; }
    void clearHighlight();
    /// The word at a point on a page (normalised page coordinates), optionally highlighting it.
    std::optional<PickedWord> wordAtPage(int page, const QPointF &norm, bool highlight);
    QPdfDocument *pdfDocument() const { return m_pdf; }

    // Keyboard navigation (vim/zathura-style commands call these)
    void scrollLines(int dx, int dy);
    void scrollScreens(qreal fraction);
    void nextPage();
    void prevPage();
    void goToFirstPage();
    void goToLastPage();
    /// Goes to a page and records the jump (so jumpBack returns here).
    void goToPage(int page, double offset = 0);
    void jumpBack();
    void jumpForward();
    /// The next letter typed sets / jumps to a mark.
    void beginSetMark();
    void beginJumpToMark();

    // Search
    void setSearchText(const QString &text, bool backward = false);
    QString searchText() const { return m_searchText; }
    bool hasCurrentMatch() const { return m_searchIndex >= 0; }
    /// Next / previous match in the search direction (vim n / N).
    void findNext();
    void findPrevious();
    void clearSearch();

    /// Shows letter hints on the links on screen; typing a hint follows that link.
    void showLinkHints();

signals:
    void wordPicked(const PickedWord &word);
    void pageChanged(int page, int pageCount);
    void undoStateChanged();
    void zoomChanged(int percent);
    /// Search progress: current match (1-based, 0 = none) of total.
    void searchStatus(int current, int total);
    /// A short status message (mark set, no links on screen, …); empty clears it.
    void message(const QString &text);

protected:
    bool event(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool viewportEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    struct InkEdit {
        int page;
        QVector<Stroke> before;
        QVector<Stroke> after;
    };
    struct Position {
        int page = 0;
        double offset = 0;
    };

    // Layout & rendering
    void layoutPages();
    void applyTransform();
    void requestRender(int page, int widthPx);
    void evictFarPages();
    int pageAt(qreal sceneY) const;
    std::optional<std::pair<int, QPointF>> pageAndNorm(const QPointF &viewportPos) const;
    Position visiblePosition() const;
    void updatePosition();

    // Ink
    void beginStroke(const QPointF &viewportPos, float pressure, bool erase);
    void extendStroke(const QPointF &viewportPos, float pressure);
    void endStroke();
    void eraseAt(int page, const QPointF &norm);
    void commit(const InkEdit &edit);
    void applyInk(int page, const QVector<Stroke> &strokes);

    // Dictionary
    std::optional<PickedWord> wordAt(const QPointF &viewportPos, bool highlight);

    // Zoom
    void setZoom(qreal zoom, const QPointF &anchorViewportPos);

    // Keyboard modes, jumps, search, links
    enum class KeyMode { None, SetMark, JumpToMark, Hints };
    struct Hint {
        QString label;
        QPdfLink link;
        QGraphicsItem *item;
    };
    void recordJump();
    void jumpToPosition(const Position &p);
    void cancelKeyMode();
    void filterHints();
    void followLink(const QPdfLink &link);
    QRectF toScene(int page, const QRectF &pointsRect) const;
    void onSearchResults();
    int searchIndexFrom(const Position &from, bool forward, bool wrap) const;
    void showSearchResult(int index);

    AppContext &m_ctx;
    Document m_doc;
    QPdfDocument *m_pdf = nullptr;
    QPdfPageRenderer *m_renderer = nullptr;
    QVector<PageItem *> m_pages;
    QVector<qreal> m_pageTops;
    QVector<QSizeF> m_pagePoints; ///< Page sizes in PDF points (for text lookup).
    QGraphicsPathItem *m_highlight = nullptr;

    ToolSettings m_tools;
    qreal m_zoom = 1.0; ///< Relative to fit-width.
    int m_rotation = 0;

    // Active stroke
    bool m_drawing = false;
    bool m_erasing = false;
    int m_strokePage = -1;
    Stroke m_live;
    QPointF m_downPos;
    QVector<Stroke> m_eraseBefore;

    // Touch / panning
    bool m_panning = false;
    QPointF m_lastPan;
    qreal m_pinchStartDist = 0;
    qreal m_pinchStartZoom = 1;

    QVector<InkEdit> m_undo;
    QVector<InkEdit> m_redo;

    Position m_start;
    Position m_position;
    bool m_userMoved = false;
    bool m_restoring = false;
    QTimer m_saveTimer;
    QTimer m_evictTimer;

    QVector<Position> m_jumpBack;
    QVector<Position> m_jumpForward;
    KeyMode m_keyMode = KeyMode::None;
    QVector<Hint> m_hints;
    QString m_hintTyped;

    QPdfSearchModel *m_search = nullptr;
    QString m_searchText;
    bool m_searchBackward = false;
    bool m_searchPending = false; ///< Waiting for the first match to jump to.
    Position m_searchFrom;
    int m_searchIndex = -1;
    QGraphicsPathItem *m_searchItem = nullptr;
    QGraphicsPathItem *m_currentMatchItem = nullptr;
    QTimer m_searchTimer;
    QTimer m_searchSettle;
};
