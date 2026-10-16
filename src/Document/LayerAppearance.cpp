#include "Document/LayerAppearance.h"

namespace iiSharedCanvas {
bool LayerMask::operator==(const LayerMask &other) const
{
    return assetId == other.assetId && enabled == other.enabled && inverted == other.inverted
        && linkedToLayer == other.linkedToLayer && channel == other.channel
        && density == other.density && featherRadius == other.featherRadius
        && outsideCoverage == other.outsideCoverage && transform.m11 == other.transform.m11
        && transform.m12 == other.transform.m12 && transform.m21 == other.transform.m21
        && transform.m22 == other.transform.m22 && transform.translationX == other.transform.translationX
        && transform.translationY == other.transform.translationY;
}
} // namespace iiSharedCanvas
