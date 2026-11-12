#pragma once

#include <QStyledItemDelegate>

/// Item data roles shared by the Home views and their painters.
namespace HomeRoles {
constexpr int Id = Qt::UserRole;          ///< docKey / folder id / word
constexpr int Kind = Qt::UserRole + 1;    ///< "doc" or "folder"
constexpr int Percent = Qt::UserRole + 2; ///< reading progress 0–100 (docs)
constexpr int Badge = Qt::UserRole + 3;   ///< short line under the title
constexpr int Tamil = Qt::UserRole + 4;   ///< words: Tamil meaning
constexpr int Detail = Qt::UserRole + 5;  ///< words: definition
} // namespace HomeRoles

/// A PDF (or folder) tile: rounded cover with a soft shadow, title, progress bar and status.
class TileDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    static QSize tileSize() { return {190, 262}; }
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return tileSize(); }
};

/// A saved word: the word and its Tamil meaning, the definition underneath, a divider line between.
class WordDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;
};
