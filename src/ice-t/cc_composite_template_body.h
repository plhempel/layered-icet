/* -*- c -*- *******************************************************/
/*
 * Copyright (C) 2011 Sandia Corporation
 * Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
 * the U.S. Government retains certain rights in this software.
 *
 * This source code is released under the New BSD License.
 */

/* This is not a traditional header file, but rather a "macro" file that defines
 * a template for a decompression function.  (If this were C++, we would
 * actually use tempaltes.)  In general, there are many flavors of the
 * decompression functionality which differ only slightly.  Rather than maintain
 * lots of different code bases or try to debug big macros, we just include this
 * file with various parameters.
 *
 * The following macros must be defined:
 *      CCC_FRONT_COMPRESSED_IMAGE - compressed image to blend in front.
 *      CCC_BACK_COMPRESSED_IMAGE - compressed image to blend in back.
 *      CCC_DEST_COMPRESSED_IMAGE - the resulting compressed image buffer.
 *      CCC_COMPOSITE(front_pointer, back_pointer, dest_pointer) - given
 *              pointers to actual data in the three buffers, perform the
 *              actual compositing operation and increment the pointers.
 *      CCC_FRAGMENT_SIZE - the number of bytes required to store the data
 *              for one fragment, i.e. one color and depth value.
 *
 * Optionally, the macro CCC_LAYERED may be defined to enable support for
 * combining layered images.
 *
 * All of the above macros are undefined at the end of this file.
 */

/* Check for required macros. */
#ifndef ICET_IMAGE_DATA
#error Need ICET_IMAGE_DATA macro.  Is this included in image.c?
#endif
#ifndef INACTIVE_RUN_LENGTH
#error Need INACTIVE_RUN_LENGTH macro.  Is this included in image.c?
#endif
#ifndef ACTIVE_RUN_LENGTH
#error Need ACTIVE_RUN_LENGTH macro.  Is this included in image.c?
#endif
#ifndef ACTIVE_RUN_LENGTH_FRAGMENTS
#error Need ACTIVE_RUN_LENGTH_FRAGMENTS macro.  Is this included in image.c?
#endif

/* Local utility macros. */
#define CCC_MIN(x, y) ((x) < (y) ? (x) : (y))

#ifdef CCC_LAYERED
#define CCC_RUN_LENGTH_SIZE RUN_LENGTH_SIZE_LAYERED
#else
#define CCC_RUN_LENGTH_SIZE RUN_LENGTH_SIZE
#endif

{
    /* Use IceTByte for byte-based pointer arithmetic. */
    const IceTByte *_front;
    const IceTByte *_back;
    IceTByte *_dest;
    IceTVoid *_dest_runlengths;
    IceTSizeType _num_pixels;
    IceTSizeType _pixel;
    IceTSizeType _front_num_inactive;
    IceTSizeType _front_num_active;
    IceTSizeType _back_num_inactive;
    IceTSizeType _back_num_active;
    IceTSizeType _dest_num_active;
#ifdef CCC_LAYERED
    /* Sparse layered images do not have a fixed number of fragments per pixels,
     * so they have to be counted separately. */
    IceTSizeType _front_num_active_frags = 0;
    IceTSizeType _back_num_active_frags = 0;
    IceTSizeType _dest_num_active_frags = 0;
#endif

    _num_pixels = icetSparseImageGetNumPixels(CCC_FRONT_COMPRESSED_IMAGE);
    if (_num_pixels != icetSparseImageGetNumPixels(CCC_BACK_COMPRESSED_IMAGE)) {
        icetRaiseError(ICET_SANITY_CHECK_FAIL,
                       "Input buffers do not agree for compressed-compressed"
                       " composite.");
    }
    icetSparseImageSetDimensions(
                           CCC_DEST_COMPRESSED_IMAGE,
                           icetSparseImageGetWidth(CCC_FRONT_COMPRESSED_IMAGE),
                           icetSparseImageGetHeight(CCC_BACK_COMPRESSED_IMAGE));

    _front = ICET_IMAGE_DATA(CCC_FRONT_COMPRESSED_IMAGE);
    _back = ICET_IMAGE_DATA(CCC_BACK_COMPRESSED_IMAGE);
    _dest = ICET_IMAGE_DATA(CCC_DEST_COMPRESSED_IMAGE);
    _dest_runlengths = NULL;

    _pixel = 0;
    _front_num_inactive = _front_num_active = 0;
    _back_num_inactive = _back_num_active = 0;
    _dest_num_active = 0;
    while (_pixel < _num_pixels) {
        /* When num_active is 0, we have exhausted all active pixels and the
           buffer pointer must be pointing to run lengths. */
        while(   (_front_num_active == 0)
              && ((_front_num_inactive + _pixel) < _num_pixels) ) {
            _front_num_inactive += INACTIVE_RUN_LENGTH(_front);
            _front_num_active = ACTIVE_RUN_LENGTH(_front);
#ifdef CCC_LAYERED
            _front_num_active_frags = ACTIVE_RUN_LENGTH_FRAGMENTS(_front);
#endif
            _front += CCC_RUN_LENGTH_SIZE;
        }
        while(   (_back_num_active == 0)
              && ((_back_num_inactive + _pixel) < _num_pixels) ) {
            _back_num_inactive += INACTIVE_RUN_LENGTH(_back);
            _back_num_active = ACTIVE_RUN_LENGTH(_back);
#ifdef CCC_LAYERED
            _back_num_active_frags = ACTIVE_RUN_LENGTH_FRAGMENTS(_back);
#endif
            _back += CCC_RUN_LENGTH_SIZE;
        }

        {
            IceTSizeType _dest_num_inactive
                = CCC_MIN(_front_num_inactive, _back_num_inactive);
            if (_dest_num_inactive > 0) {
                /* Record active pixel count.  (Special case on first iteration
                 * where there is no runlength and no place to put it.) */
                if (_dest_runlengths != NULL) {
                    ACTIVE_RUN_LENGTH(_dest_runlengths) = _dest_num_active;
                    _dest_num_active = 0;
#ifdef CCC_LAYERED
                    ACTIVE_RUN_LENGTH_FRAGMENTS(_dest_runlengths)
                        = _dest_num_active_frags;
                    _dest_num_active_frags = 0;
#endif
                }
                _dest_runlengths = _dest;
                _dest += CCC_RUN_LENGTH_SIZE;
                /* Handle inactive pixel region. */
                _pixel += _dest_num_inactive;
                _front_num_inactive -= _dest_num_inactive;
                _back_num_inactive -= _dest_num_inactive;
                INACTIVE_RUN_LENGTH(_dest_runlengths) = _dest_num_inactive;
            } else {
                /* Handle special case where first pixel is active. */
                if (_dest_runlengths == NULL) {
                    _dest_runlengths = _dest;
                    _dest += CCC_RUN_LENGTH_SIZE;
                    INACTIVE_RUN_LENGTH(_dest_runlengths) = 0;
                }
            }
        }

        /* At this point, either the front or back (or both) have no inactive
           pixels. */

        if ((0 < _front_num_inactive) && (0 < _back_num_active)) {
            IceTSizeType _pixels_to_copy
                = CCC_MIN(_front_num_inactive, _back_num_active);
            size_t _bytes_to_copy;
#ifdef CCC_LAYERED
            IceTSizeType _frags_to_copy;

            if (_pixels_to_copy == _back_num_active) {
                /* When using the rest of the active run, we already know the
                   number of fragments. */
                _frags_to_copy = _back_num_active_frags;
                _bytes_to_copy =  _pixels_to_copy*sizeof(IceTLayerCount)    /* Frags per pixel. */
                                + _frags_to_copy*CCC_FRAGMENT_SIZE;         /* Fragment data. */
            } else {
                /* Otherwise we will have to iterate over the active pixels to
                   count the fragments. */
                const IceTVoid* _new_back = _back;
                icetSparseLayeredImageScanFragments(&_new_back,
                                                    _pixels_to_copy,
                                                    CCC_FRAGMENT_SIZE,
                                                    &_frags_to_copy);
                _bytes_to_copy = (const IceTByte *)_new_back - _back;
            }

            _back_num_active_frags -= _frags_to_copy;
            _dest_num_active_frags += _frags_to_copy;
#else
            _bytes_to_copy = _pixels_to_copy*CCC_FRAGMENT_SIZE;
#endif
            _front_num_inactive -= _pixels_to_copy;
            _back_num_active -= _pixels_to_copy;
            _dest_num_active += _pixels_to_copy;
            _pixel += _pixels_to_copy;
            memcpy(_dest, _back, _bytes_to_copy);
            _dest += _bytes_to_copy;
            _back += _bytes_to_copy;
        }

        if ((0 < _back_num_inactive) && (0 < _front_num_active)) {
            IceTSizeType _pixels_to_copy
                = CCC_MIN(_back_num_inactive, _front_num_active);
            size_t _bytes_to_copy;
#ifdef CCC_LAYERED
            IceTSizeType _frags_to_copy;

            if (_pixels_to_copy == _front_num_active) {
                /* When using the rest of the active run, we already know the
                   number of fragments. */
                _frags_to_copy = _front_num_active_frags;
                _bytes_to_copy =  _pixels_to_copy*sizeof(IceTLayerCount)    /* Frags per pixel. */
                                + _frags_to_copy*CCC_FRAGMENT_SIZE;         /* Fragment data. */
            } else {
                /* Otherwise we will have to iterate over the active pixels to
                   count the fragments. */
                const IceTVoid* _new_front = _front;
                icetSparseLayeredImageScanFragments(&_new_front,
                                                    _pixels_to_copy,
                                                    CCC_FRAGMENT_SIZE,
                                                    &_frags_to_copy);
                _bytes_to_copy = (const IceTByte *)_new_front - _front;
            }

            _front_num_active_frags -= _frags_to_copy;
            _dest_num_active_frags += _frags_to_copy;
#else
            _bytes_to_copy = _pixels_to_copy*CCC_FRAGMENT_SIZE;
#endif
            _back_num_inactive -= _pixels_to_copy;
            _front_num_active -= _pixels_to_copy;
            _dest_num_active += _pixels_to_copy;
            _pixel += _pixels_to_copy;
            memcpy(_dest, _front, _bytes_to_copy);
            _dest += _bytes_to_copy;
            _front += _bytes_to_copy;
        }

        if ((_front_num_inactive == 0) && (_back_num_inactive == 0)) {
            IceTSizeType _num_to_composite
                = CCC_MIN(_front_num_active, _back_num_active);
            _front_num_active -= _num_to_composite;
            _back_num_active -= _num_to_composite;
            _dest_num_active += _num_to_composite;
            _pixel += _num_to_composite;
            for ( ; 0 < _num_to_composite; _num_to_composite--) {
                CCC_COMPOSITE(_front, _back, _dest);
            }
        }
    }

    if (_dest_runlengths != NULL) {
        ACTIVE_RUN_LENGTH(_dest_runlengths) = _dest_num_active;
#ifdef CCC_LAYERED
        ACTIVE_RUN_LENGTH_FRAGMENTS(_dest_runlengths) = _dest_num_active_frags;
#endif
    }

    if (_pixel != _num_pixels) {
        icetRaiseError(ICET_INVALID_VALUE, "Corrupt compressed image.");
    }

    {
        /* Compute the actual number of bytes used to store the image. */
        IceTPointerArithmetic _buffer_begin
            =(IceTPointerArithmetic)ICET_IMAGE_HEADER(CCC_DEST_COMPRESSED_IMAGE);
        IceTPointerArithmetic _buffer_end
            =(IceTPointerArithmetic)_dest;
        IceTPointerArithmetic _compressed_size = _buffer_end - _buffer_begin;
        ICET_IMAGE_HEADER(CCC_DEST_COMPRESSED_IMAGE)
            [ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX]
            = (IceTInt)_compressed_size;
    }
}

/* Undefine local macros. */
#undef CCC_FRONT_COMPRESSED_IMAGE
#undef CCC_BACK_COMPRESSED_IMAGE
#undef CCC_DEST_COMPRESSED_IMAGE
#undef CCC_COMPOSITE
#undef CCC_FRAGMENT_SIZE
#undef CCC_RUN_LENGTH_SIZE
