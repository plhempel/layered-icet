/* This file instantiates the combining of two layered images, implemented in
 * `cc_composite_template_body.h`, for a given fragment format specified via the
 * macro CCCL_FRAGMENT_FROMAT.  Since the purpose of using layered images is to
 * support non-commutative compositing operators, the actual compositing is only
 * performed during the final decompression, this template simply merges the
 * fragments from both images in order per pixel.
 * This file should only be included at the appropriate locations in
 * `cc_composite_func_body.h`.
 */
 {
/* Helper to expand and concatenate two macros. */
#define CCCL_CONCAT_IMPL(a, b) a##b
#define CCCL_CONCAT(a, b) CCCL_CONCAT_IMPL(a, b)

/* Combine two pixels from different images into one by merging their fragments
 * ordered by depth.  The order of the input images is arbitrary.
 */
#define CCC_COMPOSITE(pixel1_pointer, pixel2_pointer, dest_pointer)             \
{                                                                               \
    /* Retrieve the number of fragments in each input pixel. */                 \
    const IceTLayerCount num_frags1 = *(const IceTLayerCount *)pixel1_pointer;  \
    const IceTLayerCount num_frags2 = *(const IceTLayerCount *)pixel2_pointer;  \
    /* Advance pointers past the layer count. */                                \
    const CCCL_FRAGMENT_TYPE *frag1 = (const CCCL_FRAGMENT_TYPE *)              \
                                     (pixel1_pointer + sizeof(IceTLayerCount)); \
    const CCCL_FRAGMENT_TYPE *frag2 = (const CCCL_FRAGMENT_TYPE *)              \
                                     (pixel2_pointer + sizeof(IceTLayerCount)); \
    CCCL_FRAGMENT_TYPE *dest_frag = (CCCL_FRAGMENT_TYPE *)                      \
                                   (dest_pointer + sizeof(IceTLayerCount));     \
    /* Calculate the address past the last fragment in each pixel. */           \
    const CCCL_FRAGMENT_TYPE *const end1 = frag1 + num_frags1;                  \
    const CCCL_FRAGMENT_TYPE *const end2 = frag2 + num_frags2;                  \
                                                                                \
    /* Write the total number of fragments to the ouput pixel. */               \
    *(IceTLayerCount *)dest_pointer = num_frags1 + num_frags2;                  \
                                                                                \
    /* Copy pixels in order. */                                                 \
    while (ICET_TRUE) {                                                         \
        switch ((frag1 < end1) | ((frag2 < end2) << 1)) {                       \
        case 0: /* All fragments have been copied, the pixel is complete. */    \
            goto CCCL_CONCAT(pixel_complete_, CCCL_FRAGMENT_TYPE);              \
        case 1: /* Only pixel 1 has a fragment left, copy it. */                \
            *(dest_frag++) = *(frag1++);                                        \
            continue;                                                           \
        case 2: /* Only pixel 2 has a fragment left, copy it. */                \
            *(dest_frag++) = *(frag2++);                                        \
            continue;                                                           \
        case 3: /* Both pixels have a fragment left, copy the one in front. */  \
            if (frag1->depth <= frag2->depth) {                                 \
                *(dest_frag++) = *(frag1++);                                    \
            } else {                                                            \
                *(dest_frag++) = *(frag2++);                                    \
            }                                                                   \
            continue;                                                           \
        }                                                                       \
    }                                                                           \
    /* Label to break from the loop, tagged with the fragment type to           \
     * distinguish between template instantiations. */                          \
    CCCL_CONCAT(pixel_complete_, CCCL_FRAGMENT_TYPE):;                          \
    /* Count fragments as processed. */                                         \
    _front_num_active_frags -= num_frags1;                                      \
    _back_num_active_frags -= num_frags2;                                       \
    /* Advance pointers past the pixel. */                                      \
    pixel1_pointer = (const IceTByte*)frag1;                                    \
    pixel2_pointer = (const IceTByte*)frag2;                                    \
    dest_pointer = (IceTByte*)dest_frag;                                        \
}

/* Define common macros. */
#define CCC_FRONT_COMPRESSED_IMAGE FRONT_SPARSE_IMAGE
#define CCC_BACK_COMPRESSED_IMAGE BACK_SPARSE_IMAGE
#define CCC_DEST_COMPRESSED_IMAGE DEST_SPARSE_IMAGE
#define CCC_LAYERED
#define CCC_FRAGMENT_SIZE sizeof(CCCL_FRAGMENT_TYPE)

#include "cc_composite_template_body.h"
}

/* Undefine local macros. */
#undef CCCL_CONCAT
#undef CCCL_CONCAT_IMPl
#undef CCCL_FRAGMENT_TYPE
