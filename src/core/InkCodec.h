#pragma once

#include "core/Models.h"

/// Encodes strokes in the same JSON shape the Android app uses (kotlinx.serialization):
/// [{"id":"…","tool":"PEN","color":4279903102,"width":0.0028,"points":[x,y,p,…]}]
namespace InkCodec {
QString encode(const QVector<Stroke> &strokes);
QVector<Stroke> decode(const QString &json);
} // namespace InkCodec
