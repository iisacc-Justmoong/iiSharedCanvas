#pragma once
#include "ProjectedStrokeBridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Exclusive single-threaded document handle. It owns the document and history. */
typedef struct IiscCanvasControl IiscCanvasControl;
typedef struct IiscSequenceFlatten IiscSequenceFlatten;
typedef struct IiscSequenceFlattenInfo {
    uint64_t first_frame, frame_count;
    int32_t width, height;
    const char *source_sequence, *source_clip;
} IiscSequenceFlattenInfo;
typedef struct IiscCanvasInfo {
    int32_t width, height;
    uint32_t frame_count;
    uint64_t assets, layers, authorship_revision, undo_entries, redo_entries;
} IiscCanvasInfo;
/* Named numeric fields are validated by the native processing implementation. */
typedef struct IiscCanvasSetting { const char *name; double value; } IiscCanvasSetting;
/* All pointer fields are borrowed for the duration of a single call. */
typedef struct IiscFillStop { double position; uint32_t color; } IiscFillStop;
typedef struct IiscFillSpec {
    int32_t kind, width, height, repeat;
    uint32_t color;
    double start_x, start_y, end_x, end_y, pattern_x, pattern_y;
    const char *pattern_asset;
    const IiscFillStop *stops;
    size_t stop_count;
} IiscFillSpec;
typedef struct IiscLayerStyle {
    int32_t kind, enabled;
    uint32_t color;
    double opacity, radius, offset_x, offset_y;
} IiscLayerStyle;
typedef struct IiscMaskSpec {
    const char *asset;
    int32_t enabled, inverted, linked, channel;
    double density, feather, outside, m11, m12, m21, m22, x, y;
} IiscMaskSpec;

/* Borrowed geometry and vector values. No pointer is retained after a call. */
typedef struct IiscLayerGeometry { const char *id; double x,y,width,height; } IiscLayerGeometry;
typedef struct IiscShapeOperand { const char *asset; double m11,m12,m21,m22,x,y; } IiscShapeOperand;
typedef struct IiscVectorCommand { int32_t kind; double x1,y1,x2,y2,x3,y3; } IiscVectorCommand;
typedef struct IiscCanvasBounds { int32_t present,x,y,width,height; } IiscCanvasBounds;

/* Scan borrowed ARGB pixels without a document; false reports invalid input. */
IISC_PYTHON_EXPORT int iisc_scan_transparent_bounds(const uint32_t *,size_t,int32_t,int32_t,
    int32_t,int32_t,uint8_t,IiscCanvasBounds *);

/* Create an owned finite raster document, or return NULL for invalid input. */
IISC_PYTHON_EXPORT IiscCanvasControl *iisc_canvas_raster(int32_t, int32_t, uint32_t);
/* Create an independent opaque grayscale document from 8-bit coverage. */
IISC_PYTHON_EXPORT IiscCanvasControl *iisc_canvas_from_channel(int32_t,int32_t,const uint8_t *,size_t);
/* Decode an independent document; input bytes are borrowed only during this call. */
IISC_PYTHON_EXPORT IiscCanvasControl *iisc_canvas_decode(const uint8_t *, size_t);
/* Clone the source value with fresh history; NULL reports an error on source. */
IISC_PYTHON_EXPORT IiscCanvasControl *iisc_canvas_clone(IiscCanvasControl *);
/* Destroy the handle and its edit sessions. The pointer must not be reused. */
IISC_PYTHON_EXPORT void iisc_canvas_destroy(IiscCanvasControl *);
/* Borrow the last operation's error text until the next operation or destroy. */
IISC_PYTHON_EXPORT const char *iisc_canvas_error(const IiscCanvasControl *);
/* Copy a detached metadata and history summary into the caller's output. */
IISC_PYTHON_EXPORT int iisc_canvas_info(IiscCanvasControl *, IiscCanvasInfo *);
/* Call once with NULL output to obtain the size, then again with a buffer. */
IISC_PYTHON_EXPORT int iisc_canvas_encode(IiscCanvasControl *, uint8_t *, size_t, size_t *);
/* Render the requested frame and return one document-coordinate ARGB pixel. */
IISC_PYTHON_EXPORT int iisc_canvas_pixel(IiscCanvasControl *, uint32_t, int32_t, int32_t, uint32_t *);
/* Copy the source into a new owned asset and layer as one history command. */
IISC_PYTHON_EXPORT int iisc_canvas_embed(IiscCanvasControl *, const IiscCanvasControl *, const char *, const char *, uint32_t);
/* Replace an existing owned copy without retaining the source handle. */
IISC_PYTHON_EXPORT int iisc_canvas_replace(IiscCanvasControl *, const IiscCanvasControl *, const char *, uint32_t);
/* Move one shared history entry backward or forward, subject to conflict checks. */
IISC_PYTHON_EXPORT int iisc_canvas_undo(IiscCanvasControl *);
IISC_PYTHON_EXPORT int iisc_canvas_redo(IiscCanvasControl *);
/* Capture the current undo entry and toggle only that stable entry thereafter. */
IISC_PYTHON_EXPORT int iisc_canvas_capture_last(IiscCanvasControl *);
IISC_PYTHON_EXPORT int iisc_canvas_toggle_last(IiscCanvasControl *);
/* Capture original pixels with a caller-selected logical memory budget. */
IISC_PYTHON_EXPORT int iisc_canvas_begin_reedit(IiscCanvasControl *, const char *, size_t);
/* Recalculate a complete adjustment from the captured original pixels. */
IISC_PYTHON_EXPORT int iisc_canvas_adjust(IiscCanvasControl *, const IiscCanvasSetting *, size_t);
/* Recalculate the selected RasterEffectKind from the same original pixels. */
IISC_PYTHON_EXPORT int iisc_canvas_effect(IiscCanvasControl *, int32_t, const IiscCanvasSetting *, size_t);
/* Create a layer when layer_id is present, otherwise replace existing content. */
IISC_PYTHON_EXPORT int iisc_canvas_put_fill(IiscCanvasControl *,const char *,const char *,const IiscFillSpec *);
IISC_PYTHON_EXPORT int iisc_canvas_put_adjustment(IiscCanvasControl *,const char *,const char *,int32_t,int32_t,
    const IiscCanvasSetting *,size_t);
/* Replace a complete style stack or one mask slot as a history command. */
IISC_PYTHON_EXPORT int iisc_canvas_styles(IiscCanvasControl *,const char *,const IiscLayerStyle *,size_t);
IISC_PYTHON_EXPORT int iisc_canvas_mask(IiscCanvasControl *,const char *,int,const IiscMaskSpec *);
/* Fill caller-owned ARGB pixels from a layer (non-null ID) or the composition. */
IISC_PYTHON_EXPORT int iisc_canvas_slice(IiscCanvasControl *,const char *,uint32_t,int32_t,int32_t,int32_t,int32_t,
    int32_t,int32_t,int,uint32_t *,size_t);
/* Channel and exchange operations use borrowed, aligned arrays. Publication
   to an owned asset is one lock-aware, reversible document history command. */
IISC_PYTHON_EXPORT int iisc_calculate_channels(const uint32_t *,const uint32_t *,size_t,int32_t,int32_t,
    int,int,int,int,int,int,double,const uint8_t *,size_t,uint8_t *);
/* The caller owns all arrays; motion vectors use interleaved x/y doubles. */
IISC_PYTHON_EXPORT int iisc_estimate_optical_flow(const uint32_t *,const uint32_t *,size_t,int32_t,int32_t,
    uint32_t,uint32_t,uint64_t,size_t,double *,double *);
IISC_PYTHON_EXPORT int iisc_interpolate_optical_flow(const uint32_t *,const uint32_t *,size_t,int32_t,int32_t,
    const double *,const double *,double,size_t,uint32_t *);
/* Explicit prepare, app-owned temporary publication and atomic history commit. */
IISC_PYTHON_EXPORT int iisc_canvas_sequence_flatten_prepare(IiscCanvasControl *,const char *,const char *,
    uint64_t,uint64_t,IiscSequenceFlatten **,IiscSequenceFlattenInfo *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_flatten_write(IiscCanvasControl *,const IiscSequenceFlatten *,const char *,const char *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_flatten_commit(IiscCanvasControl *,const IiscSequenceFlatten *,
    const uint32_t *,size_t,const char *,const char *,const char *,const char *,const char *,uint64_t);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_flatten_replace_original(IiscCanvasControl *,const char *,const char *,
    const IiscCanvasControl *,uint64_t);
IISC_PYTHON_EXPORT void iisc_sequence_flatten_destroy(IiscSequenceFlatten *);
IISC_PYTHON_EXPORT int iisc_raw_sequence_frame(const uint8_t *,size_t,uint64_t,size_t,uint32_t *,size_t,int32_t *,int32_t *);
IISC_PYTHON_EXPORT int iisc_canvas_apply_image(IiscCanvasControl *,const char *,uint32_t,const uint32_t *,size_t,
    int32_t,int32_t,int,int,int,int,double,const uint8_t *,size_t);
/* Preserve frame rate and change frame count through one history entry. */
IISC_PYTHON_EXPORT int iisc_canvas_set_frame_count(IiscCanvasControl *,uint32_t);
IISC_PYTHON_EXPORT int iisc_canvas_composite(IiscCanvasControl *,const char *const *,size_t,uint32_t,
    int32_t,int32_t,int32_t,int32_t,int32_t,int32_t,int,const uint8_t *,size_t,uint32_t *,size_t);
/* Text results follow the same size-query and copy protocol as JSON snapshots. */
IISC_PYTHON_EXPORT int iisc_canvas_layer_css(IiscCanvasControl *,const char *,uint32_t,uint8_t *,size_t,size_t *);
IISC_PYTHON_EXPORT int iisc_canvas_selected_svg(IiscCanvasControl *,const char *const *,size_t,uint32_t,
    int32_t,int32_t,int32_t,int32_t,uint8_t *,size_t,size_t *);
/* Plan and atomically apply layer order, alignment or distribution. */
IISC_PYTHON_EXPORT int iisc_canvas_arrange(IiscCanvasControl *,const char *const *,size_t,int);
IISC_PYTHON_EXPORT int iisc_canvas_align(IiscCanvasControl *,const IiscLayerGeometry *,size_t,double,double,double,double,int);
IISC_PYTHON_EXPORT int iisc_canvas_distribute(IiscCanvasControl *,const IiscLayerGeometry *,size_t,int,int);
/* Construct and edit filled vector operands and their exact compound result. */
IISC_PYTHON_EXPORT int iisc_canvas_put_vector(IiscCanvasControl *,const char *,const char *,const IiscVectorCommand *,size_t,uint32_t);
IISC_PYTHON_EXPORT int iisc_canvas_combine_shapes(IiscCanvasControl *,const IiscShapeOperand *,size_t,int,const char *,const char *,uint32_t);
IISC_PYTHON_EXPORT int iisc_canvas_vector_command(IiscCanvasControl *,const char *,size_t,size_t,IiscVectorCommand *);
IISC_PYTHON_EXPORT int iisc_canvas_edit_vector_command(IiscCanvasControl *,const char *,size_t,size_t,const IiscVectorCommand *);
/* Render and publish a frame-preserving merge or flatten as one history entry. */
IISC_PYTHON_EXPORT int iisc_canvas_merge(IiscCanvasControl *,const char *const *,size_t,const char *,const char *);
IISC_PYTHON_EXPORT int iisc_canvas_flatten(IiscCanvasControl *,const char *,const char *);
/* Publish a non-destructive canvas resize or crop as one history entry. */
IISC_PYTHON_EXPORT int iisc_canvas_resize(IiscCanvasControl *,int32_t,int32_t,int);
IISC_PYTHON_EXPORT int iisc_canvas_crop(IiscCanvasControl *,int32_t,int32_t,int32_t,int32_t);
IISC_PYTHON_EXPORT int iisc_canvas_resample(IiscCanvasControl *,int32_t,int32_t,int);
IISC_PYTHON_EXPORT int iisc_canvas_rotate(IiscCanvasControl *,double);
/* Query document bounds with a bounded region and optional frame list. */
IISC_PYTHON_EXPORT int iisc_canvas_content_bounds(IiscCanvasControl *,int,int,int32_t,int32_t,int32_t,int32_t,
    const uint32_t *,size_t,uint8_t,size_t,IiscCanvasBounds *);
/* Read or change exact pixel aspect; display output is square-pixel ARGB. */
IISC_PYTHON_EXPORT int iisc_canvas_pixel_aspect(IiscCanvasControl *,uint32_t *,uint32_t *);
IISC_PYTHON_EXPORT int iisc_canvas_set_pixel_aspect(IiscCanvasControl *,uint32_t,uint32_t);
IISC_PYTHON_EXPORT int iisc_canvas_display(IiscCanvasControl *,uint32_t,int,uint32_t *,size_t,int32_t *,int32_t *);
/* Save a complete alpha mask and publish the change as one undo entry. */
IISC_PYTHON_EXPORT int iisc_canvas_selection_set(IiscCanvasControl *,const char *,const char *,
    int32_t,int32_t,int32_t,int32_t,const uint8_t *,size_t);
IISC_PYTHON_EXPORT int iisc_canvas_selection_rename(IiscCanvasControl *,const char *,const char *);
IISC_PYTHON_EXPORT int iisc_canvas_selection_delete(IiscCanvasControl *,const char *);
IISC_PYTHON_EXPORT int iisc_canvas_selection_coverage(IiscCanvasControl *,const char *,int32_t,int32_t,uint8_t *);
/* Upsert, remove or assign groups; empty parent/group IDs denote the root. */
IISC_PYTHON_EXPORT int iisc_canvas_group_set(IiscCanvasControl *,const char *,const char *,const char *,int,int);
IISC_PYTHON_EXPORT int iisc_canvas_group_delete(IiscCanvasControl *,const char *);
IISC_PYTHON_EXPORT int iisc_canvas_group_assign(IiscCanvasControl *,const char *,const char *);
/* Establish/remove an undirected edge or set layer/group lock flags. */
IISC_PYTHON_EXPORT int iisc_canvas_link(IiscCanvasControl *,const char *,const char *,int);
IISC_PYTHON_EXPORT int iisc_canvas_lock(IiscCanvasControl *,const char *,int,uint8_t);
/* Return UTF-8 JSON snapshots using the usual size-query then copy protocol. */
IISC_PYTHON_EXPORT int iisc_canvas_relations(IiscCanvasControl *,uint8_t *,size_t,size_t *);
IISC_PYTHON_EXPORT int iisc_canvas_query(IiscCanvasControl *,const char *,int,int64_t,int,const char *,uint8_t *,size_t,size_t *);

/* Sequence arrays and strings are borrowed for one call. All edits below are
   published as one session-history entry after native validation. */
typedef struct IiscSequencePair { const char *left, *right; } IiscSequencePair;
typedef struct IiscSequenceClipRange { const char *id; int64_t start, duration; } IiscSequenceClipRange;
/* A borrowed description for creating one empty sequence; all identifiers and
   settings are copied before the call returns. Duration is measured in the
   supplied sequence time base. */
typedef struct IiscSequenceCreate {
    const char *project_id, *sequence_id, *name;
    int64_t duration;
    uint32_t time_num, time_den, rate_num, rate_den;
    int32_t width, height;
} IiscSequenceCreate;
typedef struct IiscSequenceSettings {
    uint32_t flags; const char *name; uint32_t rate_num, rate_den;
    int32_t width, height; int64_t aspect_num; uint64_t aspect_den;
    uint32_t audio_rate; const char *audio_layout;
    /* The fields below are read only when flag 64 is set. */
    int32_t timecode_present; int64_t timecode_start_frame;
    uint32_t timecode_rate_num, timecode_rate_den, timecode_nominal_fps;
    int32_t timecode_counting, timecode_wrap_24_hours;
} IiscSequenceSettings;
/* A typed subtitle property. kind follows TimelinePropertyValue alternative
   order. Input strings and number arrays are borrowed for the call. */
typedef struct IiscSequenceProperty {
    const char *key; int32_t kind, boolean_value; int64_t integer_value;
    uint64_t denominator; double components[4]; const char *text;
    const double *numbers; size_t number_count;
} IiscSequenceProperty;
/* A complete subtitle style. All data is copied into the resulting history
   entry. Colors are RGBA doubles, and flags are exactly zero or one. */
typedef struct IiscSequenceCaptionStyle {
    const char *font_family; double font_size;
    double fill[4], outline[4], background[4], outline_width;
    int32_t bold, italic, underline;
    const char *horizontal_alignment, *vertical_alignment, *writing_direction;
    const IiscSequenceProperty *properties; size_t property_count;
} IiscSequenceCaptionStyle;
/* Owned media input is borrowed only for the call and copied into history. */
typedef struct IiscSequenceOwnedMedia {
    const char *id, *media_type; const uint8_t *data; size_t size;
} IiscSequenceOwnedMedia;
typedef struct IiscSequenceTrackChanges {
    uint32_t flags; int32_t enabled, locked, muted, solo, sync_locked;
    const char *role, *language, *blend_mode, *output_bus; double opacity;
} IiscSequenceTrackChanges;
typedef struct IiscSequenceKeyframe { int64_t time; double center_x, center_y, scale; } IiscSequenceKeyframe;
typedef struct IiscSequenceObservation {
    int64_t time; const char *track_id; double x, y, width, height;
} IiscSequenceObservation;
typedef struct IiscSequenceDetection {
    int64_t time; int32_t class_id; const char *label;
    double confidence, x, y, width, height;
} IiscSequenceDetection;

IISC_PYTHON_EXPORT int iisc_canvas_sequence_snapshot(IiscCanvasControl *,uint8_t *,size_t,size_t *);
/* Each mutation is atomic and undoable. Copy borrows the source only for the
   call, and the target owns independent copies of its media bytes. Existing
   target projects are rejected unless replace is one. */
IISC_PYTHON_EXPORT int iisc_canvas_sequence_create(IiscCanvasControl *,const IiscSequenceCreate *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_copy(IiscCanvasControl *,const IiscCanvasControl *,int);
/* Full, typed project trees use the frozen timeline schema. JSON projections
   and media outputs are caller-owned buffers; replacement validates and copies
   all inputs before publishing one history entry. */
IISC_PYTHON_EXPORT int iisc_canvas_sequence_model(IiscCanvasControl *,uint8_t *,size_t,size_t *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_model_schema(IiscCanvasControl *,uint8_t *,size_t,size_t *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_model_default(IiscCanvasControl *,const char *,uint8_t *,size_t,size_t *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_media_list(IiscCanvasControl *,uint8_t *,size_t,size_t *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_media_data(IiscCanvasControl *,const char *,uint8_t *,size_t,size_t *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_model_replace(IiscCanvasControl *,const uint8_t *,size_t,
                                                           const IiscSequenceOwnedMedia *,size_t);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_settings(IiscCanvasControl *,const char *,const IiscSequenceSettings *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_tracks(IiscCanvasControl *,const char *,const char *const *,size_t,
                                                    const IiscSequenceTrackChanges *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_add_track(IiscCanvasControl *,const char *,const char *,int);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_split(IiscCanvasControl *,const char *,const IiscSequencePair *,size_t,
                                                   const IiscSequencePair *,size_t,int64_t);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_trim(IiscCanvasControl *,const char *,const IiscSequenceClipRange *,size_t);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_extend(IiscCanvasControl *,const char *,const char *const *,size_t,int,int64_t);
/* operation: 0 lift, 1 extract, 2 close gap, 3 copy subsequence, 4 move subsequence. */
IISC_PYTHON_EXPORT int iisc_canvas_sequence_range(IiscCanvasControl *,int,const char *,const char *const *,size_t,
                                                   int64_t,int64_t,const char *,const char *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_simplify(IiscCanvasControl *,const char *,uint32_t,const char *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_caption(IiscCanvasControl *,const char *,const char *,int,const char *,
    const char *,const char *,int64_t,int64_t,const char *,const char *,double);
/* Optional full style and text changes are published atomically as one edit. */
IISC_PYTHON_EXPORT int iisc_canvas_sequence_caption_ex(IiscCanvasControl *,const char *,const char *,int,const char *,
    const char *,const char *,int64_t,int64_t,const char *,const char *,double,const IiscSequenceCaptionStyle *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_transition(IiscCanvasControl *,const char *,const char *,const char *,
    const char *,const char *,const char *,int64_t,int64_t,int);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_reframe(IiscCanvasControl *,const char *,const char *,const char *,
    const IiscSequenceKeyframe *,size_t,const IiscSequenceObservation *,size_t,const char *,
    int32_t,int32_t,int32_t,int32_t,double);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_gaps(IiscCanvasControl *,const char *,const char *const *,size_t,
                                                  uint8_t *,size_t,size_t *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_effect_time(IiscCanvasControl *,const char *,const char *,int64_t,uint64_t,
    int *,int64_t *,uint64_t *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_through(IiscCanvasControl *,const char *,const char *const *,size_t,
                                                     uint8_t *,size_t,size_t *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_default(IiscCanvasControl *,int,int64_t,const char *,uint8_t *,size_t,size_t *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_match(IiscCanvasControl *,const char *,uint64_t,uint8_t *,size_t,size_t *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_reverse_match(IiscCanvasControl *,const char *,const char *,const char *,
    uint64_t,uint64_t,uint8_t *,size_t,size_t *);
IISC_PYTHON_EXPORT int iisc_canvas_sequence_track_subjects(IiscCanvasControl *,const IiscSequenceDetection *,size_t,
    const char *,int64_t,double,double,uint8_t *,size_t,size_t *);

#ifdef __cplusplus
}
#endif
