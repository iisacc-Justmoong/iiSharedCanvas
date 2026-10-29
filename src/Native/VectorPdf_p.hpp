#pragma once

#include <string>

namespace iiSharedCanvas::media_detail {
// PDF numeric tokens cannot use exponent notation.
std::string pdfNumber(double value);
}
