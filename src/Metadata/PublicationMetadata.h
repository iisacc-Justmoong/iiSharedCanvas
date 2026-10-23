#pragma once

#include <map>
#include <string>
#include <variant>
#include <vector>

namespace iiSharedCanvas {

// Shared iisacc publication data. No application, UI or transport dependency.
using PublicationSetting = std::variant<std::string, bool>;

struct CanvasPublicationMetadata {
    // Empty identifies the document's root canvas; nonempty identifies an artboard.
    std::string artboardId;
    std::string caption;
    std::string alternativeText;
    bool operator==(const CanvasPublicationMetadata &) const = default;
};

struct PublicationMetadata {
    std::string title;
    std::string description;
    std::string coverArtboardId;
    // Publication order is independent of the artwork's stacking order.
    std::vector<CanvasPublicationMetadata> canvases;
    // Typed common settings; consumers preserve keys they do not interpret.
    std::map<std::string, PublicationSetting> settings;
    bool operator==(const PublicationMetadata &) const = default;
};

} // namespace iiSharedCanvas
