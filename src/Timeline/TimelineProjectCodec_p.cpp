#include "Timeline/TimelineProjectCodec_p.hpp"
#include "Timeline/TimelineProjectFields_p.hpp"

#include <iiSDKCore/Core/Serialization/Serialization.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <new>
#include <tuple>
#include <type_traits>

namespace iiSharedCanvas::timeline_codec {
template<class T> concept Described = requires(T &value) { Fields<T>::tie(value); };
template<class T> struct EnumMaximum;
#define IISC_ENUM(Type, Last) template<> struct EnumMaximum<Type> { static constexpr auto value = Type::Last; };
IISC_ENUM(TimelineFrameRateMode, Passthrough)
IISC_ENUM(TimelineTimecodeCounting, DropFrame)
IISC_ENUM(TimelineColorRange, Full)
IISC_ENUM(TimelineScanMode, Interlaced)
IISC_ENUM(TimelineFieldOrder, BottomFirst)
IISC_ENUM(TimelineAlphaMode, Premultiplied)
IISC_ENUM(TimelineSubtitleKind, Metadata)
IISC_ENUM(TimelineStreamKind, Data)
IISC_ENUM(TimelineMediaRepresentationRole, OfflinePlaceholder)
IISC_ENUM(TimelineKeyframeInterpolation, Bezier)
IISC_ENUM(TimelineMarkerKind, Out)
IISC_ENUM(TimelineLoopMode, PingPong)
IISC_ENUM(TimelineVideoSampling, OpticalFlow)
IISC_ENUM(TimelineAudioStretchMode, Custom)
IISC_ENUM(TimelineTransitionAlignment, Custom)
IISC_ENUM(TimelineRateControl, Lossless)
IISC_ENUM(TimelineSubtitleOutputMode, Omit)
IISC_ENUM(TimelineRenderRangeMode, Custom)
#undef IISC_ENUM
template<class T> concept KnownEnum = std::is_enum_v<T> && requires { EnumMaximum<T>::value; };
} // namespace iiSharedCanvas::timeline_codec

namespace ii::serialization {

// Binary payload fields retain byte identity without allocating one CBOR item
// per byte. Core's decoder applies the caller's cumulative allocation budget.
template<> struct Codec<std::vector<std::uint8_t>> final {
    static Result<void> encode(Encoder &out, const std::vector<std::uint8_t> &value) {
        return out.binary(std::as_bytes(std::span(value)));
    }
    static Result<std::vector<std::uint8_t>> decode(Decoder &in) {
        auto bytes = in.binary();
        if (!bytes) return std::unexpected(bytes.error());
        std::vector<std::uint8_t> result(bytes->size());
        std::transform(bytes->begin(), bytes->end(), result.begin(),
                       [](std::byte value) { return std::to_integer<std::uint8_t>(value); });
        return result;
    }
};

template<iiSharedCanvas::timeline_codec::KnownEnum T> struct Codec<T> final {
    static Result<void> encode(Encoder &out, T value) {
        using U = std::underlying_type_t<T>;
        const auto raw = static_cast<U>(value);
        if (raw > static_cast<U>(iiSharedCanvas::timeline_codec::EnumMaximum<T>::value))
            return out.reject<void>(ErrorCode::schema_mismatch, "unknown timeline enumeration");
        return out.write(raw);
    }
    static Result<T> decode(Decoder &in) {
        using U = std::underlying_type_t<T>;
        auto raw = in.read<U>();
        if (!raw) return std::unexpected(raw.error());
        if (*raw > static_cast<U>(iiSharedCanvas::timeline_codec::EnumMaximum<T>::value))
            return in.reject<T>(ErrorCode::schema_mismatch, "unknown timeline enumeration");
        return static_cast<T>(*raw);
    }
};

// The clip window, track-effect map and editable video source are 1.29 trailing extensions.
// An absent extension is written in the exact 1.28 shape; both shapes are
// accepted when reading. The document version guards the new semantics.
template<iiSharedCanvas::timeline_codec::Described T> struct Codec<T> final {
    static Result<void> encode(Encoder &out, const T &value) {
        auto fields = iiSharedCanvas::timeline_codec::Fields<T>::tie(value);
        auto count=std::tuple_size_v<decltype(fields)>;
        if constexpr (std::is_same_v<T,iiSharedCanvas::TimelineClipProperties>)
            if(!value.contentWindow)--count;
        if constexpr (std::is_same_v<T,iiSharedCanvas::TimelineTrackProperties>)
            if(!value.effectTimeMap)--count;
        if constexpr (std::is_same_v<T,iiSharedCanvas::TimelineVideoClip>)
            if(!value.editableOriginal)--count;
        if (auto result = out.array(count); !result) return result;
        Result<void> result;
        std::size_t index=0;
        std::apply([&](const auto &...field) {
            ((result && index++<count ? void(result = out.write(field)) : void()), ...);
        }, fields);
        return result;
    }
    static Result<T> decode(Decoder &in) {
        T value;
        auto fields = iiSharedCanvas::timeline_codec::Fields<T>::tie(value);
        auto count = in.array();
        if (!count) return std::unexpected(count.error());
        constexpr auto fullCount=std::tuple_size_v<decltype(fields)>;
        const bool oldShape=(std::is_same_v<T,iiSharedCanvas::TimelineClipProperties> ||
                             std::is_same_v<T,iiSharedCanvas::TimelineTrackProperties> ||
                             std::is_same_v<T,iiSharedCanvas::TimelineVideoClip>) && *count==fullCount-1;
        if (*count != fullCount && !oldShape)
            return in.reject<T>(ErrorCode::schema_mismatch, "timeline field count mismatch");
        Result<void> result;
        std::size_t index=0;
        std::apply([&](auto &...field) {
            const auto read = [&](auto &destination) {
                if (!result || index++>=*count) return;
                using Field = std::remove_cvref_t<decltype(destination)>;
                auto decoded = in.read<Field>();
                if (!decoded) result = std::unexpected(decoded.error());
                else destination = std::move(*decoded);
            };
            (read(field), ...);
        }, fields);
        if (!result) return std::unexpected(result.error());
        return value;
    }
};
} // namespace ii::serialization

namespace iiSharedCanvas::detail {
namespace {
template<class T> struct IsVector : std::false_type {};
template<class T, class A> struct IsVector<std::vector<T,A>> : std::true_type { using Value = T; };
template<class T> struct IsOptional : std::false_type {};
template<class T> struct IsOptional<std::optional<T>> : std::true_type {};
template<class T> struct IsVariant : std::false_type {};
template<class... T> struct IsVariant<std::variant<T...>> : std::true_type {};

struct TextBudget {
    const SerializationLimits &limits;
    std::uint64_t total = 0;
    bool valid = true;
};
template<class T> void measureText(const T &value, TextBudget &budget) {
    if (!budget.valid) return;
    if constexpr (std::is_same_v<T, std::string>) {
        const auto size = std::uint64_t(value.size());
        if (size > budget.limits.maximumStringBytes || size > budget.limits.maximumTotalStringBytes ||
            budget.total > budget.limits.maximumTotalStringBytes - size) budget.valid = false;
        else budget.total += size;
    } else if constexpr (timeline_codec::Described<T>) {
        std::apply([&](const auto &...field) { (measureText(field, budget), ...); }, timeline_codec::Fields<T>::tie(value));
    } else if constexpr (IsVector<T>::value) {
        if constexpr (!std::is_same_v<typename IsVector<T>::Value, std::uint8_t>)
            for (const auto &item : value) measureText(item, budget);
    } else if constexpr (IsOptional<T>::value) {
        if (value) measureText(*value, budget);
    } else if constexpr (IsVariant<T>::value) {
        std::visit([&](const auto &item) { measureText(item, budget); }, value);
    }
}

ii::serialization::Limits codecLimits(const SerializationLimits &limits) {
    ii::serialization::Limits value;
    value.max_bytes = static_cast<std::size_t>(std::min<std::uint64_t>(
        limits.maximumTimelineProjectBytes, std::numeric_limits<std::size_t>::max()));
    value.max_blob_bytes = value.max_bytes;
    value.max_container_items = 1'000'000;
    value.max_items = 8'000'000;
    value.max_depth = 96;
    value.max_allocation_bytes = value.max_bytes > std::numeric_limits<std::size_t>::max() / 4
        ? std::numeric_limits<std::size_t>::max() : value.max_bytes * 4;
    return value;
}
IiscError failure(const ii::serialization::Error &error) {
    return {error.code == ii::serialization::ErrorCode::limit_exceeded
                ? IiscErrorCode::LimitExceeded : IiscErrorCode::InvalidData,
            error.offset, error.message};
}
}

IiscError measureTimelineStrings(const TimelineProject &project, const SerializationLimits &limits, std::uint64_t &total) {
    TextBudget budget{limits};
    measureText(project, budget);
    if (!budget.valid) return {IiscErrorCode::LimitExceeded, 0, "timeline text exceeds the configured string budget"};
    total = budget.total;
    return {};
}

TimelineProjectEncodeResult encodeTimelineProject(const TimelineProject &project,
                                                   const SerializationLimits &limits) {
    try {
        std::uint64_t stringBytes = 0;
        if (auto error = measureTimelineStrings(project, limits, stringBytes); error.code != IiscErrorCode::None)
            return {{}, std::move(error)};
        auto encoded = ii::serialization::serialize(project, codecLimits(limits));
        if (!encoded) return {{}, failure(encoded.error())};
        std::vector<std::uint8_t> bytes(encoded->size());
        std::transform(encoded->begin(), encoded->end(), bytes.begin(),
                       [](std::byte value) { return std::to_integer<std::uint8_t>(value); });
        return {std::move(bytes), {}};
    } catch (const std::bad_alloc &) {
        return {{}, {IiscErrorCode::LimitExceeded, 0, "insufficient memory for timeline encoding"}};
    }
}

TimelineProjectDecodeResult decodeTimelineProject(std::span<const std::uint8_t> bytes,
                                                   const SerializationLimits &limits) {
    try {
        auto decoded = ii::serialization::deserialize<TimelineProject>(std::as_bytes(bytes), codecLimits(limits));
        if (!decoded) return {{}, failure(decoded.error())};
        std::uint64_t stringBytes = 0;
        if (auto error = measureTimelineStrings(*decoded, limits, stringBytes); error.code != IiscErrorCode::None)
            return {{}, std::move(error)};
        return {std::move(*decoded), {}};
    } catch (const std::bad_alloc &) {
        return {{}, {IiscErrorCode::LimitExceeded, 0, "insufficient memory for timeline decoding"}};
    }
}
} // namespace iiSharedCanvas::detail
