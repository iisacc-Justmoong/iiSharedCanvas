#pragma once
#include "Document/Document.h"
#include <span>
#include <string>
#include <vector>

namespace iiSharedCanvas::detail {
// Returns source-order IDs referenced only by removed layers, retaining assets
// still referenced by any surviving procedural fill even when that fill is unused.
std::vector<std::string> removableLayerAssets(const Document &,std::span<const std::string> removed);
}
