#ifndef CTL_FRAGMENT_FORMAT
#error "Missing macro CTL_FRAGMENT_FORMAT."
#endif

{
#define CT_COMPRESSED_IMAGE     OUTPUT_SPARSE_IMAGE
#define CT_COLOR_FORMAT         _color_format
#define CT_DEPTH_FORMAT         _depth_format
#define CT_PIXEL_COUNT          _pixel_count

#ifdef REGION
#define CT_INCREMENT_PIXEL()    _region_count++;                        \
                                if (_region_count >= _region_width) {   \
                                    _fragment += _region_x_skip;        \
                                    _region_count = 0;                  \
                                }
#else
#define CT_INCREMENT_PIXEL()
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

#ifdef CT_FRAG_COUNT
    IceTSizeType CT_FRAG_COUNT = 0;
#endif

#define CTL_CONCAT_(T1, T2) T1 ## T2
#define CTL_CONCAT(T1, T2) CTL_CONCAT_(T1, T2)
#define CTL_FRAGMENT_TYPE CTL_CONCAT(IceTFragment_, CTL_FRAGMENT_FORMAT)

    const CTL_FRAGMENT_TYPE *_fragment =
        CTL_CONCAT(icetLayeredImageGetFragmentsConst_, CTL_FRAGMENT_FORMAT)(INPUT_IMAGE);

#ifdef OFFSET
    _fragment += OFFSET;
#endif

#include "compress_template_body.h"
}

#undef CTL_CONCAT
#undef CTL_CONCAT_
#undef CTL_FRAGMENT_FORMAT
#undef CTL_FRAGMENT_TYPE
