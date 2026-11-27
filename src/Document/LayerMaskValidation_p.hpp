#pragma once

#include "Document/Document.h"
#include <optional>
#include <string>

namespace iiSharedCanvas::detail {
// Shared validation keeps detached mask preparation and document loading on
// the same bounds, transform, channel, and asset ownership contract.
std::string validateLayerMask(const Document &, const std::optional<LayerMask> &, bool vectorOnly);
}
