#pragma once

#include <QFileInfo>
#include <QString>
#include <QVector>

// Data model shared with the Android app. Field names and meanings match its Room entities and the
// JSON files in the Drive appDataFolder, so both apps read each other's data.

/// A PDF the user has opened. docKey is the first 32 hex chars of the file's SHA-256, so the same
/// PDF on any device (Android or desktop) shares its notes.
struct Document {
    QString docKey;
    QString title;
    QString localPath;   ///< Empty when the PDF is only in the cloud.
    QString driveFileId; ///< Id of the uploaded copy in the Drive appDataFolder, once synced.
    QString folderId;    ///< Catalogue folder; empty (or a deleted folder) = Unfiled.
    int pageCount = 0;
    int lastPage = 0;
    double lastOffset = 0; ///< How far down lastPage the reader was (0..1 of page height).
    qint64 lastOpenedAt = 0;
    qint64 updatedAt = 0;
    bool deleted = false; ///< Tombstone for "delete from all devices".

    bool onDevice() const { return !localPath.isEmpty() && QFileInfo::exists(localPath); }
    bool inCloud() const { return !driveFileId.isEmpty(); }
};

enum class InkTool { Pen, Highlighter };

/// One stroke. Coordinates are normalised to the page *width* (x and y both divided by it), and
/// points are flattened as x, y, pressure triples, exactly like the Android app.
struct Stroke {
    QString id;
    InkTool tool = InkTool::Pen;
    quint32 color = 0xFF000000; ///< ARGB
    float width = 0.0028f;      ///< Fraction of page width.
    QVector<float> points;
    // Highlights made from selected text (all optional, synced with Android):
    QString group; ///< Strokes of one highlight across several lines share this id.
    QString text;  ///< The highlighted text (on the group's first stroke).
    QString note;  ///< Your comment on the highlight (on the group's first stroke).

    int pointCount() const { return int(points.size() / 3); }
    float x(int i) const { return points[i * 3]; }
    float y(int i) const { return points[i * 3 + 1]; }
    float pressure(int i) const { return points[i * 3 + 2]; }
    bool operator==(const Stroke &o) const { return id == o.id; }
};

/// A word the user chose to keep. Deletions are tombstoned so they sync to other devices.
struct VocabWord {
    QString word;
    QString phonetic;
    QString partOfSpeech;
    QString definition;
    QString tamil;
    QString contextSentence;
    QString sourceDocKey;
    QString sourceTitle;
    int sourcePage = -1; ///< 0-based; -1 when the word wasn't saved from a document.
    qint64 addedAt = 0;
    qint64 updatedAt = 0;
    bool deleted = false;
};

/// A catalogue folder. Folders nest through parentId (empty = top level). Deletions are tombstoned.
struct Folder {
    QString id;
    QString name;
    QString parentId;
    qint64 updatedAt = 0;
    bool deleted = false;
};

/// All strokes of one page as stored in the database and synced (strokes as a JSON string).
struct PageInkRow {
    int page = 0;
    QString strokesJson;
    qint64 updatedAt = 0;
};
