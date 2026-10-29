#pragma once

#include "Document/Document.h"

#include <type_traits>
#include <utility>

namespace iiSharedCanvas::detail {

// A timeline candidate touches no asset payloads. Restore before durable file
// publication; publish can then swap the prepared candidate back without an
// allocation or exception after commit.
class TimelineGuard final {
    Document &document_;
    Timeline timeline_;
    DocumentAuthorship authorship_;
    FormatVersion version_;
    bool restored_=false, published_=false;

    void swapValues() noexcept {
        using std::swap;
        swap(document_.timeline,timeline_);
        swap(document_.authorship,authorship_);
        swap(document_.formatVersion,version_);
    }
public:
    explicit TimelineGuard(Document &document)
        : document_(document),timeline_(document.timeline),authorship_(document.authorship),
          version_(document.formatVersion) {
        static_assert(std::is_nothrow_swappable_v<Timeline>);
        static_assert(std::is_nothrow_swappable_v<DocumentAuthorship>);
        static_assert(std::is_nothrow_swappable_v<FormatVersion>);
        using std::swap; swap(document_.authorship,authorship_);
    }
    TimelineGuard(const TimelineGuard &)=delete;
    TimelineGuard &operator=(const TimelineGuard &)=delete;
    void restore() noexcept {
        if (!published_ && !restored_) { swapValues(); restored_=true; }
    }
    void publish() noexcept {
        if (published_) return;
        if (restored_) swapValues();
        published_=true;
    }
    ~TimelineGuard() noexcept { restore(); }
};
} // namespace iiSharedCanvas::detail
