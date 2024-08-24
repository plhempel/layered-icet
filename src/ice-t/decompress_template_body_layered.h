/* This file instantiates the blending of a sparse layered image into a regular
 * non-layered image, implemented in `decompress_template_body.h`, for a given
 * fragment format.
 * The corresponding type must be defined as `DTL_FRAGMENT_TYPE`.
 * Additionally, the matching over-operator must be defined as `DTL_OVER`.
 * Both macros are undefined at the end of this file.  The file should only be
 * included at the appropriate locations in `decompress_func_body.h`.
 */

/* Check for required macros. */
#ifndef DTL_FRAGMENT_TYPE
#error "Missing macro DTL_FRAGMENT_TYPE."
#endif
#ifndef DTL_OVER
#error "Missing macro DTL_OVER."
#endif

{
/* Local utility to write the configured background color to the current pixel,
 * without advancing the output pointer.
 */
#define DTL_COPY_BACKGROUND()                           \
{                                                       \
    for (int channel = 0; channel < 4; channel++) {     \
        _color[channel] = _background_color[channel];   \
    }                                                   \
}

/* Define input image for `decompress_template_body.h`. */
#define DT_COMPRESSED_IMAGE INPUT_SPARSE_IMAGE

/* For a run of inactive pixels, set each to the configured background color. */
#define DT_INCREMENT_INACTIVE_PIXELS(count)     \
{                                               \
    for (IceTSizeType i = 0; i < count; i++) {  \
        DTL_COPY_BACKGROUND();                  \
        _color += 4;                            \
    }                                           \
}

/* For a single active pixel, set the output to the configured background color,
 * then blend all fragments over it.
 */
#define DT_READ_PIXEL(src)                                          \
{                                                                   \
    const IceTLayerCount num_layers = *(const IceTLayerCount *)src; \
    src += sizeof(IceTLayerCount);                                  \
                                                                    \
    DTL_COPY_BACKGROUND();                                          \
                                                                    \
    for (                                                           \
        const DTL_FRAGMENT_TYPE *in_frag =                          \
            (const DTL_FRAGMENT_TYPE *)src + num_layers - 1;        \
        (const IceTByte *)in_frag >= src;                           \
        in_frag--                                                   \
    ) {                                                             \
        DTL_OVER(in_frag->color, _color);                           \
    }                                                               \
                                                                    \
    src += num_layers * sizeof(DTL_FRAGMENT_TYPE);                  \
    _color += 4;                                                    \
}

/* Account for initial offset. */
#ifdef OFFSET
    _color += 4*(OFFSET);
#endif

/* Instantiate the template. */
#include "decompress_template_body.h"
}

/* Undefine local macros. */
#undef DTL_COPY_BACKGROUND
#undef DTL_FRAGMENT_TYPE
#undef DTL_OVER
