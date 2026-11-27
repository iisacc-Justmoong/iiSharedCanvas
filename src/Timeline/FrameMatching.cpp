#include "Timeline/FrameMatching.h"
#include "Timeline/ExactBezier_p.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace iiSharedCanvas {
namespace {
struct Failure : std::runtime_error { using std::runtime_error::runtime_error; };
using Integer = std::int64_t;
using Unsigned = std::uint64_t;

Unsigned magnitude(Integer value) {
    return value < 0 ? Unsigned(-(value + 1)) + 1 : Unsigned(value);
}
Integer signedValue(Unsigned value, bool negative) {
    constexpr auto max = Unsigned(std::numeric_limits<Integer>::max());
    if (value > max + (negative ? 1U : 0U)) throw Failure("timeline rational exceeds its integer budget");
    if (!negative) return static_cast<Integer>(value);
    if (value == max + 1) return std::numeric_limits<Integer>::min();
    return -static_cast<Integer>(value);
}
Integer multiply(Integer a, Integer b) {
    const auto left = magnitude(a), right = magnitude(b);
    if (right && left > std::numeric_limits<Unsigned>::max() / right)
        throw Failure("timeline rational exceeds its integer budget");
    return signedValue(left * right, (a < 0) != (b < 0));
}
Integer add(Integer a, Integer b) {
    if ((b > 0 && a > std::numeric_limits<Integer>::max() - b) ||
        (b < 0 && a < std::numeric_limits<Integer>::min() - b))
        throw Failure("timeline rational exceeds its integer budget");
    return a + b;
}
Integer negate(Integer value) {
    if (value == std::numeric_limits<Integer>::min()) throw Failure("timeline rational exceeds its integer budget");
    return -value;
}
struct Fraction { Integer n = 0, d = 1; };
Fraction fraction(Integer n, Integer d = 1) {
    if (d <= 0) throw Failure("invalid timeline rational denominator");
    const auto divisor = std::gcd(magnitude(n), Unsigned(d));
    return {signedValue(magnitude(n) / divisor, n < 0), static_cast<Integer>(Unsigned(d) / divisor)};
}
Fraction plus(Fraction a, Fraction b) {
    const auto divisor = std::gcd(a.d, b.d);
    const auto x = b.d / divisor, y = a.d / divisor;
    return fraction(add(multiply(a.n, x), multiply(b.n, y)), multiply(a.d, x));
}
Fraction minus(Fraction a, Fraction b) { return plus(a, {negate(b.n), b.d}); }
Fraction times(Fraction a, Fraction b) {
    const auto first = std::gcd(magnitude(a.n), Unsigned(b.d));
    const auto second = std::gcd(magnitude(b.n), Unsigned(a.d));
    const auto an = signedValue(magnitude(a.n) / first, a.n < 0);
    const auto bn = signedValue(magnitude(b.n) / second, b.n < 0);
    return fraction(multiply(an, bn), multiply(a.d / static_cast<Integer>(second), b.d / static_cast<Integer>(first)));
}
Fraction divide(Fraction a, Fraction b) {
    if (!b.n || magnitude(b.n) > Unsigned(std::numeric_limits<Integer>::max()))
        throw Failure("invalid timeline rational division");
    return times(a, fraction(b.n < 0 ? -b.d : b.d, static_cast<Integer>(magnitude(b.n))));
}
int compare(Fraction a, Fraction b) {
    const auto divisor = std::gcd(a.d, b.d);
    const auto left = multiply(a.n, b.d / divisor), right = multiply(b.n, a.d / divisor);
    return left < right ? -1 : left > right ? 1 : 0;
}
Fraction integer(Integer value) { return {value, 1}; }

Fraction frameTime(const TimelineSequence &sequence, Unsigned frame) {
    if (frame > Unsigned(std::numeric_limits<Integer>::max())) throw Failure("sequence frame exceeds its integer budget");
    auto offset = integer(static_cast<Integer>(frame));
    offset = times(offset, fraction(sequence.editingFrameRate.denominator, sequence.editingFrameRate.numerator));
    offset = times(offset, fraction(sequence.timeBase.denominator, sequence.timeBase.numerator));
    return plus(integer(sequence.startTicks), offset);
}
bool inRange(Fraction time, TimelineTickRange range, bool leftLimit) {
    const auto start = compare(time, integer(range.start));
    const auto end = compare(time, integer(range.start + range.duration));
    return leftLimit ? start > 0 && end <= 0 : start >= 0 && end < 0;
}

struct Position { Fraction tick; bool leftLimit = false; std::optional<exact_timeline::Bezier> bezier; };
using BigRational=exact_timeline::Rational;
using BigInteger=exact_timeline::Integer;
BigRational big(Fraction value) { return exact_timeline::fromFraction(value.n,value.d); }
int comparePosition(const Position &position,const BigRational &value) {
    return position.bezier ? exact_timeline::compare(*position.bezier,value)
                           : exact_timeline::sign(big(position.tick)-value);
}
bool inRangePosition(const Position &position,TimelineTickRange range) {
    const auto start=comparePosition(position,BigRational(range.start));
    const auto end=comparePosition(position,BigRational(range.start)+range.duration);
    return position.leftLimit ? start>0 && end<=0 : start>=0 && end<0;
}
BigInteger ceilPositive(const BigRational &value) {
    if(value<=0)return 0;
    const BigInteger n=numerator(value),d=denominator(value);
    return (n+d-1)/d;
}
BigInteger floorExact(const BigRational &value) {
    const BigInteger n=numerator(value),d=denominator(value);
    BigInteger quotient=n/d;
    if(n<0 && n%d!=0)--quotient;
    return quotient;
}
// Time maps may deliberately travel beyond the source trim when looping is
// enabled. Find the exact cycle, then translate or reflect the original cubic;
// its parameter and target remain unchanged, even for irrational positions.
Position wrapTimeMap(Position position,const TimelineClipProperties &clip) {
    if(clip.loopMode==TimelineLoopMode::None)return position;
    const BigRational start=clip.sourceRange.start,period=clip.sourceRange.duration;
    BigInteger cycle;
    if(position.bezier){
        const auto [minimum,maximum]=std::minmax_element(position.bezier->value.begin(),position.bezier->value.end());
        BigInteger low=floorExact((*minimum-start)/period),high=floorExact((*maximum-start)/period);
        while(low<high){
            const BigInteger middle=low+(high-low+1)/2;
            const BigRational boundary=start+BigRational(middle)*period;
            if(exact_timeline::compare(*position.bezier,boundary)>=0)low=middle;else high=middle-1;
        }
        cycle=low;
    } else cycle=floorExact((big(position.tick)-start)/period);
    const BigRational boundary=start+BigRational(cycle)*period;
    if(position.leftLimit && comparePosition(position,boundary)==0)--cycle;
    const bool reflected=clip.loopMode==TimelineLoopMode::PingPong && cycle%2!=0;
    if(position.bezier){
        if(reflected){
            const BigRational pivot=2*start+(BigRational(cycle)+1)*period;
            for(auto &point:position.bezier->value)point=pivot-point;
        }else{
            const BigRational shift=BigRational(cycle)*period;
            for(auto &point:position.bezier->value)point-=shift;
        }
    } else {
        const BigRational value=reflected ? BigRational(2*start+(BigRational(cycle)+1)*period-big(position.tick))
                                         : BigRational(big(position.tick)-BigRational(cycle)*period);
        const BigRational normalized(numerator(value),denominator(value));
        const BigInteger n=numerator(normalized),d=denominator(normalized);
        if(n<std::numeric_limits<Integer>::min() || n>std::numeric_limits<Integer>::max() ||
           d>std::numeric_limits<Integer>::max())throw Failure("wrapped time exceeds its integer budget");
        position.tick={n.convert_to<Integer>(),d.convert_to<Integer>()};
    }
    if(reflected)position.leftLimit=!position.leftLimit;
    return position;
}
// Select on an exact, bounded constant-rate grid without converting an
// algebraic source time to floating point. Ties for nearest choose earlier.
std::optional<Unsigned> sampleGrid(const Position &position,std::int64_t start,TimelineTimeBase base,
                                   TimelineFrameRate rate,BigInteger count,bool nearest) {
    if(count<=0)return {};
    if(count>BigInteger(std::numeric_limits<Unsigned>::max())) throw Failure("source frame count exceeds its integer budget");
    const Unsigned limit=count.convert_to<Unsigned>();
    const BigRational step=BigRational(base.denominator)*rate.denominator/base.numerator/rate.numerator;
    const auto at=[&](Unsigned index)->BigRational{return BigRational(start)+BigRational(index)*step;};
    if(comparePosition(position,BigRational(start))<0)return {};
    Unsigned low=0,high=limit;
    while(low<high){const Unsigned middle=low+(high-low)/2;const int order=comparePosition(position,at(middle));
        if(order>0 || (order==0 && !position.leftLimit && !nearest))low=middle+1;else high=middle;}
    if(nearest){
        if(!low)return Unsigned(0);if(low==limit)return limit-1;
        return comparePosition(position,(at(low-1)+at(low))/2)<=0 ? low-1 : low;
    }
    if(!low)return {};return low-1;
}
Position mappedPosition(const TimelineProject &project, const TimelineSequence &sequence,
                        const TimelineClipProperties &clip, Position position) {
    auto offset = minus(position.tick, integer(clip.timelineRange.start));
    if(clip.contentWindow)offset=plus(offset,integer(clip.contentWindow->offsetTicks));
    if (!clip.timeMap.empty()) {
        for (std::size_t i = 0; i + 1 < clip.timeMap.size(); ++i) {
            const auto &a = clip.timeMap[i], &b = clip.timeMap[i + 1];
            const auto atStart = compare(offset, integer(a.timelineTicks));
            const auto atEnd = compare(offset, integer(b.timelineTicks));
            if ((atStart > 0 || (atStart == 0 && !position.leftLimit)) &&
                (atEnd < 0 || (atEnd == 0 && position.leftLimit))) {
                if (a.interpolation == TimelineKeyframeInterpolation::Bezier) {
                    auto curve=exact_timeline::makeBezier(a,b,big(offset));
                    const bool left=exact_timeline::leftLimit(curve,position.leftLimit);
                    return wrapTimeMap({{},left,std::move(curve)},clip);
                }
                if (a.interpolation == TimelineKeyframeInterpolation::Hold)
                    return wrapTimeMap({integer(a.sourceTicks), false},clip);
                const auto span = minus(integer(b.sourceTicks), integer(a.sourceTicks));
                const auto t = divide(minus(offset, integer(a.timelineTicks)),
                                      integer(b.timelineTicks - a.timelineTicks));
                return wrapTimeMap({plus(integer(a.sourceTicks), times(span, t)),
                        span.n == 0 ? false : (span.n < 0) != position.leftLimit},clip);
            }
        }
        throw Failure("time-map segment does not contain the requested frame");
    }
    TimelineTimeBase sourceTimeBase;
    std::visit([&](const auto &source) {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, TimelineMediaReference>) {
            const auto *media = findTimelineMediaSource(project, source.mediaSourceId);
            const auto *stream = media ? findTimelineMediaStream(*media, source.streamId) : nullptr;
            if (!stream) throw Failure("clip media stream is missing");
            std::visit([&](const auto &value) { sourceTimeBase = value.timeBase; }, *stream);
        } else if constexpr (std::is_same_v<Source, TimelineSequenceReference>) {
            const auto *child = findTimelineSequence(project, source.sequenceId);
            if (!child) throw Failure("nested sequence is missing");
            sourceTimeBase = child->timeBase;
        } else sourceTimeBase = source.timeBase;
    }, clip.source);
    if (clip.playbackRate.denominator > Unsigned(std::numeric_limits<Integer>::max()) ||
        magnitude(clip.playbackRate.numerator) > Unsigned(std::numeric_limits<Integer>::max()))
        throw Failure("playback rate exceeds its integer budget");
    auto distance = times(offset, fraction(sequence.timeBase.numerator, sequence.timeBase.denominator));
    distance = times(distance, fraction(sourceTimeBase.denominator, sourceTimeBase.numerator));
    distance = times(distance, fraction(static_cast<Integer>(magnitude(clip.playbackRate.numerator)),
                                        static_cast<Integer>(clip.playbackRate.denominator)));
    bool forward = clip.playbackRate.numerator > 0;
    if (clip.loopMode != TimelineLoopMode::None) {
        const Position unwrapped=forward
            ? Position{plus(integer(clip.sourceRange.start),distance),position.leftLimit}
            : Position{minus(integer(clip.sourceRange.start+clip.sourceRange.duration),distance),!position.leftLimit};
        return wrapTimeMap(unwrapped,clip);
    }
    return forward
        ? Position{plus(integer(clip.sourceRange.start), distance), position.leftLimit}
        : Position{minus(integer(clip.sourceRange.start + clip.sourceRange.duration), distance), !position.leftLimit};
}

struct SelectedStream { const TimelineVideoStream *stream = nullptr; std::string representationId; };
SelectedStream selectedStream(const TimelineMediaSource &source, const std::string &streamId) {
    std::vector<const TimelineMediaRepresentation *> ordered;
    const auto append = [&](const std::string &id) {
        const auto *value = findTimelineMediaRepresentation(source, id);
        if (value && std::find(ordered.begin(), ordered.end(), value) == ordered.end()) ordered.push_back(value);
    };
    append(source.activeRepresentationId); append(source.originalRepresentationId);
    for (const auto &representation : source.representations)
        if (std::find(ordered.begin(), ordered.end(), &representation) == ordered.end()) ordered.push_back(&representation);
    for (const auto *representation : ordered) for (const auto &item : representation->streams) {
        const auto *video = std::get_if<TimelineVideoStream>(&item);
        if (video && video->id == streamId) return {video, representation->id};
    }
    throw Failure("video stream is missing from source representations");
}

std::optional<Unsigned> sampleAt(const TimelineVideoStream &stream, Position position,
                                 TimelineVideoSampling sampling, TimelineTickRange clipSourceRange) {
    // A reverse clip begins immediately before its exclusive source end.
    // Do not select a frame at that excluded endpoint just because it is near.
    const bool atExclusiveEnd = position.leftLimit &&
        comparePosition(position,BigRational(clipSourceRange.start)+clipSourceRange.duration) == 0;
    const bool nearest = sampling == TimelineVideoSampling::Nearest && !atExclusiveEnd;
    if (!stream.timing.samples.empty()) {
        // Sample PTS values are strictly ordered by project validation. Find
        // the candidate logarithmically: reverse matching can inspect many
        // sequence frames against a long variable-rate source.
        std::size_t first = 0, last = stream.timing.samples.size();
        while (first < last) {
            const auto middle = first + (last - first) / 2;
            const auto order = comparePosition(position,BigRational(stream.timing.samples[middle].presentationTicks));
            if (order > 0 || (order == 0 && !position.leftLimit && !nearest)) first = middle + 1;
            else last = middle;
        }
        if (nearest) {
            // The next source frame may exist in the stream while lying beyond
            // a trimmed clip. Exclude presentation times at the clip's end.
            std::size_t lower = 0, upper = stream.timing.samples.size();
            const auto end = integer(clipSourceRange.start + clipSourceRange.duration);
            while (lower < upper) {
                const auto middle = lower + (upper - lower) / 2;
                if (compare(integer(stream.timing.samples[middle].presentationTicks), end) < 0) lower = middle + 1;
                else upper = middle;
            }
            if (!lower) return {};
            std::size_t index;
            if (!first) index = 0;
            else if (first == stream.timing.samples.size()) index = first - 1;
            else {
                const BigRational midpoint=(BigRational(stream.timing.samples[first-1].presentationTicks)+
                                            stream.timing.samples[first].presentationTicks)/2;
                index = comparePosition(position,midpoint) <= 0 ? first - 1 : first;
            }
            return Unsigned(std::min(index, lower - 1));
        }
        if (!first) return {};
        const auto index = first - 1;
        const auto &sample = stream.timing.samples[index];
        return inRangePosition(position, {sample.presentationTicks, sample.durationTicks})
            ? std::optional<Unsigned>(index) : std::nullopt;
    }
    if (stream.timing.mode == TimelineFrameRateMode::Variable || !stream.timing.nominal)
        throw Failure("actual source-frame timing is unavailable");
    if(position.bezier){
        BigRational end=BigRational(clipSourceRange.start)+clipSourceRange.duration-stream.startTicks;
        end*=BigRational(stream.timeBase.numerator)/stream.timeBase.denominator;
        end*=BigRational(stream.timing.nominal->numerator)/stream.timing.nominal->denominator;
        auto count=ceilPositive(end);
        if(stream.durationTicks>0){BigRational duration=BigRational(stream.durationTicks)*stream.timeBase.numerator/
                stream.timeBase.denominator*stream.timing.nominal->numerator/stream.timing.nominal->denominator;
            count=std::min(count,ceilPositive(duration));}
        if(stream.frameCount)count=std::min(count,BigInteger(*stream.frameCount));
        return sampleGrid(position,stream.startTicks,stream.timeBase,*stream.timing.nominal,count,nearest);
    }
    auto frame = minus(position.tick, integer(stream.startTicks));
    frame = times(frame, fraction(stream.timeBase.numerator, stream.timeBase.denominator));
    frame = times(frame, fraction(stream.timing.nominal->numerator, stream.timing.nominal->denominator));
    if (frame.n < 0) return {};
    auto index = frame.n / frame.d;
    const auto remainder = frame.n % frame.d;
    if (nearest && remainder > frame.d - remainder) ++index;
    else if (!nearest && position.leftLimit && !remainder) --index;
    if (nearest) {
        auto end = minus(integer(clipSourceRange.start + clipSourceRange.duration), integer(stream.startTicks));
        end = times(end, fraction(stream.timeBase.numerator, stream.timeBase.denominator));
        end = times(end, fraction(stream.timing.nominal->numerator, stream.timing.nominal->denominator));
        if (end.n <= 0) return {};
        const auto lastClipFrame = end.n / end.d - (end.n % end.d == 0 ? 1 : 0);
        index = std::min(index, lastClipFrame);
        std::optional<Unsigned> count = stream.frameCount;
        if (stream.durationTicks > 0) {
            auto frames = times(integer(stream.durationTicks), fraction(stream.timeBase.numerator, stream.timeBase.denominator));
            frames = times(frames, fraction(stream.timing.nominal->numerator, stream.timing.nominal->denominator));
            const auto actual = Unsigned(frames.n / frames.d) + (frames.n % frames.d != 0);
            count = count ? std::min(*count, actual) : actual;
        }
        if (count && *count && Unsigned(index) >= *count) index = static_cast<Integer>(*count - 1);
    }
    if (index < 0 || (stream.frameCount && Unsigned(index) >= *stream.frameCount)) return {};
    return Unsigned(index);
}

// Nested sequences have their own frame grid. Select a child output frame
// before visiting its source clips; otherwise fractional parent positions can
// incorrectly address frames the child composition never emitted.
std::optional<Position> sampleSequence(const TimelineSequence &sequence, Position position,
                                       TimelineVideoSampling sampling, TimelineTickRange sourceRange) {
    if(position.bezier){
        BigRational end=BigRational(sourceRange.start)+sourceRange.duration-sequence.startTicks;
        end*=BigRational(sequence.timeBase.numerator)/sequence.timeBase.denominator;
        end*=BigRational(sequence.editingFrameRate.numerator)/sequence.editingFrameRate.denominator;
        const bool endLimit=position.leftLimit && comparePosition(position,BigRational(sourceRange.start)+sourceRange.duration)==0;
        const auto selected=sampleGrid(position,sequence.startTicks,sequence.timeBase,sequence.editingFrameRate,
                                       ceilPositive(end),sampling==TimelineVideoSampling::Nearest && !endLimit);
        return selected ? std::optional<Position>(Position{frameTime(sequence,*selected),false}) : std::nullopt;
    }
    auto frames = minus(position.tick, integer(sequence.startTicks));
    frames = times(frames, fraction(sequence.timeBase.numerator, sequence.timeBase.denominator));
    frames = times(frames, fraction(sequence.editingFrameRate.numerator, sequence.editingFrameRate.denominator));
    if (frames.n < 0) return {};
    auto index = frames.n / frames.d;
    const auto remainder = frames.n % frames.d;
    if (sampling == TimelineVideoSampling::Nearest && remainder > frames.d - remainder) ++index;
    else if (sampling == TimelineVideoSampling::Previous && position.leftLimit && !remainder) --index;

    // A reference can trim a child to less than its full duration. The frame
    // beginning at the exclusive end must never become visible through it.
    auto end = minus(integer(sourceRange.start + sourceRange.duration), integer(sequence.startTicks));
    end = times(end, fraction(sequence.timeBase.numerator, sequence.timeBase.denominator));
    end = times(end, fraction(sequence.editingFrameRate.numerator, sequence.editingFrameRate.denominator));
    if (end.n <= 0) return {};
    index = std::min(index, end.n / end.d - (end.n % end.d == 0 ? 1 : 0));
    if (index < 0) return {};
    return Position{frameTime(sequence, Unsigned(index)), false};
}

// A sample interval and its original source position are sufficient to
// express the interpolation phase exactly, including irrational Bezier time.
TimelineFrameInterpolation interpolation(const TimelineClipProperties &clip, const Position &position,
                                         Unsigned lower, Unsigned upper, const BigRational &lowerTime,
                                         const BigRational &upperTime, bool isUpper) {
    TimelineFrameInterpolation value;
    value.clipId=clip.id; value.mode=clip.videoSampling;
    value.lowerFrameIndex=lower; value.upperFrameIndex=upper;
    value.lowerSampleTime=exact_timeline::publicRational(lowerTime);
    value.upperSampleTime=exact_timeline::publicRational(upperTime);
    value.sourcePosition={position.tick.n,Unsigned(position.tick.d)};
    if(position.bezier){value.sourcePosition={0,0};value.sourcePositionBezier=exact_timeline::publicExpression(*position.bezier);}
    value.upper=isUpper;
    return value;
}

BigRational streamSampleTime(const TimelineVideoStream &stream, Unsigned index) {
    if(!stream.timing.samples.empty())return BigRational(stream.timing.samples.at(static_cast<size_t>(index)).presentationTicks);
    if(!stream.timing.nominal)throw Failure("actual source-frame timing is unavailable");
    return BigRational(stream.startTicks)+BigRational(index)*stream.timeBase.denominator*
        stream.timing.nominal->denominator/stream.timeBase.numerator/stream.timing.nominal->numerator;
}

bool streamHasSample(const TimelineVideoStream &stream, Unsigned index, const BigRational &sampleTime,
                     TimelineTickRange sourceRange) {
    if(stream.frameCount && index>=*stream.frameCount)return false;
    if(!stream.timing.samples.empty() && index>=stream.timing.samples.size())return false;
    const BigRational end=BigRational(sourceRange.start)+sourceRange.duration;
    if(sampleTime>=end)return false;
    return stream.durationTicks<=0 || sampleTime<BigRational(stream.startTicks)+stream.durationTicks;
}

void walk(const TimelineProject &project, const TimelineSequence &sequence, Position position,
          std::vector<std::string> &path, std::vector<TimelineSourceFrameMatch> &matches,
          std::vector<TimelineFrameInterpolation> &interpolations,
          std::size_t maximumMatches, std::size_t depth) {
    if (depth > 64) throw Failure("nested sequence depth exceeds the traversal budget");
    const bool solo = std::any_of(sequence.tracks.begin(), sequence.tracks.end(), [](const auto &item) {
        const auto *track = std::get_if<TimelineVideoTrack>(&item);
        return track && track->properties.enabled && track->properties.solo;
    });
    for (const auto &item : sequence.tracks) {
        const auto *track = std::get_if<TimelineVideoTrack>(&item);
        if (!track || !track->properties.enabled || (solo && !track->properties.solo)) continue;
        for (const auto &clip : track->clips) {
            if (!clip.properties.enabled || !inRange(position.tick, clip.properties.timelineRange, position.leftLimit)) continue;
            if (std::holds_alternative<TimelineGeneratedReference>(clip.properties.source)) continue;
            const auto mapped = mappedPosition(project, sequence, clip.properties, position);
            // A curved map may overshoot even when both keyframes are within
            // the trim. Such a position has no source sample unless looping
            // brought it back inside the source's half-open interval.
            if(!inRangePosition(mapped,clip.properties.sourceRange))continue;
            const bool interpolated=clip.properties.videoSampling==TimelineVideoSampling::Blend ||
                                    clip.properties.videoSampling==TimelineVideoSampling::OpticalFlow;
            path.push_back(clip.properties.id);
            if (const auto *media = std::get_if<TimelineMediaReference>(&clip.properties.source)) {
                const auto *source = findTimelineMediaSource(project, media->mediaSourceId);
                if (!source) throw Failure("clip media source is missing");
                const auto selected = selectedStream(*source, media->streamId);
                auto samplingPosition=mapped;
                if(interpolated && comparePosition(mapped,BigRational(clip.properties.sourceRange.start)+clip.properties.sourceRange.duration)!=0)
                    samplingPosition.leftLimit=false;
                const auto sampling=interpolated ? TimelineVideoSampling::Previous : clip.properties.videoSampling;
                if (const auto frame = sampleAt(*selected.stream, samplingPosition, sampling,
                                                clip.properties.sourceRange)) {
                    const BigRational lowerTime=streamSampleTime(*selected.stream,*frame);
                    std::optional<BigRational> upperTime;
                    if(interpolated && comparePosition(mapped,lowerTime)>0 && *frame<std::numeric_limits<Unsigned>::max() &&
                       (selected.stream->timing.samples.empty() || *frame+1<selected.stream->timing.samples.size())) {
                        const BigRational candidate=streamSampleTime(*selected.stream,*frame+1);
                        if(streamHasSample(*selected.stream,*frame+1,candidate,clip.properties.sourceRange))upperTime=candidate;
                    }
                    const auto append=[&](Unsigned index,bool upper){
                        if (matches.size() >= maximumMatches) throw Failure("source match budget exceeded");
                        TimelineSourceFrameMatch found{media->mediaSourceId, selected.representationId, media->streamId,
                            index, {mapped.tick.n, Unsigned(mapped.tick.d)}, path};
                        if(mapped.bezier){found.sourcePosition={0,0};found.sourcePositionBezier=exact_timeline::publicExpression(*mapped.bezier);}
                        found.interpolationPath=interpolations;
                        if(upperTime)found.interpolationPath.push_back(interpolation(clip.properties,mapped,*frame,*frame+1,lowerTime,*upperTime,upper));
                        matches.push_back(std::move(found));
                    };
                    append(*frame,false);
                    if(upperTime)append(*frame+1,true);
                }
            } else if (const auto *nested = std::get_if<TimelineSequenceReference>(&clip.properties.source)) {
                const auto *child = findTimelineSequence(project, nested->sequenceId);
                if (!child) throw Failure("nested sequence is missing");
                auto samplingPosition=mapped;
                if(interpolated && comparePosition(mapped,BigRational(clip.properties.sourceRange.start)+clip.properties.sourceRange.duration)!=0)
                    samplingPosition.leftLimit=false;
                const auto sampling=interpolated ? TimelineVideoSampling::Previous : clip.properties.videoSampling;
                if (const auto childFrame = sampleSequence(*child, samplingPosition, sampling, clip.properties.sourceRange)) {
                    const auto frameNumber=times(times(minus(childFrame->tick,integer(child->startTicks)),
                        fraction(child->timeBase.numerator,child->timeBase.denominator)),
                        fraction(child->editingFrameRate.numerator,child->editingFrameRate.denominator));
                    if(frameNumber.n<0 || frameNumber.d!=1)throw Failure("invalid nested frame grid");
                    const auto lower=Unsigned(frameNumber.n);
                    const BigRational lowerTime=big(childFrame->tick);
                    std::optional<Position> upperFrame;
                    if(interpolated && comparePosition(mapped,lowerTime)>0 && lower<Unsigned(std::numeric_limits<Integer>::max())) {
                        auto candidate=Position{frameTime(*child,lower+1),false};
                        if(inRange(candidate.tick,clip.properties.sourceRange,false) &&
                           inRange(candidate.tick,{child->startTicks,child->durationTicks},false))upperFrame=candidate;
                    }
                    if(upperFrame)interpolations.push_back(interpolation(clip.properties,mapped,lower,lower+1,lowerTime,big(upperFrame->tick),false));
                    walk(project,*child,*childFrame,path,matches,interpolations,maximumMatches,depth+1);
                    if(upperFrame){
                        interpolations.back().upper=true;
                        walk(project,*child,*upperFrame,path,matches,interpolations,maximumMatches,depth+1);
                        interpolations.pop_back();
                    }
                }
            }
            path.pop_back();
        }
    }
}

std::vector<TimelineSourceFrameMatch> match(const TimelineProject &project, const TimelineSequence &sequence,
                                            Fraction position, std::size_t maximumMatches) {
    std::vector<TimelineSourceFrameMatch> matches;
    std::vector<std::string> path;
    std::vector<TimelineFrameInterpolation> interpolations;
    walk(project, sequence, {position, false}, path, matches, interpolations, maximumMatches, 0);
    return matches;
}
} // namespace

TimelineSourceFrameResult SourceFrameMatcher::operator()(const TimelineProject &project, const std::string &sequenceId,
    std::uint64_t sequenceFrameIndex, std::size_t maximumMatches) const {
    if (!validateTimelineProject(project).ok()) return {{}, "invalid input project"};
    const auto *sequence = findTimelineSequence(project, sequenceId);
    if (!sequence) return {{}, "unknown sequence"};
    try {
        const auto position = frameTime(*sequence, sequenceFrameIndex);
        if (!inRange(position, {sequence->startTicks, sequence->durationTicks}, false))
            return {{}, "sequence frame lies outside the sequence"};
        return {match(project, *sequence, position, maximumMatches), {}};
    } catch (const Failure &error) { return {{}, error.what()}; }
}

TimelineFrameOccurrencesResult ReverseFrameMatcher::operator()(const TimelineProject &project,
    const std::string &sequenceId, const std::string &mediaSourceId, const std::string &streamId,
    std::uint64_t sourceFrameIndex, std::uint64_t maximumSequenceFrames, std::size_t maximumMatches) const {
    if (!validateTimelineProject(project).ok()) return {{}, "invalid input project"};
    const auto *sequence = findTimelineSequence(project, sequenceId);
    if (!sequence) return {{}, "unknown sequence"};
    if (mediaSourceId.empty() || streamId.empty()) return {{}, "source and stream identities are required"};
    try {
        TimelineFrameOccurrencesResult result;
        for (Unsigned frame = 0;; ++frame) {
            const auto position = frameTime(*sequence, frame);
            if (!inRange(position, {sequence->startTicks, sequence->durationTicks}, false)) break;
            if (frame >= maximumSequenceFrames) return {{}, "sequence frame search budget exceeded"};
            auto candidates = match(project, *sequence, position, maximumMatches);
            for (auto &candidate : candidates) {
                if (candidate.mediaSourceId != mediaSourceId || candidate.streamId != streamId ||
                    candidate.sourceFrameIndex != sourceFrameIndex) continue;
                if (result.occurrences.size() >= maximumMatches) return {{}, "source match budget exceeded"};
                result.occurrences.push_back({frame, std::move(candidate)});
            }
            if (frame == std::numeric_limits<Unsigned>::max()) throw Failure("sequence frame count exceeds its integer budget");
        }
        return result;
    } catch (const Failure &error) { return {{}, error.what()}; }
}
} // namespace iiSharedCanvas
