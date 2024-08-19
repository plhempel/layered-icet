/* This file instantiates the compression of layered images, defined in
 * `compress_template_body.h`, for a given fragment format.  The format must be
 * specified via the macro `CTL_FRAGMENT_FORMAT`, which is undefined at the end
 * of this file.  The file should only be included at the appropriate location
 * in `compress_func_body.h`.
 */

/* Check for required macros. */
#ifndef CTL_FRAGMENT_FORMAT
#error "Missing macro CTL_FRAGMENT_FORMAT."
#endif

{
/* Define macros that are independent of the fragment format, but nevertheless
 * have to be defined for each case since `compress_template_body.h` undefines
 * them. */
#define CT_COMPRESSED_IMAGE     OUTPUT_SPARSE_IMAGE
#define CT_COLOR_FORMAT         _color_format
#define CT_DEPTH_FORMAT         _depth_format
#define CT_PIXEL_COUNT          _pixel_count

#ifdef REGION
#define CT_INCREMENT_PIXEL()    _fragment += _num_layers;               \
                                _region_count++;                        \
                                if (_region_count >= _region_width) {   \
                                    _fragment += _region_x_skip;        \
                                    _region_count = 0;                  \
                                }
#else
#define CT_INCREMENT_PIXEL()    _fragment += _num_layers;
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

/* Concatenate two tokens.  Indirection is required to macro-expand arguments.
 */
#define CTL_CONCAT_(T1, T2) T1 ## T2
#define CTL_CONCAT(T1, T2)  CTL_CONCAT_(T1, T2)

/* Struct type representing a fragment of the given format. */
#define CTL_FRAGMENT_TYPE   CTL_CONCAT(IceTFragment_, CTL_FRAGMENT_FORMAT)

    /* Input iterator. */
    const CTL_FRAGMENT_TYPE *_fragment =
        CTL_CONCAT(icetLayeredImageGetFragmentsConst_, CTL_FRAGMENT_FORMAT)(INPUT_IMAGE);

#ifdef OFFSET
    _fragment += OFFSET;
#endif

/* Instantiate the template. */
#include "compress_template_body.h"
}

/* Undefine local macros. */
#undef CTL_CONCAT
#undef CTL_CONCAT_
#undef CTL_FRAGMENT_FORMAT
#undef CTL_FRAGMENT_TYPE
