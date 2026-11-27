#include "Timeline/SequenceRasterFlatten.h"
#include "Serialization/IiscCodec.h"
#include "Timeline/ExactBezier_p.hpp"
#include "Validation/Validation.h"
#include <iiFileProviderNative.h>

#include <algorithm>
#include <array>
#include <limits>
#include <new>

namespace iiSharedCanvas {
namespace {
using Big=exact_timeline::Integer;
Big gcd(Big a,Big b){while(b!=0){Big next=a%b;a=std::move(b);b=std::move(next);}return a;}
Big ceilDiv(const Big &a,const Big &b){return (a+b-1)/b;}
bool fits(const Big &value,std::uint64_t max){return value>=0 && value<=max;}
const TimelineVideoClip *findVideo(const TimelineSequence &sequence,const std::string &id,bool &locked,
                                   const TimelineVideoTrack **owner=nullptr){
    for(const auto &item:sequence.tracks)if(const auto *track=std::get_if<TimelineVideoTrack>(&item))
        for(const auto &clip:track->clips)if(clip.properties.id==id){locked=track->properties.locked;if(owner)*owner=track;return &clip;}
    return nullptr;
}
TimelineVideoClip *findVideo(TimelineSequence &sequence,const std::string &id){
    for(auto &item:sequence.tracks)if(auto *track=std::get_if<TimelineVideoTrack>(&item))
        for(auto &clip:track->clips)if(clip.properties.id==id)return &clip;
    return nullptr;
}
void put32(std::vector<std::uint8_t> &bytes,std::uint32_t value){for(unsigned n=0;n<4;++n)bytes.push_back(std::uint8_t(value>>(8*n)));}
void put64(std::vector<std::uint8_t> &bytes,std::uint64_t value){for(unsigned n=0;n<8;++n)bytes.push_back(std::uint8_t(value>>(8*n)));}
std::uint32_t get32(std::span<const std::uint8_t> bytes,std::size_t at){std::uint32_t n=0;for(unsigned i=0;i<4;++i)n|=std::uint32_t(bytes[at+i])<<(8*i);return n;}
std::uint64_t get64(std::span<const std::uint8_t> bytes,std::size_t at){std::uint64_t n=0;for(unsigned i=0;i<8;++i)n|=std::uint64_t(bytes[at+i])<<(8*i);return n;}
constexpr std::array<std::uint8_t,8> magic{'I','I','S','A','R','G','B','1'};
constexpr std::size_t headerSize=24;
IiscDecodeResult decodeOriginal(const Document &document,const TimelineEditableVideoSource &reference,SerializationLimits limits){
    const auto fail=[](std::string message){return IiscDecodeResult{{},{IiscErrorCode::InvalidData,0,std::move(message)}};};
    const auto source=std::find_if(document.sequenceMedia.begin(),document.sequenceMedia.end(),
        [&](const auto &media){return media.id==reference.ownedSnapshotId;});
    if(source==document.sequenceMedia.end() || !source->bytes || source->mediaType!="application/vnd.iisacc.ii-shared-canvas")
        return fail("the editable original is not a document-owned IISC snapshot");
    auto result=decodeIisc(*source->bytes,limits);
    if(!result.ok())return result;
    const auto *sequence=result.document.sequenceProject?findTimelineSequence(*result.document.sequenceProject,reference.sequenceId):nullptr;
    bool locked=false;const auto *clip=sequence?findVideo(*sequence,reference.clipId,locked):nullptr;
    if(!clip || !std::holds_alternative<TimelineSequenceReference>(clip->properties.source))
        return fail("the snapshot no longer contains the original nested video clip");
    return result;
}
}

SequenceRasterFlattenPreparation SequenceRasterFlattenPrepare::operator()(const Document &document,
    const std::string &sequenceId,const std::string &clipId,std::uint64_t maximumFrames,std::uint64_t maximumSnapshotBytes) const {
    SequenceRasterFlattenPreparation result;
    const auto fail=[&](std::string message){result.error=std::move(message);return result;};
    if(!document.sequenceProject || !validate(document).ok())return fail("invalid document or missing sequence project");
    const auto *sequence=findTimelineSequence(*document.sequenceProject,sequenceId);
    const TimelineVideoTrack *owner=nullptr;
    bool locked=false;const auto *clip=sequence?findVideo(*sequence,clipId,locked,&owner):nullptr;
    if(!clip || locked || (!std::holds_alternative<TimelineSequenceReference>(clip->properties.source) && !clip->editableOriginal))
        return fail("an unlocked nested video clip is required");
    try {
        const auto &base=sequence->timeBase;const auto &fps=sequence->editingFrameRate;
        const Big denominator=Big(base.denominator)*fps.denominator;
        const Big rate=Big(base.numerator)*fps.numerator;
        const auto clipStart=clip->properties.timelineRange.start;
        const auto clipEnd=clipStart+clip->properties.timelineRange.duration;
        auto renderStart=clipStart,renderEnd=clipEnd;
        for(const auto &transition:owner->transitions){
            if(transition.toClipId==clipId)renderStart=std::min(renderStart,transition.timelineRange.start);
            if(transition.fromClipId==clipId)renderEnd=std::max(renderEnd,transition.timelineRange.start+transition.timelineRange.duration);
        }
        const Big start=Big(renderStart)-sequence->startTicks;
        const Big first=ceilDiv(start*rate,denominator);
        const Big end=ceilDiv((Big(renderEnd)-sequence->startTicks)*rate,denominator);
        const Big count=end-first;
        if(count<=0 || !fits(first,std::numeric_limits<std::uint64_t>::max()) ||
           !fits(end-1,std::numeric_limits<std::uint64_t>::max()) || !fits(count,maximumFrames) || count>1000000)
            return fail("clip frame count is empty or exceeds the frame budget");
        // The common tick divides both a sequence tick and a frame period.
        const Big commonDen=Big(base.denominator)/gcd(base.denominator,fps.numerator)*fps.numerator;
        const Big timelineUnit=Big(base.numerator)*(commonDen/base.denominator);
        const Big frameUnit=Big(fps.denominator)*(commonDen/fps.numerator);
        const Big unit=gcd(timelineUnit,frameUnit);
        const Big commonDivisor=gcd(unit,commonDen);
        const Big outputNum=unit/commonDivisor,outputDen=commonDen/commonDivisor;
        const Big timelineTicks=timelineUnit/unit,frameTicks=frameUnit/unit;
        const Big duration=(Big(renderEnd)-renderStart)*timelineTicks;
        const Big visibleStart=(Big(clipStart)-renderStart)*timelineTicks;
        const Big visibleDuration=Big(clip->properties.timelineRange.duration)*timelineTicks;
        const Big firstOffset=first*frameTicks-start*timelineTicks;
        const auto max64=std::uint64_t(std::numeric_limits<std::int64_t>::max());
        if(!fits(outputNum,std::numeric_limits<std::uint32_t>::max()) ||
           !fits(outputDen,std::numeric_limits<std::uint32_t>::max()) ||
           !fits(duration,max64) || !fits(visibleStart,max64) || !fits(visibleDuration,max64) ||
           !fits(frameTicks,max64) || !fits(firstOffset,max64) ||
           firstOffset>=duration)return fail("exact output frame grid exceeds the timeline format");
        SerializationLimits limits;limits.maximumContainerBytes=maximumSnapshotBytes;
        std::vector<std::uint8_t> snapshot;
        std::string sourceSequence=sequenceId,sourceClip=clipId;
        if(clip->editableOriginal){
            const auto &reference=*clip->editableOriginal;
            auto original=decodeOriginal(document,reference,limits);
            if(!original.ok())return fail("cannot reopen editable source: "+original.error.message);
            const auto *originalSequence=findTimelineSequence(*original.document.sequenceProject,reference.sequenceId);
            bool ignored=false;const auto *originalClip=findVideo(*originalSequence,reference.clipId,ignored);
            if(originalSequence->timeBase!=sequence->timeBase || originalSequence->editingFrameRate!=sequence->editingFrameRate ||
               originalSequence->startTicks!=sequence->startTicks || originalSequence->canvasExtent!=sequence->canvasExtent ||
               originalSequence->pixelAspectRatio!=sequence->pixelAspectRatio || originalSequence->color!=sequence->color ||
               originalClip->properties.timelineRange!=clip->properties.timelineRange)
                return fail("edited source timing or extent must match the rendered clip before rerendering");
            const auto media=std::find_if(document.sequenceMedia.begin(),document.sequenceMedia.end(),
                [&](const auto &item){return item.id==reference.ownedSnapshotId;});
            snapshot=*media->bytes;sourceSequence=reference.sequenceId;sourceClip=reference.clipId;
        }else{
            auto encoded=encodeIisc(document,limits);
            if(!encoded.ok())return fail("cannot create bounded source snapshot: "+encoded.error.message);
            snapshot=std::move(encoded.bytes);
        }
        result.sequenceId=sequenceId;result.clipId=clipId;result.expectedProject=*document.sequenceProject;
        result.sourceSequenceId=std::move(sourceSequence);result.sourceClipId=std::move(sourceClip);
        result.expectedMedia=document.sequenceMedia;result.snapshot=std::move(snapshot);
        result.extent=sequence->canvasExtent;result.frameRate=fps;
        result.outputTimeBase={outputNum.convert_to<std::uint32_t>(),outputDen.convert_to<std::uint32_t>()};
        result.outputDurationTicks=duration.convert_to<std::int64_t>();
        result.visibleSourceRange={visibleStart.convert_to<std::int64_t>(),visibleDuration.convert_to<std::int64_t>()};
        result.firstOffsetTicks=firstOffset.convert_to<std::int64_t>();
        result.frameStepTicks=frameTicks.convert_to<std::int64_t>();
        result.firstSequenceFrame=first.convert_to<std::uint64_t>();result.frameCount=count.convert_to<std::uint64_t>();
        return result;
    }catch(const std::bad_alloc &){return fail("insufficient memory to prepare raster flattening");}
}

SequenceRasterFlattenWriteResult SequenceRasterFlattenTemporaryWriter::operator()(const SequenceRasterFlattenPreparation &plan,
    const std::filesystem::path &snapshotPath,const std::filesystem::path &notePath) const {
    SequenceRasterFlattenWriteResult result;
    if(!plan.ok() || plan.snapshot.empty() || snapshotPath.empty() || notePath.empty() || snapshotPath==notePath){
        result.error="a prepared group and two distinct application-owned paths are required";return result;
    }
    try {
        const std::string note="# Temporary nested track group design\n\n"
            "Preserve editable child tracks, their order, composition boundary, time maps, effects and source ownership. "
            "This is a deferred design, not the selected flattening implementation. The selected implementation renders "
            "the isolated nested clip to document-owned frames; the accompanying IISC snapshot retains its source.\n";
        iiFileProvider::native::File::create(notePath,note);result.noteWritten=true;
        const auto bytes=std::string_view(reinterpret_cast<const char *>(plan.snapshot.data()),plan.snapshot.size());
        iiFileProvider::native::File::create(snapshotPath,bytes);result.snapshotWritten=true;
    }catch(const std::exception &error){result.error=error.what();}
    return result;
}

SequenceRasterFlattenResult SequenceRasterFlattenCommit::operator()(const Document &document,
    const SequenceRasterFlattenPreparation &plan,std::span<const RasterLayer> frames,
    const SequenceRasterFlattenIds &ids,std::uint64_t maximumBytes) const {
    const auto fail=[](std::string message){return SequenceRasterFlattenResult{{},{},std::move(message)};};
    if(!plan.ok() || !document.sequenceProject || *document.sequenceProject!=plan.expectedProject || document.sequenceMedia!=plan.expectedMedia)
        return fail("the source project or owned media changed after preparation");
    const auto *currentSequence=findTimelineSequence(*document.sequenceProject,plan.sequenceId);
    bool locked=false;const auto *currentClip=currentSequence?findVideo(*currentSequence,plan.clipId,locked):nullptr;
    if(!currentClip || locked || plan.sourceSequenceId.empty() || plan.sourceClipId.empty() || plan.snapshot.empty())
        return fail("the prepared clip or editable source is unavailable");
    SerializationLimits sourceLimits;
    sourceLimits.maximumContainerBytes=std::max<std::uint64_t>(sourceLimits.maximumContainerBytes,plan.snapshot.size());
    const auto snapshot=decodeIisc(plan.snapshot,sourceLimits);
    const auto *sourceSequence=snapshot.ok() && snapshot.document.sequenceProject ?
        findTimelineSequence(*snapshot.document.sequenceProject,plan.sourceSequenceId):nullptr;
    bool sourceLocked=false;const auto *sourceClip=sourceSequence?findVideo(*sourceSequence,plan.sourceClipId,sourceLocked):nullptr;
    if(!sourceClip || !std::holds_alternative<TimelineSequenceReference>(sourceClip->properties.source) ||
       sourceSequence->timeBase!=currentSequence->timeBase || sourceSequence->editingFrameRate!=currentSequence->editingFrameRate ||
       sourceSequence->startTicks!=currentSequence->startTicks || sourceSequence->canvasExtent!=currentSequence->canvasExtent ||
       sourceSequence->pixelAspectRatio!=currentSequence->pixelAspectRatio || sourceSequence->color!=currentSequence->color ||
       sourceClip->properties.timelineRange!=currentClip->properties.timelineRange)
        return fail("the editable source snapshot is invalid or no longer matches the rendered clip");
    const bool reuseOriginal=currentClip && currentClip->editableOriginal &&
        currentClip->editableOriginal->ownedSnapshotId==ids.originalSnapshotMedia;
    const auto existingOriginal=std::find_if(document.sequenceMedia.begin(),document.sequenceMedia.end(),
        [&](const auto &media){return media.id==ids.originalSnapshotMedia;});
    if(ids.source.empty() || ids.representation.empty() || ids.stream.empty() || ids.ownedMedia.empty() ||
       ids.originalSnapshotMedia.empty() || ids.originalSnapshotMedia==ids.ownedMedia ||
       findTimelineMediaSource(*document.sequenceProject,ids.source) ||
       std::any_of(document.sequenceMedia.begin(),document.sequenceMedia.end(),[&](const auto &m){
           return m.id==ids.ownedMedia || (!reuseOriginal && m.id==ids.originalSnapshotMedia);
       }) || (reuseOriginal && (existingOriginal==document.sequenceMedia.end() || !existingOriginal->bytes ||
                               *existingOriginal->bytes!=plan.snapshot)))
        return fail("new media identities must be nonempty and unique");
    if(frames.size()!=plan.frameCount || !plan.frameCount || plan.extent.width<=0 || plan.extent.height<=0 ||
       plan.outputDurationTicks<=0 || plan.frameStepTicks<=0 || plan.firstOffsetTicks<0 ||
       plan.visibleSourceRange.start<0 || plan.visibleSourceRange.duration<=0 ||
       plan.visibleSourceRange.start>plan.outputDurationTicks ||
       plan.visibleSourceRange.duration>plan.outputDurationTicks-plan.visibleSourceRange.start)
        return fail("rendered frames do not match the prepared grid");
    const auto pixels=std::uint64_t(plan.extent.width)*std::uint64_t(plan.extent.height);
    if(maximumBytes<headerSize || pixels>(maximumBytes-headerSize)/4 ||
       plan.frameCount>(maximumBytes-headerSize)/(pixels*4))return fail("rendered frame byte budget exceeded");
    if(plan.frameCount>(std::numeric_limits<std::size_t>::max()-headerSize)/(pixels*4))
        return fail("rendered frame storage exceeds this platform's address space");
    for(const auto &frame:frames)if(frame.width!=plan.extent.width || frame.height!=plan.extent.height || frame.pixels.size()!=pixels)
        return fail("all rendered frames must match the prepared canvas extent");
    try {
        auto bytes=std::make_shared<std::vector<std::uint8_t>>();
        bytes->reserve(headerSize+std::size_t(plan.frameCount*pixels*4));
        bytes->insert(bytes->end(),magic.begin(),magic.end());put32(*bytes,std::uint32_t(plan.extent.width));
        put32(*bytes,std::uint32_t(plan.extent.height));put64(*bytes,plan.frameCount);
        for(const auto &frame:frames)for(const auto pixel:frame.pixels)put32(*bytes,pixel);
        auto project=*document.sequenceProject;
        auto *sequence=findTimelineSequence(project,plan.sequenceId);
        auto *clip=sequence?findVideo(*sequence,plan.clipId):nullptr;
        if(!clip || (!std::holds_alternative<TimelineSequenceReference>(clip->properties.source) && !clip->editableOriginal))
            return fail("nested clip is no longer available");
        TimelineVideoStream stream;stream.id=ids.stream;stream.timeBase=plan.outputTimeBase;
        stream.durationTicks=plan.outputDurationTicks;stream.codec.identifier="iisacc.raw-argb32";
        stream.codedExtent=stream.displayExtent=plan.extent;stream.pixelAspectRatio=sequence->pixelAspectRatio;
        stream.timing.nominal=plan.frameRate;stream.pixelFormat="argb32";stream.bitDepth=8;
        stream.alphaMode=TimelineAlphaMode::Straight;stream.color=sequence->color;stream.frameCount=plan.frameCount;
        stream.timing.samples.reserve(frames.size());
        for(std::uint64_t i=0;i<plan.frameCount;++i){
            const Big at=Big(plan.firstOffsetTicks)+Big(i)*plan.frameStepTicks;
            if(!fits(at,std::uint64_t(plan.outputDurationTicks-1)))return fail("rendered sample exceeds the prepared duration");
            const auto tick=at.convert_to<std::int64_t>();
            stream.timing.samples.push_back({tick,{},std::min(plan.frameStepTicks,plan.outputDurationTicks-tick),true});
        }
        TimelineMediaRepresentation representation;representation.id=ids.representation;
        representation.ownedMediaId=ids.ownedMedia;representation.fileSizeBytes=bytes->size();
        representation.container.identifier="iisacc.raw-argb32-sequence";representation.container.mimeType="application/vnd.iisacc.argb32-sequence";
        representation.timeBase=plan.outputTimeBase;representation.durationTicks=plan.outputDurationTicks;
        representation.streams.emplace_back(std::move(stream));
        TimelineMediaSource source;source.id=ids.source;
        source.originalRepresentationId=source.activeRepresentationId=ids.representation;
        source.representations.push_back(std::move(representation));project.mediaSources.push_back(std::move(source));
        // Pushing media can reallocate the project; reacquire sequence and clip.
        sequence=findTimelineSequence(project,plan.sequenceId);clip=sequence?findVideo(*sequence,plan.clipId):nullptr;
        auto &p=clip->properties;
        const auto offset=p.contentWindow ? p.contentWindow->offsetTicks : 0;
        std::vector<TimelineMarker> markers;
        for(auto marker:p.markers){
            const Big first=Big(marker.range.start)-offset,last=first+marker.range.duration;
            const Big visibleStart=std::max(Big(0),first),visibleEnd=std::min(Big(p.timelineRange.duration),last);
            if((marker.range.duration==0 && first>=0 && first<=p.timelineRange.duration) || visibleEnd>visibleStart){
                marker.range.start=visibleStart.convert_to<std::int64_t>();
                marker.range.duration=(visibleEnd-visibleStart).convert_to<std::int64_t>();markers.push_back(std::move(marker));
            }
        }
        p.markers=std::move(markers);p.source=TimelineMediaReference{ids.source,ids.stream};
        p.sourceRange=plan.visibleSourceRange;p.playbackRate={1,1};p.loopMode=TimelineLoopMode::None;
        p.videoSampling=TimelineVideoSampling::Previous;p.timeMap.clear();p.effects.clear();p.contentWindow.reset();
        const auto blend=clip->transform.blendMode;clip->transform=TimelineTransform{};clip->transform.blendMode=blend;clip->crop=TimelineCrop{};
        clip->editableOriginal=TimelineEditableVideoSource{ids.originalSnapshotMedia,plan.sourceSequenceId,plan.sourceClipId};
        auto media=document.sequenceMedia;
        media.push_back({ids.ownedMedia,"application/vnd.iisacc.argb32-sequence",std::move(bytes)});
        if(!reuseOriginal)media.push_back({ids.originalSnapshotMedia,"application/vnd.iisacc.ii-shared-canvas",
                                          std::make_shared<const std::vector<std::uint8_t>>(plan.snapshot)});
        if(!validateTimelineProject(project).ok())return fail("rendered output cannot be represented as a valid timeline");
        return {std::move(project),std::move(media),{}};
    }catch(const std::bad_alloc &){return fail("insufficient memory for rendered frames");}
}

IiscDecodeResult SequenceRasterFlattenOriginal::operator()(const Document &document,const std::string &sequenceId,
    const std::string &clipId,SerializationLimits limits) const {
    const auto fail=[](std::string message){return IiscDecodeResult{{},{IiscErrorCode::InvalidData,0,std::move(message)}};};
    const auto *sequence=document.sequenceProject?findTimelineSequence(*document.sequenceProject,sequenceId):nullptr;
    bool locked=false;const auto *clip=sequence?findVideo(*sequence,clipId,locked):nullptr;
    if(!clip || !clip->editableOriginal)return fail("the clip has no editable original");
    return decodeOriginal(document,*clip->editableOriginal,limits);
}

SequenceRasterFlattenResult SequenceRasterFlattenReplaceOriginal::operator()(const Document &document,
    const std::string &sequenceId,const std::string &clipId,const Document &edited,std::uint64_t maximumSnapshotBytes) const {
    const auto fail=[](std::string message){return SequenceRasterFlattenResult{{},{},std::move(message)};};
    const auto *sequence=document.sequenceProject?findTimelineSequence(*document.sequenceProject,sequenceId):nullptr;
    bool locked=false;const auto *clip=sequence?findVideo(*sequence,clipId,locked):nullptr;
    if(!clip || locked || !clip->editableOriginal || !edited.sequenceProject)return fail("an unlocked rendered clip and edited source are required");
    const auto &reference=*clip->editableOriginal;
    const auto *originalSequence=findTimelineSequence(*edited.sequenceProject,reference.sequenceId);
    const auto *originalClip=originalSequence?findVideo(*originalSequence,reference.clipId,locked):nullptr;
    if(!originalClip || !std::holds_alternative<TimelineSequenceReference>(originalClip->properties.source))
        return fail("the edited source must retain its original nested clip identity");
    SerializationLimits limits;limits.maximumContainerBytes=maximumSnapshotBytes;
    auto snapshot=encodeIisc(edited,limits);
    if(!snapshot.ok())return fail("cannot encode edited source: "+snapshot.error.message);
    try{
        auto media=document.sequenceMedia;
        const auto target=std::find_if(media.begin(),media.end(),[&](const auto &item){return item.id==reference.ownedSnapshotId;});
        if(target==media.end() || target->mediaType!="application/vnd.iisacc.ii-shared-canvas")
            return fail("the editable original is not document-owned");
        target->bytes=std::make_shared<const std::vector<std::uint8_t>>(std::move(snapshot.bytes));
        return {*document.sequenceProject,std::move(media),{}};
    }catch(const std::bad_alloc &){return fail("insufficient memory to replace the editable source");}
}

RawSequenceFrameResult RawSequenceFrameReader::operator()(std::span<const std::uint8_t> bytes,
    std::uint64_t frame,std::size_t maximumPixels) const {
    const auto fail=[](std::string message){return RawSequenceFrameResult{{},std::move(message)};};
    if(bytes.size()<headerSize || !std::equal(magic.begin(),magic.end(),bytes.begin()))return fail("invalid raw frame-sequence header");
    const auto width=get32(bytes,8),height=get32(bytes,12);const auto count=get64(bytes,16);
    if(!width || !height || width>std::uint32_t(std::numeric_limits<std::int32_t>::max()) ||
       height>std::uint32_t(std::numeric_limits<std::int32_t>::max()) || frame>=count)return fail("invalid frame identity or dimensions");
    const std::uint64_t pixels=std::uint64_t(width)*height;
    if(pixels>maximumPixels || pixels>(bytes.size()-headerSize)/4 || count!=(bytes.size()-headerSize)/(pixels*4) ||
       (bytes.size()-headerSize)%(pixels*4))return fail("raw frame payload is malformed or exceeds its pixel budget");
    try {
        RasterLayer result{static_cast<std::int32_t>(width),static_cast<std::int32_t>(height),{}};
        result.pixels.reserve(std::size_t(pixels));const auto offset=headerSize+std::size_t(frame*pixels*4);
        for(std::size_t i=0;i<pixels;++i)result.pixels.push_back(get32(bytes,offset+i*4));
        return {std::move(result),{}};
    }catch(const std::bad_alloc &){return fail("insufficient memory to decode frame");}
}
} // namespace iiSharedCanvas
