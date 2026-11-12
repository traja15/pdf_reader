#include "core/InkCodec.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace InkCodec {

QString encode(const QVector<Stroke> &strokes)
{
    QJsonArray arr;
    for (const Stroke &s : strokes) {
        QJsonArray pts;
        for (float v : s.points)
            pts.append(double(v));
        QJsonObject o{
            {"id", s.id},
            {"tool", s.tool == InkTool::Highlighter ? "HIGHLIGHTER" : "PEN"},
            {"color", double(s.color)},
            {"width", double(s.width)},
            {"points", pts},
        };
        if (!s.group.isEmpty()) o["group"] = s.group;
        if (!s.text.isEmpty()) o["text"] = s.text;
        if (!s.note.isEmpty()) o["note"] = s.note;
        arr.append(o);
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

QVector<Stroke> decode(const QString &json)
{
    QVector<Stroke> out;
    if (json.isEmpty())
        return out;
    const QJsonArray arr = QJsonDocument::fromJson(json.toUtf8()).array();
    out.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        Stroke s;
        s.id = o["id"].toString();
        s.tool = o["tool"].toString() == "HIGHLIGHTER" ? InkTool::Highlighter : InkTool::Pen;
        s.color = quint32(qint64(o["color"].toDouble()));
        s.width = float(o["width"].toDouble());
        s.group = o["group"].toString();
        s.text = o["text"].toString();
        s.note = o["note"].toString();
        const QJsonArray pts = o["points"].toArray();
        s.points.reserve(pts.size());
        for (const QJsonValue &p : pts)
            s.points.append(float(p.toDouble()));
        if (!s.id.isEmpty() && s.pointCount() > 0)
            out.append(std::move(s));
    }
    return out;
}

} // namespace InkCodec
