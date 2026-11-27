#include <iiSharedCanvas>
#include <Timeline/SequenceRasterFlatten.h>
#include <iiFileProviderNative.h>

#include <filesystem>
#include <iostream>

using namespace iiSharedCanvas;
namespace {
int failures=0;
void expect(bool condition,const char *message){if(!condition){std::cerr<<"FAIL: "<<message<<'\n';++failures;}}
Document fixture(){
    Document document;document.extent={2,2};
    TimelineVideoTrack parentTrack;parentTrack.properties.id="parent-track";
    TimelineVideoClip parent;parent.properties.id="parent";
    parent.properties.source=TimelineSequenceReference{"nested",TimelineStreamKind::Video};
    parent.properties.timelineRange={1,3};parent.properties.sourceRange={0,3};
    parent.properties.loopMode=TimelineLoopMode::PingPong;
    parent.properties.timeMap={{0,0},{3,6}};
    TimelineEffect effect;effect.id="animated";effect.pluginId="renderer.example";
    effect.activeRange=TimelineTickRange{0,3};parent.properties.effects.push_back(effect);
    parent.transform.rotationDegrees=17.;
    parentTrack.clips.push_back(parent);
    TimelineSequence sequence;sequence.id="main";sequence.durationTicks=6;
    sequence.timeBase={1,24};sequence.editingFrameRate={24,1};sequence.canvasExtent={2,2};
    sequence.audioSampleRate=48000;sequence.audioChannelLayout="stereo";
    sequence.tracks.emplace_back(parentTrack);
    TimelineSequence nested;nested.id="nested";nested.durationTicks=3;nested.timeBase={1,24};
    nested.editingFrameRate={24,1};nested.canvasExtent={2,2};
    nested.audioSampleRate=48000;nested.audioChannelLayout="stereo";
    for(int i=0;i<2;++i){
        TimelineVideoTrack track;track.properties.id="child-track-"+std::to_string(i);
        TimelineVideoClip clip;clip.properties.id="child-"+std::to_string(i);
        clip.properties.source=TimelineGeneratedReference{"source",TimelineStreamKind::Video,{1,24},{}};
        clip.properties.timelineRange={0,3};clip.properties.sourceRange={0,3};
        track.clips.push_back(clip);nested.tracks.emplace_back(track);
    }
    TimelineAudioClip audio;audio.properties.id="audio";
    audio.properties.source=TimelineGeneratedReference{"tone",TimelineStreamKind::Audio,{1,24},{}};
    audio.properties.timelineRange={0,3};audio.properties.sourceRange={0,3};
    TimelineAudioTrack childAudio;childAudio.properties.id="child-audio";childAudio.clips.push_back(audio);
    nested.tracks.emplace_back(childAudio);
    audio.properties.id="nested-audio";
    audio.properties.source=TimelineSequenceReference{"nested",TimelineStreamKind::Audio};
    audio.properties.timelineRange={1,3};
    TimelineAudioTrack parentAudio;parentAudio.properties.id="parent-audio";parentAudio.clips.push_back(audio);
    sequence.tracks.emplace_back(parentAudio);
    TimelineProject project;project.id="project";project.activeSequenceId="main";
    project.sequences={sequence,nested};document.sequenceProject=std::move(project);
    return document;
}
}
int main(){
    auto document=fixture();
    expect(validate(document).ok(),"nested fixture is valid");
    const auto plan=SequenceRasterFlattenPrepare{}(document,"main","parent",100,16*1024*1024);
    expect(plan.ok() && plan.firstSequenceFrame==1 && plan.frameCount==3 && plan.extent==TimelineExtent{2,2},
           "preparation requests every visible sequence frame and retains the original composition");
    if(!plan.ok())return 1;
    iiFileProvider::native::TemporaryDirectory directory{std::filesystem::path(IISHAREDCANVAS_TEST_OUTPUT_DIR)};
    expect(directory.isValid(),"test temporary storage is available");
    if(!directory.isValid())return 1;
    const auto snapshot=directory.path()/"nested-group.tmp.iisc",note=directory.path()/"nested-group.tmp.md";
    const auto written=SequenceRasterFlattenTemporaryWriter{}(plan,snapshot,note);
    expect(written.ok() && iiFileProvider::native::File::read(snapshot).size()==plan.snapshot.size() &&
           !iiFileProvider::native::File::read(note).empty(),
           "explicit application paths receive the owned group snapshot and temporary group design note");
    expect(!SequenceRasterFlattenTemporaryWriter{}(plan,snapshot,note).ok(),
           "temporary publication never overwrites existing files");
    std::vector<RasterLayer> frames{makeRasterLayer(2,2,0xffff0000),makeRasterLayer(2,2,0xff00ff00),
                                    makeRasterLayer(2,2,0xff0000ff)};
    const auto baked=SequenceRasterFlattenCommit{}(document,plan,frames,{"baked-source","baked-rep","baked-stream","baked-bytes","original-bytes"},1024*1024);
    expect(baked.ok() && baked.project && baked.media.size()==2 && validateTimelineProject(*baked.project).ok(),
           "the complete render becomes an owned media source in a detached project");
    if(!baked.ok())return 1;
    const auto &bakedClip=std::get<TimelineVideoTrack>(baked.project->sequences[0].tracks[0]).clips[0];
    expect(bakedClip.properties.effects.empty() && bakedClip.properties.timeMap.empty() &&
           bakedClip.properties.loopMode==TimelineLoopMode::None && bakedClip.transform.rotationDegrees==0 &&
           bakedClip.editableOriginal && bakedClip.editableOriginal->ownedSnapshotId=="original-bytes",
           "rendered pixels retain a document-owned reference to their editable original");
    expect(std::get<TimelineAudioTrack>(baked.project->sequences[0].tracks[1])==
           std::get<TimelineAudioTrack>(document.sequenceProject->sequences[0].tracks[1]) &&
           std::get<TimelineAudioTrack>(baked.project->sequences[1].tracks[2])==
           std::get<TimelineAudioTrack>(document.sequenceProject->sequences[1].tracks[2]),
           "video rendering keeps original parent and nested audio tracks editable and unchanged");
    const auto frame=RawSequenceFrameReader{}(*baked.media[0].bytes,1,1024);
    expect(frame.ok() && frame.pixels.pixels==frames[1].pixels,
           "the built-in frame reader returns the exact baked ARGB pixels");
    auto changed=document;changed.sequenceProject->name="concurrent change";
    expect(!SequenceRasterFlattenCommit{}(changed,plan,frames,{"baked-source","baked-rep","baked-stream","baked-bytes","original-bytes"},1024*1024).ok(),
           "a concurrently changed project is rejected without publication");
    expect(!SequenceRasterFlattenCommit{}(document,plan,frames,{"baked-source","baked-rep","baked-stream","baked-bytes","original-bytes"},4).ok(),
           "a frame-byte budget failure returns no partial project");
    auto corrupted=plan;corrupted.snapshot={1,2,3};
    expect(!SequenceRasterFlattenCommit{}(document,corrupted,frames,{"baked-source","baked-rep","baked-stream","baked-bytes","original-bytes"},1024*1024).ok(),
           "a corrupted editable snapshot cannot be published with the rendered frames");
    DocumentHistory history(document);
    expect(history.setSequenceProject(*baked.project,baked.media).changed && history.undo().changed &&
           document.sequenceProject==fixture().sequenceProject && history.redo().changed,
           "publication, undo and redo use one document-history entry");
    const auto saved=encodeIisc(document);const auto reopened=saved.ok()?decodeIisc(saved.bytes):IiscDecodeResult{};
    expect(saved.ok() && reopened.ok() && reopened.document.sequenceProject==document.sequenceProject &&
           reopened.document.sequenceMedia==document.sequenceMedia,
           "flattened owned frames survive IISC serialization");
    const auto original=SequenceRasterFlattenOriginal{}(reopened.document,"main","parent");
    expect(original.ok() && original.document.sequenceProject==fixture().sequenceProject,
           "the full editable source can be reopened directly from the saved document without temporary files");
    if(original.ok()){
        auto edited=original.document;edited.sequenceProject->name="edited original";
        std::get<TimelineVideoTrack>(edited.sequenceProject->sequences[0].tracks[0]).clips[0].properties.effects[0]
            .parameters.push_back({"generation-strength",0.75});
        const auto replacement=SequenceRasterFlattenReplaceOriginal{}(reopened.document,"main","parent",edited,16*1024*1024);
        expect(replacement.ok(),"an edited original can replace the owned snapshot while preserving the current rendered clip");
        if(replacement.ok()){
            auto revised=reopened.document;revised.sequenceProject=*replacement.project;revised.sequenceMedia=replacement.media;
            const auto reopenedEdit=SequenceRasterFlattenOriginal{}(revised,"main","parent");
            expect(reopenedEdit.ok() && reopenedEdit.document.sequenceProject->name=="edited original" &&
                   std::get<TimelineVideoTrack>(reopenedEdit.document.sequenceProject->sequences[0].tracks[0]).clips[0]
                       .properties.effects[0].parameters[0].value==TimelinePropertyValue{0.75},
                   "reopening the flattened clip returns the latest editable original");
            const auto replan=SequenceRasterFlattenPrepare{}(revised,"main","parent",100,16*1024*1024);
            expect(replan.ok(),"a flattened clip with an edited original can be prepared for rendering again");
            if(replan.ok()){
                auto rerender=frames;rerender[1]=makeRasterLayer(2,2,0xffffff00);
                const auto updated=SequenceRasterFlattenCommit{}(revised,replan,rerender,
                    {"baked-source-2","baked-rep-2","baked-stream-2","baked-bytes-2","original-bytes"},1024*1024);
                expect(updated.ok(),"rerendering replaces the derived frames while retaining the editable original");
            }
        }
    }
    auto fractional=fixture();
    for(auto &sequence:fractional.sequenceProject->sequences){
        sequence.timeBase={1,25};sequence.editingFrameRate={24,1};
        for(auto &track:sequence.tracks)std::visit([](auto &typed){for(auto &clip:typed.clips)
            if(auto *generated=std::get_if<TimelineGeneratedReference>(&clip.properties.source))generated->timeBase={1,25};},track);
    }
    const auto exact=SequenceRasterFlattenPrepare{}(fractional,"main","parent",100,16*1024*1024);
    expect(exact.ok() && exact.outputTimeBase==TimelineTimeBase{1,600} && exact.firstOffsetTicks==1 &&
           exact.frameStepTicks==25 && exact.outputDurationTicks==72,
           "fractional timeline-to-frame boundaries produce an exact common output time base");
    if(exact.ok()){
        const auto rendered=SequenceRasterFlattenCommit{}(fractional,exact,frames,{"s","r","v","owned","original"},1024*1024);
        const auto match=rendered.ok()?SourceFrameMatcher{}(*rendered.project,"main",1):TimelineSourceFrameResult{};
        expect(rendered.ok() && match.ok() && match.matches.size()==1 && match.matches[0].sourceFrameIndex==0,
               "a baked frame remains addressable at the original fractional frame boundary");
    }
    auto transitionDocument=fixture();
    auto &transitionSequence=transitionDocument.sequenceProject->sequences[0];
    auto &transitionTrack=std::get<TimelineVideoTrack>(transitionSequence.tracks[0]);
    auto &transitionParent=transitionTrack.clips[0];
    transitionParent.properties.loopMode=TimelineLoopMode::None;transitionParent.properties.timeMap.clear();
    transitionParent.properties.effects.clear();transitionParent.transform={};
    auto &transitionChild=transitionDocument.sequenceProject->sequences[1];transitionChild.durationTicks=5;
    for(auto &track:transitionChild.tracks)std::visit([](auto &typed){for(auto &clip:typed.clips){
        clip.properties.timelineRange.duration=5;clip.properties.sourceRange.duration=5;
    }},track);
    TimelineVideoClip following;following.properties.id="following";
    following.properties.source=TimelineGeneratedReference{"next",TimelineStreamKind::Video,{1,24},{}};
    following.properties.timelineRange={4,2};following.properties.sourceRange={0,2};
    transitionTrack.clips.push_back(following);
    TimelineTransition transition;transition.id="cross";transition.typeId="builtin.cross-dissolve";
    transition.fromClipId="parent";transition.toClipId="following";transition.timelineRange={3,2};
    transition.alignment=TimelineTransitionAlignment::CenteredOnCut;transitionTrack.transitions.push_back(transition);
    const auto handles=SequenceRasterFlattenPrepare{}(transitionDocument,"main","parent",10,16*1024*1024);
    expect(handles.ok() && handles.firstSequenceFrame==1 && handles.frameCount==4 &&
           handles.outputDurationTicks==4 && handles.visibleSourceRange==TimelineTickRange{0,3},
           "flatten preparation requests transition-handle frames beyond the visible clip");
    if(handles.ok()){
        auto handleFrames=frames;handleFrames.push_back(makeRasterLayer(2,2,0xffabcdef));
        const auto result=SequenceRasterFlattenCommit{}(transitionDocument,handles,handleFrames,
            {"handle-source","handle-rep","handle-stream","handle-bytes","handle-original"},1024*1024);
        expect(result.ok() && result.project &&
               std::get<TimelineVideoTrack>(result.project->sequences[0].tracks[0]).transitions[0]==transition &&
               std::get<TimelineVideoTrack>(result.project->sequences[0].tracks[0]).clips[0].properties.sourceRange==TimelineTickRange{0,3},
               "baked media retains transition handles while the original transition remains editable");
        if(result.ok()){
            auto withoutTransition=*result.project;
            std::get<TimelineVideoTrack>(withoutTransition.sequences[0].tracks[0]).transitions.clear();
            expect(TimelineTransitionPlacement{}(withoutTransition,"main","parent-track",transition).ok(),
                   "the baked stream exposes real source handles for later transition edits");
        }
    }
    auto incomingDocument=transitionDocument;
    auto &incomingTrack=std::get<TimelineVideoTrack>(incomingDocument.sequenceProject->sequences[0].tracks[0]);
    incomingTrack.clips[0].properties.timelineRange={2,3};
    incomingTrack.clips[0].properties.sourceRange={1,3};
    incomingTrack.clips[1].properties.timelineRange={0,2};
    std::swap(incomingTrack.clips[0],incomingTrack.clips[1]);
    auto &incoming=incomingTrack.transitions[0];
    incoming.fromClipId="following";incoming.toClipId="parent";incoming.timelineRange={1,2};
    const auto incomingHandles=SequenceRasterFlattenPrepare{}(incomingDocument,"main","parent",10,16*1024*1024);
    expect(incomingHandles.ok() && incomingHandles.firstSequenceFrame==1 && incomingHandles.frameCount==4 &&
           incomingHandles.outputDurationTicks==4 && incomingHandles.visibleSourceRange==TimelineTickRange{1,3},
           "flatten preparation includes incoming frames before the visible clip");
    if(incomingHandles.ok()){
        auto handleFrames=frames;handleFrames.push_back(makeRasterLayer(2,2,0xffabcdef));
        const auto result=SequenceRasterFlattenCommit{}(incomingDocument,incomingHandles,handleFrames,
            {"incoming-source","incoming-rep","incoming-stream","incoming-bytes","incoming-original"},1024*1024);
        expect(result.ok() && result.project &&
               std::get<TimelineVideoTrack>(result.project->sequences[0].tracks[0]).transitions[0]==incoming &&
               std::get<TimelineVideoTrack>(result.project->sequences[0].tracks[0]).clips[1].properties.sourceRange==TimelineTickRange{1,3},
               "baked media retains incoming handles without shifting the visible content");
        if(result.ok()){
            auto withoutTransition=*result.project;
            std::get<TimelineVideoTrack>(withoutTransition.sequences[0].tracks[0]).transitions.clear();
            expect(TimelineTransitionPlacement{}(withoutTransition,"main","parent-track",incoming).ok(),
                   "incoming source handles remain usable for later transition edits");
        }
    }
    Document huge;huge.extent={1,1};
    TimelineSequence longSequence;longSequence.id="long";longSequence.timeBase={1,1};
    longSequence.editingFrameRate={3,1};longSequence.durationTicks=6148914691236517206LL;
    longSequence.canvasExtent={1,1};
    TimelineSequence shortSource;shortSource.id="short";shortSource.timeBase={1,1};
    shortSource.editingFrameRate={3,1};shortSource.durationTicks=1;shortSource.canvasExtent={1,1};
    TimelineVideoTrack longTrack;longTrack.properties.id="long-track";
    TimelineVideoClip last;last.properties.id="last";last.properties.source=TimelineSequenceReference{"short",TimelineStreamKind::Video};
    last.properties.timelineRange={6148914691236517205LL,1};last.properties.sourceRange={0,1};
    longTrack.clips.push_back(last);longSequence.tracks.emplace_back(longTrack);
    TimelineProject longProject;longProject.id="long-project";longProject.activeSequenceId="long";
    longProject.sequences={longSequence,shortSource};huge.sequenceProject=longProject;
    expect(validate(huge).ok() && !SequenceRasterFlattenPrepare{}(huge,"long","last",3,1024*1024).ok(),
           "preparation rejects a visible frame range whose final frame index exceeds 64 bits");
    return failures?1:0;
}
