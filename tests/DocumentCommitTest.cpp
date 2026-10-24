#include "Document/DocumentCommit_p.hpp"
#include <iostream>
#include <stdexcept>

using namespace iiSharedCanvas;
int main() {
    try {
        Document current;
        current.extent = {16, 24};
        auto *identity = &current;
        Document next;
        next.extent = {32, 48};
        next.publicationMetadata.emplace();
        next.publicationMetadata->title = "Complete publication";
        next.publicationMetadata->settings["enabled"] = true;
        {
            detail::PreparedDocumentCommit abandoned(current, next);
        }
        if (current.publicationMetadata || current.extent != CanvasExtent{16, 24})
            throw std::runtime_error("Aborted preparation changed the live document");
        detail::PreparedDocumentCommit(current, next).commit();
        if (&current != identity || current.extent != CanvasExtent{32, 48} ||
            current.publicationMetadata->title != "Complete publication" ||
            !std::get<bool>(current.publicationMetadata->settings.at("enabled")))
            throw std::runtime_error("Commit lost identity, geometry or map-backed publication metadata");
        Document cleared;
        cleared.extent = {64, 64};
        detail::PreparedDocumentCommit(current, cleared).commit();
        if (&current != identity || current.publicationMetadata || current.extent != CanvasExtent{64, 64})
            throw std::runtime_error("Clearing optional metadata changed identity or retained old settings");
        std::cout << "Prepared document commit passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
