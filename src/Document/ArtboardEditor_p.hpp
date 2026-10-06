#pragma once

#include "Document/DocumentEditor.h"

namespace iiSharedCanvas::artboard_detail {
DocumentEditResult changed();
DocumentEditResult missing();
DocumentEditResult duplicate();
DocumentEditResult badIndex();
} // namespace iiSharedCanvas::artboard_detail
