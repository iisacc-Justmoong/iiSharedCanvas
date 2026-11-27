// Included after CanvasControlBridge.cpp's private handle and helpers. The C
// ABI borrows all inputs for a call; only the document/history own results.
namespace {
const TimelineProject &sequenceProject(const IiscCanvasControl &canvas) {
    if (!canvas.document.sequenceProject) throw std::invalid_argument("document has no sequence project");
    return *canvas.document.sequenceProject;
}
void publishSequence(IiscCanvasControl &canvas, TimelineProjectPlan plan, const char *label) {
    if (!plan.ok()) throw std::runtime_error(plan.error);
    accepted(canvas.history.setSequenceProject(std::move(*plan.project), canvas.document.sequenceMedia, label));
}
std::vector<TimelineSplitTarget> splitTargets(const IiscSequencePair *pairs, size_t count) {
    if ((!pairs && count) || count > 65536) throw std::invalid_argument("invalid split targets");
    std::vector<TimelineSplitTarget> result; result.reserve(count);
    for (size_t i = 0; i < count; ++i) result.push_back({requiredString(pairs[i].left), requiredString(pairs[i].right)});
    return result;
}
std::vector<TimelineSplitLink> splitLinks(const IiscSequencePair *pairs, size_t count) {
    if ((!pairs && count) || count > 65536) throw std::invalid_argument("invalid split links");
    std::vector<TimelineSplitLink> result; result.reserve(count);
    for (size_t i = 0; i < count; ++i) result.push_back({requiredString(pairs[i].left), requiredString(pairs[i].right)});
    return result;
}
void sequenceJsonExactRational(std::string &out,const TimelineExactRational &value) {
    out += "{\"numerator\":"; jsonString(out, value.numerator);
    out += ",\"denominator\":"; jsonString(out, value.denominator); out += '}';
}
void sequenceJsonBezier(std::string &out,const std::optional<TimelineBezierSourcePosition> &curve) {
    if(!curve){out += "null";return;}
    const auto controls = [&](const auto &values) {
        out += '[';
        for (size_t i = 0; i < values.size(); ++i) { if (i) out += ','; sequenceJsonExactRational(out,values[i]); }
        out += ']';
    };
    out += "{\"time_control\":"; controls(curve->timeControl);
    out += ",\"value_control\":"; controls(curve->valueControl);
    out += ",\"target\":"; sequenceJsonExactRational(out,curve->target); out += '}';
}
void sourceMatchJson(std::string &out, const TimelineSourceFrameMatch &match) {
    out += "{\"source\":"; jsonString(out, match.mediaSourceId);
    out += ",\"representation\":"; jsonString(out, match.representationId);
    out += ",\"stream\":"; jsonString(out, match.streamId);
    out += ",\"source_frame\":" + std::to_string(match.sourceFrameIndex);
    out += ",\"source_tick_num\":" + std::to_string(match.sourcePosition.numerator);
    out += ",\"source_tick_den\":" + std::to_string(match.sourcePosition.denominator);
    out += ",\"source_tick_bezier\":"; sequenceJsonBezier(out,match.sourcePositionBezier);
    out += ",\"path\":[";
    for (size_t i = 0; i < match.clipPath.size(); ++i) { if (i) out += ','; jsonString(out, match.clipPath[i]); }
    out += "],\"interpolation\":[";
    for(size_t i=0;i<match.interpolationPath.size();++i){
        if(i)out+=',';
        const auto &step=match.interpolationPath[i];
        out += "{\"clip\":";jsonString(out,step.clipId);
        out += ",\"mode\":";jsonString(out,step.mode==TimelineVideoSampling::Blend ? "blend" : "optical_flow");
        out += ",\"lower_frame\":"+std::to_string(step.lowerFrameIndex)+",\"upper_frame\":"+std::to_string(step.upperFrameIndex);
        out += ",\"lower_time\":";sequenceJsonExactRational(out,step.lowerSampleTime);
        out += ",\"upper_time\":";sequenceJsonExactRational(out,step.upperSampleTime);
        out += ",\"source_tick_num\":"+std::to_string(step.sourcePosition.numerator);
        out += ",\"source_tick_den\":"+std::to_string(step.sourcePosition.denominator);
        out += ",\"source_tick_bezier\":";sequenceJsonBezier(out,step.sourcePositionBezier);
        out += step.upper ? ",\"upper\":true}" : ",\"upper\":false}";
    }
    out += "]}";
}
void sequenceJsonDouble(std::string &out, double value) {
    std::array<char, 64> buffer{};
    const auto converted = std::to_chars(buffer.data(), buffer.data()+buffer.size(), value,
                                         std::chars_format::general, std::numeric_limits<double>::max_digits10);
    if (converted.ec != std::errc{}) throw std::runtime_error("cannot encode sequence number");
    out.append(buffer.data(), converted.ptr);
    if (std::find_if(buffer.data(),converted.ptr,[](char c){return c=='.'||c=='e'||c=='E';})==converted.ptr)
        out+=".0";
}
// These projections retain every TimelinePropertyValue alternative. The C
// inputs are borrowed; the native values returned here own all text and arrays.
TimelineProperty sequenceProperty(const IiscSequenceProperty &input) {
    TimelineProperty value; value.key=requiredString(input.key);
    switch (input.kind) {
    case 0: value.value=std::monostate{}; break;
    case 1: value.value=boolean(input.boolean_value); break;
    case 2: value.value=input.integer_value; break;
    case 3: value.value=input.components[0]; break;
    case 4: if (!input.text) throw std::invalid_argument("property text is required"); value.value=std::string(input.text); break;
    case 5: value.value=TimelineRational{input.integer_value,input.denominator}; break;
    case 6: value.value=TimelineColor{input.components[0],input.components[1],input.components[2],input.components[3]}; break;
    case 7: value.value=TimelineVector2{input.components[0],input.components[1]}; break;
    case 8:
        if ((!input.numbers && input.number_count) || input.number_count>1000000)
            throw std::invalid_argument("invalid property number array");
        value.value=input.number_count ? std::vector<double>(input.numbers,input.numbers+input.number_count)
                                       : std::vector<double>{};
        break;
    default: throw std::invalid_argument("unknown timeline property kind");
    }
    return value;
}
TimelineSubtitleStyle captionStyle(const IiscSequenceCaptionStyle &input) {
    if ((!input.properties && input.property_count) || input.property_count>65536)
        throw std::invalid_argument("invalid caption style properties");
    const auto color=[](const double *c) { return TimelineColor{c[0],c[1],c[2],c[3]}; };
    TimelineSubtitleStyle style;
    if (!input.font_family || !input.horizontal_alignment || !input.vertical_alignment || !input.writing_direction)
        throw std::invalid_argument("caption style strings are required");
    style.fontFamily=input.font_family; style.fontSize=input.font_size;
    style.fill=color(input.fill); style.outline=color(input.outline); style.background=color(input.background);
    style.outlineWidth=input.outline_width; style.bold=boolean(input.bold); style.italic=boolean(input.italic);
    style.underline=boolean(input.underline); style.horizontalAlignment=input.horizontal_alignment;
    style.verticalAlignment=input.vertical_alignment; style.writingDirection=input.writing_direction;
    for (size_t i=0;i<input.property_count;++i) style.properties.push_back(sequenceProperty(input.properties[i]));
    return style;
}
void colorJson(std::string &out,const TimelineColor &c) {
    out+='['; sequenceJsonDouble(out,c.red); out+=','; sequenceJsonDouble(out,c.green);
    out+=','; sequenceJsonDouble(out,c.blue); out+=','; sequenceJsonDouble(out,c.alpha); out+=']';
}
void captionStyleJson(std::string &out,const TimelineSubtitleStyle &style) {
    out+="{\"font_family\":"; jsonString(out,style.fontFamily); out+=",\"font_size\":"; sequenceJsonDouble(out,style.fontSize);
    out+=",\"fill\":"; colorJson(out,style.fill); out+=",\"outline\":"; colorJson(out,style.outline);
    out+=",\"outline_width\":"; sequenceJsonDouble(out,style.outlineWidth);
    out+=",\"background\":"; colorJson(out,style.background);
    out+=",\"bold\":"; out+=style.bold?"true":"false"; out+=",\"italic\":"; out+=style.italic?"true":"false";
    out+=",\"underline\":"; out+=style.underline?"true":"false";
    out+=",\"horizontal_alignment\":"; jsonString(out,style.horizontalAlignment);
    out+=",\"vertical_alignment\":"; jsonString(out,style.verticalAlignment);
    out+=",\"writing_direction\":"; jsonString(out,style.writingDirection); out+=",\"properties\":[";
    constexpr const char *kinds[]={"null","bool","int","number","string","rational","color","vector2","numbers"};
    for(size_t i=0;i<style.properties.size();++i) {
        const auto &property=style.properties[i]; if(i) out+=','; out+="{\"key\":"; jsonString(out,property.key);
        out+=",\"type\":"; jsonString(out,kinds[property.value.index()]); out+=",\"value\":";
        std::visit([&](const auto &item) {
            using T=std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T,std::monostate>) out+="null";
            else if constexpr (std::is_same_v<T,bool>) out+=item?"true":"false";
            else if constexpr (std::is_same_v<T,std::int64_t>) out+=std::to_string(item);
            else if constexpr (std::is_same_v<T,double>) sequenceJsonDouble(out,item);
            else if constexpr (std::is_same_v<T,std::string>) jsonString(out,item);
            else if constexpr (std::is_same_v<T,TimelineRational>) out+='['+std::to_string(item.numerator)+','+std::to_string(item.denominator)+']';
            else if constexpr (std::is_same_v<T,TimelineColor>) colorJson(out,item);
            else if constexpr (std::is_same_v<T,TimelineVector2>) { out+='[';sequenceJsonDouble(out,item.x);out+=',';sequenceJsonDouble(out,item.y);out+=']'; }
            else { out+='[';for(size_t j=0;j<item.size();++j){if(j)out+=',';sequenceJsonDouble(out,item[j]);}out+=']'; }
        },property.value);
        out+='}';
    }
    out+="]}";
}
} // namespace

extern "C" {
int iisc_canvas_sequence_create(IiscCanvasControl *canvas,const IiscSequenceCreate *value) {
    return invoke(canvas,[&](auto &owner) {
        if (!value) throw std::invalid_argument("sequence settings are required");
        if (owner.document.sequenceProject) throw std::invalid_argument("document already has a sequence project");
        TimelineProject project;
        project.id=requiredString(value->project_id);
        TimelineSequence sequence;
        sequence.id=requiredString(value->sequence_id);
        sequence.name=value->name ? value->name : "";
        sequence.durationTicks=value->duration;
        sequence.timeBase={value->time_num,value->time_den};
        sequence.editingFrameRate={value->rate_num,value->rate_den};
        sequence.canvasExtent={value->width,value->height};
        project.activeSequenceId=sequence.id;
        project.sequences.push_back(std::move(sequence));
        accepted(owner.history.setSequenceProject(std::move(project),{},"Create sequence project"));
    });
}
int iisc_canvas_sequence_copy(IiscCanvasControl *canvas,const IiscCanvasControl *source,int replace) {
    return invoke(canvas,[&](auto &owner) {
        if (!source) throw std::invalid_argument("source document is required");
        const bool canReplace=boolean(replace);
        if (owner.document.sequenceProject && !canReplace)
            throw std::invalid_argument("document already has a sequence project");
        if (!source->document.sequenceProject) throw std::invalid_argument("source has no sequence project");
        accepted(owner.history.setSequenceProject(*source->document.sequenceProject,source->document.sequenceMedia,
                                                   "Copy sequence project"));
    });
}
int iisc_canvas_sequence_snapshot(IiscCanvasControl *canvas,uint8_t *output,size_t capacity,size_t *required) {
    return invoke(canvas,[&](auto &owner) {
        const auto &project = sequenceProject(owner);
        std::string out = "{\"id\":"; jsonString(out,project.id); out += ",\"sequences\":[";
        for (size_t i=0; i<project.sequences.size(); ++i) {
            const auto &sequence=project.sequences[i]; if (i) out+=',';
            out+="{\"id\":"; jsonString(out,sequence.id); out+=",\"name\":"; jsonString(out,sequence.name);
            out+=",\"start\":"+std::to_string(sequence.startTicks)+",\"duration\":"+std::to_string(sequence.durationTicks)+",\"timecode\":";
            if (sequence.timecode) {
                const auto &tc=*sequence.timecode;
                out+="{\"start_frame\":"+std::to_string(tc.startFrame)+",\"frame_rate\":["+
                     std::to_string(tc.frameRate.numerator)+','+std::to_string(tc.frameRate.denominator)+
                     "],\"nominal_fps\":"+std::to_string(tc.nominalFramesPerSecond)+",\"counting\":";
                jsonString(out,tc.counting==TimelineTimecodeCounting::DropFrame?"drop_frame":"non_drop_frame");
                out+=",\"wrap_24_hours\":";out+=tc.wrapsAt24Hours?"true}":"false}";
            } else out+="null";
            out+=",\"tracks\":[";
            for (size_t j=0; j<sequence.tracks.size(); ++j) {
                const auto &track=sequence.tracks[j]; const auto &properties=timelineTrackProperties(track); if (j) out+=',';
                out+="{\"id\":"; jsonString(out,properties.id);
                out+=",\"kind\":"+std::to_string(static_cast<int>(timelineTrackKind(track)))+
                     ",\"muted\":"+(properties.muted ? "true" : "false")+
                     ",\"locked\":"+(properties.locked ? "true" : "false")+",\"language\":";
                jsonString(out,properties.language); out+=",\"clips\":[";
                std::visit([&](const auto &typed) {
                    for (size_t k=0; k<typed.clips.size(); ++k) {
                        const auto &clip=typed.clips[k]; const auto &p=clip.properties; if (k) out+=',';
                        out+="{\"id\":"; jsonString(out,p.id);
                        out+=",\"start\":"+std::to_string(p.timelineRange.start)+",\"duration\":"+std::to_string(p.timelineRange.duration)+
                             ",\"effects\":"+std::to_string(p.effects.size());
                        if constexpr (std::is_same_v<std::decay_t<decltype(clip)>,TimelineSubtitleClip>) {
                            out+=",\"text\":"; jsonString(out,clip.text);
                            out+=",\"style\":"; captionStyleJson(out,clip.style);
                        }
                        out+='}';
                    }
                },track);
                out+="]}";
            }
            out+="]}";
        }
        out+="]}"; jsonOutput(out,output,capacity,required);
    });
}

int iisc_canvas_sequence_settings(IiscCanvasControl *canvas,const char *sequence,const IiscSequenceSettings *value) {
    return invoke(canvas,[&](auto &owner) {
        if (!value || (value->flags & ~127U)) throw std::invalid_argument("invalid sequence settings");
        SequenceSettingsChange change;
        if (value->flags&1U) change.name=requiredString(value->name);
        if (value->flags&2U) change.editingFrameRate=TimelineFrameRate{value->rate_num,value->rate_den};
        if (value->flags&4U) change.canvasExtent=TimelineExtent{value->width,value->height};
        if (value->flags&8U) change.pixelAspectRatio=TimelineRational{value->aspect_num,value->aspect_den};
        if (value->flags&16U) change.audioSampleRate=value->audio_rate;
        if (value->flags&32U) change.audioChannelLayout=requiredString(value->audio_layout);
        if (value->flags&64U) {
            if (boolean(value->timecode_present)) {
                if (value->timecode_counting<0 || value->timecode_counting>1)
                    throw std::invalid_argument("invalid timecode counting mode");
                TimelineTimecode tc; tc.startFrame=value->timecode_start_frame;
                tc.frameRate={value->timecode_rate_num,value->timecode_rate_den};
                tc.nominalFramesPerSecond=value->timecode_nominal_fps;
                tc.counting=static_cast<TimelineTimecodeCounting>(value->timecode_counting);
                tc.wrapsAt24Hours=boolean(value->timecode_wrap_24_hours);
                change.timecode.emplace(std::move(tc));
            } else change.timecode.emplace(std::nullopt);
        }
        publishSequence(owner,SequenceSettingsEditor{}(sequenceProject(owner),requiredString(sequence),change),"Sequence settings");
    });
}
int iisc_canvas_sequence_tracks(IiscCanvasControl *canvas,const char *sequence,const char *const *ids,size_t count,
                                const IiscSequenceTrackChanges *value) {
    return invoke(canvas,[&](auto &owner) {
        if (!value || (value->flags & ~1023U)) throw std::invalid_argument("invalid track changes");
        TimelineTrackChanges change;
        if (value->flags&1U) change.enabled=boolean(value->enabled);
        if (value->flags&2U) change.locked=boolean(value->locked);
        if (value->flags&4U) change.muted=boolean(value->muted);
        if (value->flags&8U) change.solo=boolean(value->solo);
        if (value->flags&16U) change.syncLocked=boolean(value->sync_locked);
        if (value->flags&32U) change.role=requiredString(value->role);
        if (value->flags&64U) change.language=requiredString(value->language);
        if (value->flags&128U) change.blendMode=requiredString(value->blend_mode);
        if (value->flags&256U) change.outputBusId=requiredString(value->output_bus);
        if (value->flags&512U) change.opacity=value->opacity;
        publishSequence(owner,TimelineTrackBatchEdit{}(sequenceProject(owner),requiredString(sequence),layerIds(ids,count),change),"Track batch edit");
    });
}
int iisc_canvas_sequence_add_track(IiscCanvasControl *canvas,const char *sequence,const char *id,int kind) {
    return invoke(canvas,[&](auto &owner) {
        auto project=sequenceProject(owner); auto *target=findTimelineSequence(project,requiredString(sequence));
        if (!target || kind<0 || kind>3) throw std::invalid_argument("invalid sequence or track kind");
        const std::string trackId=requiredString(id);
        switch (kind) {
        case 0: { TimelineVideoTrack track; track.properties.id=trackId; target->tracks.emplace_back(std::move(track)); break; }
        case 1: { TimelineAudioTrack track; track.properties.id=trackId; target->tracks.emplace_back(std::move(track)); break; }
        case 2: { TimelineSubtitleTrack track; track.properties.id=trackId; target->tracks.emplace_back(std::move(track)); break; }
        case 3: { TimelineDataTrack track; track.properties.id=trackId; target->tracks.emplace_back(std::move(track)); break; }
        }
        const auto validation=validateTimelineProject(project);
        if (!validation.ok()) throw std::invalid_argument(validation.issues.front().message);
        accepted(owner.history.setSequenceProject(std::move(project),owner.document.sequenceMedia,"Add sequence track"));
    });
}
int iisc_canvas_sequence_split(IiscCanvasControl *canvas,const char *sequence,const IiscSequencePair *pairs,size_t count,
                               const IiscSequencePair *links,size_t linkCount,int64_t cut) {
    return invoke(canvas,[&](auto &owner) {
        publishSequence(owner,TimelineSplitPlan{}(sequenceProject(owner),requiredString(sequence),splitTargets(pairs,count),splitLinks(links,linkCount),cut),"Split clips");
    });
}
int iisc_canvas_sequence_trim(IiscCanvasControl *canvas,const char *sequence,const IiscSequenceClipRange *values,size_t count) {
    return invoke(canvas,[&](auto &owner) {
        if (!values || !count || count>65536) throw std::invalid_argument("invalid trim targets");
        std::vector<TimelineClipRangeTarget> targets; targets.reserve(count);
        for (size_t i=0;i<count;++i) targets.push_back({requiredString(values[i].id),{values[i].start,values[i].duration}});
        publishSequence(owner,TimelineTrimPlan{}(sequenceProject(owner),requiredString(sequence),targets),"Trim clips");
    });
}
int iisc_canvas_sequence_extend(IiscCanvasControl *canvas,const char *sequence,const char *const *ids,size_t count,int edge,int64_t playhead) {
    return invoke(canvas,[&](auto &owner) {
        if (edge<0 || edge>1) throw std::invalid_argument("invalid clip edge");
        publishSequence(owner,TimelineExtendToPlayheadPlan{}(sequenceProject(owner),requiredString(sequence),layerIds(ids,count),
            static_cast<TimelineClipEdge>(edge),playhead),"Extend clips");
    });
}
int iisc_canvas_sequence_range(IiscCanvasControl *canvas,int operation,const char *sequence,const char *const *ids,size_t count,
                               int64_t start,int64_t duration,const char *prefix,const char *newId) {
    return invoke(canvas,[&](auto &owner) {
        const auto &project=sequenceProject(owner); const std::string sequenceId=requiredString(sequence);
        const TimelineTickRange range{start,duration}; TimelineProjectPlan plan;
        switch (operation) {
        case 0: plan=TimelineLiftPlan{}(project,sequenceId,layerIds(ids,count),range,requiredString(prefix)); break;
        case 1: plan=TimelineExtractPlan{}(project,sequenceId,layerIds(ids,count),range,requiredString(prefix)); break;
        case 2: plan=TimelineGapClosePlan{}(project,sequenceId,layerIds(ids,count),range); break;
        case 3: case 4: plan=SubsequenceExtractor{}(project,sequenceId,range,requiredString(newId),
                                      operation==3 ? SubsequenceMode::Copy : SubsequenceMode::Move,requiredString(prefix)); break;
        default: throw std::invalid_argument("unknown sequence range operation");
        }
        publishSequence(owner,std::move(plan),"Sequence range edit");
    });
}
int iisc_canvas_sequence_simplify(IiscCanvasControl *canvas,const char *sequence,uint32_t flags,const char *newId) {
    return invoke(canvas,[&](auto &owner) {
        if (flags & ~15U) throw std::invalid_argument("invalid simplification options");
        SequenceSimplificationOptions options;
        options.removeEmptyTracks=flags&1U; options.removeDisabledClips=flags&2U;
        options.removeGaps=flags&4U; options.flattenNestedSequences=flags&8U;
        if (newId) options.outputSequenceId=newId;
        publishSequence(owner,SequenceSimplificationPlan{}(sequenceProject(owner),requiredString(sequence),options),"Simplify sequence");
    });
}
int iisc_canvas_sequence_caption_ex(IiscCanvasControl *canvas,const char *sequence,const char *trackId,int action,const char *id,
    const char *text,const char *markup,int64_t start,int64_t duration,const char *language,const char *font,double fontSize,
    const IiscSequenceCaptionStyle *style) {
    return invoke(canvas,[&](auto &owner) {
        const auto &project=sequenceProject(owner); const auto *seq=findTimelineSequence(project,requiredString(sequence));
        const auto *entry=seq ? findTimelineTrack(*seq,requiredString(trackId)) : nullptr;
        const auto *track=entry ? std::get_if<TimelineSubtitleTrack>(entry) : nullptr;
        if (!track || action<0 || action>2) throw std::invalid_argument("invalid caption track or operation");
        CaptionTrackChange change; change.action=static_cast<CaptionTrackAction>(action);
        if (language) change.language=language;
        if (action==0) {
            const std::string clipId=requiredString(id);
            const auto existing=std::find_if(track->clips.begin(),track->clips.end(),[&](const auto &clip){return clip.properties.id==clipId;});
            if (existing!=track->clips.end()) change.clip=*existing;
            else { change.clip.properties.id=clipId; change.clip.properties.source=TimelineGeneratedReference{"builtin.caption",TimelineStreamKind::Subtitle,seq->timeBase,{}}; }
            change.clip.properties.timelineRange={start,duration};
            if (std::holds_alternative<TimelineGeneratedReference>(change.clip.properties.source))
                change.clip.properties.sourceRange={0,duration};
            if (text) change.clip.text=text; if (markup) change.clip.markup=markup;
            if (font) { change.clip.style.fontFamily=font; change.clip.style.fontSize=fontSize; }
            if (style) change.clip.style=captionStyle(*style);
        } else if (style) {
            throw std::invalid_argument("caption style is valid only for an upsert");
        } else if (action==1) change.clipId=requiredString(id);
        publishSequence(owner,CaptionTrackEditor{}(project,seq->id,requiredString(trackId),change),"Edit captions");
    });
}
int iisc_canvas_sequence_caption(IiscCanvasControl *canvas,const char *sequence,const char *trackId,int action,const char *id,
    const char *text,const char *markup,int64_t start,int64_t duration,const char *language,const char *font,double fontSize) {
    return iisc_canvas_sequence_caption_ex(canvas,sequence,trackId,action,id,text,markup,start,duration,language,font,fontSize,nullptr);
}
int iisc_canvas_sequence_transition(IiscCanvasControl *canvas,const char *sequence,const char *track,const char *id,
    const char *type,const char *from,const char *to,int64_t start,int64_t duration,int alignment) {
    return invoke(canvas,[&](auto &owner) {
        if (alignment<0 || alignment>3) throw std::invalid_argument("invalid transition alignment");
        TimelineTransition transition; transition.id=requiredString(id); transition.typeId=requiredString(type);
        if (from) transition.fromClipId=from; if (to) transition.toClipId=to;
        transition.timelineRange={start,duration}; transition.alignment=static_cast<TimelineTransitionAlignment>(alignment);
        publishSequence(owner,TimelineTransitionPlacement{}(sequenceProject(owner),requiredString(sequence),requiredString(track),transition),"Place transition");
    });
}
int iisc_canvas_sequence_reframe(IiscCanvasControl *canvas,const char *sequence,const char *clip,const char *effect,
    const IiscSequenceKeyframe *keys,size_t keyCount,const IiscSequenceObservation *observations,size_t observationCount,
    const char *selected,int32_t sourceWidth,int32_t sourceHeight,int32_t targetWidth,int32_t targetHeight,double padding) {
    return invoke(canvas,[&](auto &owner) {
        if ((!keys && keyCount) || (!observations && observationCount) || keyCount>1000000 || observationCount>1000000)
            throw std::invalid_argument("invalid reframe input arrays");
        AutoReframeRequest request; request.effectId=requiredString(effect);
        if (selected) request.selectedTrackId=selected;
        request.sourceExtent={sourceWidth,sourceHeight}; request.targetExtent={targetWidth,targetHeight}; request.paddingFraction=padding;
        for (size_t i=0;i<keyCount;++i) request.keyframes.push_back({keys[i].time,{keys[i].center_x,keys[i].center_y},keys[i].scale});
        for (size_t i=0;i<observationCount;++i) request.observations.push_back({observations[i].time,requiredString(observations[i].track_id),
            {observations[i].x,observations[i].y},{observations[i].width,observations[i].height}});
        publishSequence(owner,AutoReframeKeyframeApplier{}(sequenceProject(owner),requiredString(sequence),requiredString(clip),request),"Auto reframe");
    });
}
int iisc_canvas_sequence_effect_time(IiscCanvasControl *canvas,const char *sequence,const char *trackId,
                                     int64_t numerator,uint64_t denominator,int *present,int64_t *outNumerator,uint64_t *outDenominator) {
    return invoke(canvas,[&](auto &owner){
        if(!present || !outNumerator || !outDenominator)throw std::invalid_argument("effect time output is required");
        const auto *seq=findTimelineSequence(sequenceProject(owner),requiredString(sequence));
        const auto *track=seq?findTimelineTrack(*seq,requiredString(trackId)):nullptr;
        if(!track)throw std::invalid_argument("unknown sequence or track");
        const auto result=TrackEffectTimeMapper{}(timelineTrackProperties(*track),{numerator,denominator});
        if(!result.ok())throw std::invalid_argument(result.error);
        *present=result.sourceTime.has_value();
        if(result.sourceTime){*outNumerator=result.sourceTime->numerator;*outDenominator=result.sourceTime->denominator;}
    });
}
int iisc_canvas_sequence_gaps(IiscCanvasControl *canvas,const char *sequence,const char *const *ids,size_t count,
                              uint8_t *output,size_t capacity,size_t *required) {
    return invoke(canvas,[&](auto &owner) {
        const auto *seq=findTimelineSequence(sequenceProject(owner),requiredString(sequence));
        if (!seq) throw std::invalid_argument("unknown sequence");
        const auto result=TimelineGapIndex{}(*seq,layerIds(ids,count));
        if (!result.ok()) throw std::runtime_error(result.error);
        std::string out="[";
        for(size_t i=0;i<result.gaps.size();++i) { const auto &gap=result.gaps[i]; if(i)out+=',';
            out+="{\"track\":";jsonString(out,gap.trackId);out+=",\"start\":"+std::to_string(gap.range.start)+",\"duration\":"+std::to_string(gap.range.duration)+"}"; }
        out+=']'; jsonOutput(out,output,capacity,required);
    });
}
int iisc_canvas_sequence_through(IiscCanvasControl *canvas,const char *sequence,const char *const *ids,size_t count,
                                 uint8_t *output,size_t capacity,size_t *required) {
    return invoke(canvas,[&](auto &owner) {
        const auto result=ThroughEditDetector{}(sequenceProject(owner),requiredString(sequence),layerIds(ids,count));
        if (!result.ok()) throw std::runtime_error(result.error);
        std::string out="[";
        for(size_t i=0;i<result.edits.size();++i) {const auto &edit=result.edits[i];if(i)out+=',';
            out+="{\"track\":";jsonString(out,edit.trackId);out+=",\"from\":";jsonString(out,edit.fromClipId);
            out+=",\"to\":";jsonString(out,edit.toClipId);out+=",\"cut\":"+std::to_string(edit.cutTicks)+"}";}
        out+=']';jsonOutput(out,output,capacity,required);
    });
}
int iisc_canvas_sequence_default(IiscCanvasControl *canvas,int kind,int64_t duration,const char *type,
                                 uint8_t *output,size_t capacity,size_t *required) {
    return invoke(canvas,[&](auto &) {
        if(kind<0 || kind>3)throw std::invalid_argument("invalid stream kind");
        const auto result=DefaultTransitionPolicy{}(static_cast<TimelineStreamKind>(kind),duration,type?type:"");
        if(!result.ok())throw std::runtime_error(result.error);
        std::string out="{\"type\":";jsonString(out,result.typeId);out+=",\"duration\":"+std::to_string(result.durationTicks)+"}";
        jsonOutput(out,output,capacity,required);
    });
}
int iisc_canvas_sequence_match(IiscCanvasControl *canvas,const char *sequence,uint64_t frame,
                               uint8_t *output,size_t capacity,size_t *required) {
    return invoke(canvas,[&](auto &owner) {
        const auto result=SourceFrameMatcher{}(sequenceProject(owner),requiredString(sequence),frame);
        if(!result.ok())throw std::runtime_error(result.error);
        std::string out="[";for(size_t i=0;i<result.matches.size();++i){if(i)out+=',';sourceMatchJson(out,result.matches[i]);}
        out+=']';jsonOutput(out,output,capacity,required);
    });
}
int iisc_canvas_sequence_reverse_match(IiscCanvasControl *canvas,const char *sequence,const char *source,const char *stream,
    uint64_t sourceFrame,uint64_t budget,uint8_t *output,size_t capacity,size_t *required) {
    return invoke(canvas,[&](auto &owner) {
        const auto result=ReverseFrameMatcher{}(sequenceProject(owner),requiredString(sequence),requiredString(source),requiredString(stream),sourceFrame,budget);
        if(!result.ok())throw std::runtime_error(result.error);
        std::string out="[";for(size_t i=0;i<result.occurrences.size();++i){if(i)out+=',';
            out+="{\"sequence_frame\":"+std::to_string(result.occurrences[i].sequenceFrameIndex)+",\"source\":";
            sourceMatchJson(out,result.occurrences[i].source);out+='}';}
        out+=']';jsonOutput(out,output,capacity,required);
    });
}
int iisc_canvas_sequence_track_subjects(IiscCanvasControl *canvas,const IiscSequenceDetection *values,size_t count,
    const char *prefix,int64_t maximumGap,double minimumConfidence,double minimumOverlap,
    uint8_t *output,size_t capacity,size_t *required) {
    return invoke(canvas,[&](auto &) {
        if ((!values && count) || count>100000) throw std::invalid_argument("invalid detection array");
        std::vector<AutoReframeDetection> detections; detections.reserve(count);
        for (size_t i=0;i<count;++i) detections.push_back({values[i].time,values[i].class_id,
            requiredString(values[i].label),values[i].confidence,{values[i].x,values[i].y},{values[i].width,values[i].height}});
        AutoReframeTrackingOptions options; options.idPrefix=requiredString(prefix); options.maximumGapTicks=maximumGap;
        options.minimumConfidence=minimumConfidence; options.minimumOverlap=minimumOverlap;
        const auto result=AutoReframeSubjectTracker{}(detections,options);
        if (!result.ok()) throw std::runtime_error(result.error);
        std::string out="[";
        for (size_t i=0;i<result.tracks.size();++i) {
            const auto &track=result.tracks[i]; if(i)out+=',';
            out+="{\"id\":";jsonString(out,track.id);out+=",\"class_id\":"+std::to_string(track.classId)+",\"label\":";
            jsonString(out,track.label);out+=",\"observations\":[";
            for (size_t j=0;j<track.observations.size();++j) {
                const auto &item=track.observations[j];if(j)out+=',';
                out+="{\"time\":"+std::to_string(item.timeTicks)+",\"x\":";sequenceJsonDouble(out,item.origin.x);
                out+=",\"y\":";sequenceJsonDouble(out,item.origin.y);out+=",\"width\":";sequenceJsonDouble(out,item.extent.x);
                out+=",\"height\":";sequenceJsonDouble(out,item.extent.y);out+='}';
            }
            out+="]}";
        }
        out+=']';jsonOutput(out,output,capacity,required);
    });
}
} // extern "C"
