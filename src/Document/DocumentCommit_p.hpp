#pragma once
#include "Document/Document.h"

namespace iiSharedCanvas::detail {
// MSVC's map move construction can allocate. Prepare optional map storage
// before a durable commit, then replace fields without moving the Document.
class PreparedDocumentCommit final {
    Document &destination_, &source_;
    bool preparedPublication_ = false, committed_ = false;
public:
    PreparedDocumentCommit(Document &destination, Document &source)
        : destination_(destination), source_(source) {
        if (source_.publicationMetadata && !destination_.publicationMetadata) {
            destination_.publicationMetadata.emplace();
            preparedPublication_ = true;
        }
    }
    PreparedDocumentCommit(const PreparedDocumentCommit &) = delete;
    ~PreparedDocumentCommit() {
        if (preparedPublication_ && !committed_) destination_.publicationMetadata.reset();
    }
    void commit() noexcept {
        const auto move = []<class T>(T &destination, T &source) noexcept {
            static_assert(std::is_nothrow_move_assignable_v<T>);
            destination = std::move(source);
        };
        move(destination_.formatVersion, source_.formatVersion);
        move(destination_.extent, source_.extent);
        move(destination_.canvasMode, source_.canvasMode);
        move(destination_.infiniteCanvas, source_.infiniteCanvas);
        move(destination_.timeline, source_.timeline);
        move(destination_.assets, source_.assets);
        move(destination_.layers, source_.layers);
        move(destination_.frames, source_.frames);
        move(destination_.stableDiffusionMetadata, source_.stableDiffusionMetadata);
        move(destination_.audioAssets, source_.audioAssets);
        move(destination_.audioTracks, source_.audioTracks);
        move(destination_.authorship, source_.authorship);
        move(destination_.artboards, source_.artboards);
        if (source_.publicationMetadata) move(*destination_.publicationMetadata, *source_.publicationMetadata);
        else destination_.publicationMetadata.reset();
        committed_ = true;
    }
};
} // namespace iiSharedCanvas::detail
