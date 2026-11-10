#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(IISC_PYTHON_BUILD)
#define IISC_PYTHON_EXPORT __declspec(dllexport)
#else
#define IISC_PYTHON_EXPORT __declspec(dllimport)
#endif
#else
#define IISC_PYTHON_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct IiscProjectedRaster IiscProjectedRaster;
/* Straight ARGB contribution and per-stroke opacity cap at a pixel. */
typedef struct IiscProjectedSample {
    int32_t x;
    int32_t y;
    uint32_t argb;
    uint8_t opacity_cap;
} IiscProjectedSample;

/* The caller owns the returned in-memory raster and must destroy it. The
   handle is exclusive and is not safe for concurrent calls. NULL indicates an
   invalid size or allocation failure. */
IISC_PYTHON_EXPORT IiscProjectedRaster *iisc_projected_create(
    int32_t width, int32_t height, uint32_t background, int erase);
IISC_PYTHON_EXPORT void iisc_projected_destroy(IiscProjectedRaster *raster);
IISC_PYTHON_EXPORT const char *iisc_projected_error(const IiscProjectedRaster *raster);
IISC_PYTHON_EXPORT int iisc_projected_begin(IiscProjectedRaster *raster);
/* Stable samples accumulate; transient samples replace the previous live tip.
   Calls return 1 on success, 0 on error. Error text is borrowed until the next
   call on that raster. */
IISC_PYTHON_EXPORT int iisc_projected_update(IiscProjectedRaster *raster,
    const IiscProjectedSample *stable, size_t stable_count,
    const IiscProjectedSample *transient, size_t transient_count);
IISC_PYTHON_EXPORT int iisc_projected_end(IiscProjectedRaster *raster);
IISC_PYTHON_EXPORT int iisc_projected_cancel(IiscProjectedRaster *raster);
IISC_PYTHON_EXPORT int iisc_projected_undo(IiscProjectedRaster *raster);
IISC_PYTHON_EXPORT int iisc_projected_redo(IiscProjectedRaster *raster);
IISC_PYTHON_EXPORT int iisc_projected_pixel(IiscProjectedRaster *raster,
    int32_t x, int32_t y, uint32_t *pixel);
IISC_PYTHON_EXPORT int iisc_projected_source_pixel(IiscProjectedRaster *raster,
    int32_t x, int32_t y, uint32_t *pixel);

#ifdef __cplusplus
}
#endif
