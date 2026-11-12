#pragma once

#include "core/Models.h"
#include "core/TextSearch.h"
#include "ui/MouseScheme.h"
#include "ui/ToolSettings.h"

#include <QGraphicsView>
#include <QPdfLink>
#include <QMap>
#include <QTimer>
#include <optional>

class AppContext;
class PageItem;
class QPdfDocument;
class QPdfSelection;
class QGraphicsRectItem;
class QKeyEvent;
class TextSearch;
struct TextHit;
class QPdfPageRenderer;
class QGraphicsPathItem;

/// A highlight on a page: one or more highlighter strokes (one per line), with its text and comment.
struct Highlight {
    int page = 0;
    QString key;           ///< The strokes' group id, or the stroke id for a hand-drawn highlight.
    QVector<Stroke> strokes;
    QString text;          ///< The highlighted text.
    QString note;          ///< Your comment (empty: none yet).
    quint32 color = 0;
    QRectF box;            ///< Normalised to page width, like ink.
};

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
    bool open(const Document &doc, int initialPage = -1, double initialOffset = 0);
    void close();
    bool isOpen() const { return m_pdf != nullptr; }
    const Document &document() const { return m_doc; }
    /// Re-renders pages in the current page colours (after Settings → Appearance).
    void refreshColors();
    /// After a rename (here or synced from another device).
    void setDocumentTitle(const QString &title) { m_doc.title = title; }

    ToolSettings tools() const { return m_tools; }
    void setTools(const ToolSettings &t);

    // Highlights and their comments
    QVector<Highlight> highlights() const;
    std::optional<Highlight> highlightAt(const QPointF &viewportPos) const;
    void setHighlightNote(int page, const QString &key, const QString &note);
    void removeHighlight(int page, const QString &key);
    /// Scrolls to a highlight and flashes it.
    void revealHighlight(int page, const QString &key);

    // Text selection (mouse pointer: drag over text). Right-click it for highlight, translate, …
    bool hasSelection() const { return !m_sel.text.isEmpty(); }
    /// Where a point on a page (PDF points) is in the viewport right now.
    QPoint viewportPointFor(int page, const QPointF &pdfPoint) const;
    QString selectedText() const { return m_sel.text; }
    void clearSelection();
    void copySelection();
    /// Marks the selected lines with highlighter ink (synced, undoable, same as drawn highlights).
    QString highlightSelection(quint32 argb); ///< Returns the new highlight's key.
    void underlineSelection();
    // Vim visual mode: v (characters) / V (lines), then motions and actions — see visualKey().
    bool inVisualMode() const { return m_visual; }
    void enterVisualMode(bool lineWise);
    void exitVisualMode(bool keepSelection);
    // Actions on the selection (selection menu, visual mode and keyboard shortcuts).
    void translateSelection(bool sentence);
    void defineSelection();
    void findSelection();
    void smartSearchSelection();
    /// Highlights the selection and asks for a comment on it.
    void commentSelection();
    /// Next / previous highlight from where you are (wraps round).
    void gotoHighlight(bool next);
    /// The whole sentence(s) the selection sits in.
    QString selectionSentence() const;
    bool isZoomedIn() const;

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
    qreal zoom() const { return m_zoom; }
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
    /// Ink changed (drawn, erased, undone, synced): the list of highlights may have changed.
    void highlightsChanged();
    /// A click on a highlight (with the mouse pointer); show its comment.
    void highlightClicked(const Highlight &highlight, const QPoint &globalPos);
    /// "Add comment" / "Edit comment" was chosen for a highlight.
    void editHighlightNoteRequested(const Highlight &highlight);
    /// From the selection menu.
    void translateRequested(const QString &text);
    void findRequested(const QString &text);
    void smartSearchRequested(const QString &text);
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
    void onSearchFinished();
    void updateCursor();
    struct TextSelection {
        int page = -1;
        QPointF from; ///< PDF points on [page]
        QString text;
        QList<QRectF> rects; ///< One per line, in PDF points
        int start = -1, end = -1; ///< Character indices on the page
    };
    bool overText(const QPointF &viewportPos);
    QPointF pagePoint(int page, const QPointF &viewportPos) const;
    bool beginSelection(const QPointF &viewportPos);
    void extendSelection(const QPointF &viewportPos);
    void showSelectionMenu(const QPoint &globalPos);
    void showSelection(const QPdfSelection &sel);
    bool needSelection();
    bool visualKey(QKeyEvent *e);
    void updateVisualSelection();
    int vLineStart(int i) const;
    int vLineEnd(int i) const;
    bool m_visual = false;
    bool m_visualLine = false;
    int m_vAnchor = 0;
    int m_vCaret = 0;
    QString m_vText; ///< Text of the page being selected (character indices match the PDF's).
    QGraphicsRectItem *m_caretItem = nullptr;
    QString m_hlNavKey; ///< Last highlight reached with ] / [ ...
    Position m_hlNavPos; ///< ... and where that left the view.
    QVector<Stroke> highlightsInSelection() const;
    void addStrokes(int page, const QVector<Stroke> &strokes);
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
    TextSelection m_sel;
    bool m_selecting = false;
    enum class Gesture { None, Pending, Pan, Select };
    Gesture m_gesture = Gesture::None;
    Qt::MouseButton m_gestureButton = Qt::NoButton;
    QPointF m_gestureStart;
    void endGesture();
    void startTextSelection();
    QTimer m_longPress;
    QTimer m_selTimer;
    QPointF m_pendingSelPos;
    MouseScheme m_mouse;
    QGraphicsPathItem *m_selItem = nullptr;
    QHash<int, QList<QRectF>> m_lineBoxes;
    mutable QHash<QString, QString> m_highlightText; ///< Text under hand-drawn highlights (looked up once).
    QString textUnder(int page, const Stroke &s) const; ///< Text line boxes per page (points), for the I-beam cursor.

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
    quint64 m_renderGeneration = 0; ///< Bumped on close / colour change; late recolours are dropped.

    QVector<Position> m_jumpBack;
    QVector<Position> m_jumpForward;
    KeyMode m_keyMode = KeyMode::None;
    QVector<Hint> m_hints;
    QString m_hintTyped;

    TextSearch *m_search = nullptr;
    quint64 m_searchId = 0;
    bool m_searchDone = false;
    int m_searchTotal = 0; ///< Matches in the whole PDF (known before all highlight boxes are).
    QMap<int, QList<TextHit>> m_hitsByPage; ///< As they stream in (any page order).
    QList<TextHit> m_hits;                  ///< Flattened in reading order.
    std::optional<std::pair<int, QRectF>> m_currentHit; ///< Keeps the current match as new pages arrive.
    QString m_searchText;
    bool m_searchBackward = false;
    bool m_searchPending = false; ///< Waiting for the first match to jump to.
    Position m_searchFrom;
    int m_searchIndex = -1;
    QGraphicsPathItem *m_searchItem = nullptr;
    QGraphicsPathItem *m_currentMatchItem = nullptr;
    QTimer m_searchTimer;
};
