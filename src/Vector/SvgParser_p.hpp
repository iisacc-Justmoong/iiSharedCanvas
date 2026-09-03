#pragma once

#include "VectorCodec.h"

#include <QPainterPath>

namespace iiSharedCanvas::vector_detail {
QPainterPath painterPath(const VectorPath &path);
VectorPath vectorPath(const QPainterPath &path);
VectorImportResult parseSvg(const QByteArray &xml, const VectorImportOptions &options);
}
