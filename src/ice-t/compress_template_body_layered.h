/* This file instantiates the compression of layered images, implemented in
 * `compress_template_body.h`, for a given fragment format specified via the
 * macros CTL_DEPTH_TYPE as well as optionally CTL_COLOR_TYPE and
 * CTL_COLOR_CHANNELS if the format includes color.
 * The file should only be included at the appropriate locations in
 * `compress_func_body.h`.
 */

/* Check for required macros. */
#ifndef CTL_DEPTH_TYPE
#error "Missing macro CTL_DEPTH_TYPE."
#endif

#if defined(CTL_COLOR_TYPE) != defined(CTL_COLOR_CHANNELS)
#error "Must define either both or neither of CTL_COLOR_TYPE and "
       "CTL_COLOR_CHANNELS."
#endif

{
/* Define macros for copying fragment data and advancing input iterator.
 * They differ between formats with and without color.
 */
#ifdef CTL_COLOR_TYPE
#define CTL_WRITE_FRAGMENT(layer, dest)                                 \
    /* Copy color.  The compiler should unroll this loop. */            \
    for (int i = 0; i < CTL_COLOR_CHANNELS; i++) {                      \
        *(CTL_COLOR_TYPE *)dest = _color[layer*CTL_COLOR_CHANNELS + i]; \
        dest += sizeof(CTL_COLOR_TYPE);                                 \
    }                                                                   \
    /* Copy depth. */                                                   \
    *(CTL_DEPTH_TYPE *)dest = _depth[layer];                            \
    dest += sizeof(CTL_DEPTH_TYPE);
#define CTL_INCREMENT_N_PIXELS(num_pixels)                      \
    _color += _num_layers * num_pixels * CTL_COLOR_CHANNELS;    \
    _depth += _num_layers * num_pixels;
#else
#define CTL_WRITE_FRAGMENT(layer, dest)         \
    *(CTL_DEPTH_TYPE *)dest = _depth[layer];    \
    dest += sizeof(CTL_DEPTH_TYPE);
#define CTL_INCREMENT_N_PIXELS(num_pixels)  \
    _depth += _num_layers * num_pixels;
#endif

/* Define macros that are independent of the fragment format, but nevertheless
 * have to be defined for each case since `compress_template_body.h` undefines
 * them. */
#define CT_COMPRESSED_IMAGE     OUTPUT_SPARSE_IMAGE
#define CT_COLOR_FORMAT         _color_format
#define CT_DEPTH_FORMAT         _depth_format
#define CT_PIXEL_COUNT          _pixel_count

#ifdef REGION
#define CT_INCREMENT_PIXEL()    CTL_INCREMENT_N_PIXELS(1);                  \
                                _region_count++;                            \
                                if (_region_count >= _region_width) {       \
                                    CTL_INCREMENT_N_PIXELS(_region_x_skip); \
                                    _region_count = 0;                      \
                                }
#else
#define CT_INCREMENT_PIXEL()    CTL_INCREMENT_N_PIXELS(1)
#endif

#ifdef PADDING
#define CT_PADDING
#define CT_SPACE_BOTTOM         SPACE_BOTTOM
#define CT_SPACE_TOP            SPACE_TOP
#define CT_SPACE_LEFT           SPACE_LEFT
#define CT_SPACE_RIGHT          SPACE_RIGHT
#define CT_FULL_WIDTH           FULL_WIDTH
#define CT_FULL_HEIGHT          FULL_HEIGHT
#endif

#ifdef REGION
    IceTSizeType _region_count = 0;
#endif

/* Count the number of active fragments in a run.  This is optional, since in
 * some cases layered input images produce non-layered output images, which do
 * not need active fragments to be counted separately from active pixels. */
#ifdef CT_ACTIVE_FRAGS
    IceTSizeType CT_ACTIVE_FRAGS = 0;
#endif

    /* Input iterators. */
#ifdef CTL_COLOR_TYPE
    const CTL_COLOR_TYPE *_color =
        icetImageGetColorConstVoid(INPUT_IMAGE, NULL);
#endif
    const CTL_DEPTH_TYPE *_depth =
        icetImageGetDepthConstVoid(INPUT_IMAGE, NULL);

#ifdef OFFSET
    CTL_INCREMENT_N_PIXELS(OFFSET);
#endif

/* Instantiate the template. */
#include "compress_template_body.h"
}

/* Undefine local macros. */
#undef CTL_WRITE_FRAGMENT
#undef CTL_INCREMENT_N_PIXELS
#undef CTL_COLOR_TYPE
#undef CTL_COLOR_CHANNELS
