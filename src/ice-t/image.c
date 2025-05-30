/* -*- c -*- *******************************************************/
/*
 * Copyright (C) 2003 Sandia Corporation
 * Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
 * the U.S. Government retains certain rights in this software.
 *
 * This source code is released under the New BSD License.
 */

#include <IceTDevImage.h>

#include <IceT.h>

#include <IceTDevProjections.h>
#include <IceTDevState.h>
#include <IceTDevDiagnostics.h>
#include <IceTDevMatrix.h>
#include <IceTDevTiming.h>

#include <stdlib.h>
#include <string.h>

#define ICET_IMAGE_MAGIC_NUM            (IceTEnum)0x004D5000
#define ICET_IMAGE_POINTERS_MAGIC_NUM   (IceTEnum)0x004D5100
#define ICET_SPARSE_IMAGE_MAGIC_NUM     (IceTEnum)0x004D6000
/* Flag combined with another magic number to indicate that an image has a
 * layered format, allowing for multiple color and depth values per pixel.
 */
#define ICET_IMAGE_FLAG_LAYERED         (IceTEnum)0x00000001

#define ICET_IMAGE_MAGIC_NUM_INDEX              0
#define ICET_IMAGE_COLOR_FORMAT_INDEX           1
#define ICET_IMAGE_DEPTH_FORMAT_INDEX           2
#define ICET_IMAGE_WIDTH_INDEX                  3
#define ICET_IMAGE_HEIGHT_INDEX                 4
#define ICET_IMAGE_MAX_NUM_PIXELS_INDEX         5
#define ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX     6
#define ICET_IMAGE_DATA_START_INDEX             7

#define ICET_IMAGE_HEADER(image)        ((IceTInt *)image.opaque_internals)
#define ICET_IMAGE_DATA(image) \
    ((IceTVoid *)&(ICET_IMAGE_HEADER(image)[ICET_IMAGE_DATA_START_INDEX]))

/* In addition to the regular header, layered images have a nested sub-header
 * at the start of their data containing metadata specific to layered images.
 * Color and depth are stored after the sub-header either directly in the image
 * buffer or through pointers, just like in non-layered images.
 */
typedef struct IceTLayeredImageHeader {
    IceTLayerCount num_layers;
} IceTLayeredImageHeader;

typedef struct IceTLayeredImageBufferData {
    IceTLayeredImageHeader header;
    IceTUByte data[];
} IceTLayeredImageBufferData;

typedef struct IceTLayeredImagePointerData {
    IceTLayeredImageHeader header;
    const IceTVoid *color_buffer;
    const IceTVoid *depth_buffer;
} IceTLayeredImagePointerData;

typedef IceTUnsignedInt32 IceTRunLengthType;

#define INACTIVE_RUN_LENGTH(rl) (((IceTRunLengthType *)(rl))[0])
#define ACTIVE_RUN_LENGTH(rl)   (((IceTRunLengthType *)(rl))[1])
#define RUN_LENGTH_SIZE         ((IceTSizeType)(2*sizeof(IceTRunLengthType)))
/* Since sparse layered images do not have a fixed number of active fragments
 * per pixel, the number of active fragments in a run must be stored separately,
 * in order for runs to be quickly skipped without having to examine each pixel.
 * Non-layered images do not have this field.
 */
#define ACTIVE_RUN_LENGTH_FRAGMENTS(rl) (((IceTRunLengthType *)(rl))[2])
#define RUN_LENGTH_SIZE_LAYERED         ((IceTSizeType)(3*sizeof(IceTRunLengthType)))

#ifdef DEBUG
static void ICET_TEST_IMAGE_HEADER(IceTImage image)
{
    if (!icetImageIsNull(image)) {
        IceTEnum magic_num =
                ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAGIC_NUM_INDEX];
        /* Allow both layered and non-layered images by ignoring the flag. */
        IceTEnum base_magic_num = magic_num & ~ICET_IMAGE_FLAG_LAYERED;

        if (   (base_magic_num != ICET_IMAGE_MAGIC_NUM)
            && (base_magic_num != ICET_IMAGE_POINTERS_MAGIC_NUM) ) {
            icetRaiseError(ICET_SANITY_CHECK_FAIL,
                           "Detected invalid image header (magic num = 0x%X).",
                           magic_num);
        }
    }
}

/* Check whether an `IceTImage` has a valid magic number indicating a layered
 * format with multiple fragments per pixel.
 */
static void ICET_TEST_LAYERED_IMAGE_HEADER(IceTImage image)
{
    IceTEnum magic_num;

    if (icetImageIsNull(image)) {
        return;
    }

    magic_num = ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAGIC_NUM_INDEX];

    switch (magic_num) {
    case ICET_IMAGE_MAGIC_NUM | ICET_IMAGE_FLAG_LAYERED:
    case ICET_IMAGE_POINTERS_MAGIC_NUM | ICET_IMAGE_FLAG_LAYERED:
        /* Valid layered format. */
        return;
    default:
        icetRaiseError(ICET_SANITY_CHECK_FAIL,
                       "Expected layered image, got magic number %#X",
                       magic_num);
    }
}

static void ICET_TEST_SPARSE_IMAGE_HEADER(IceTSparseImage image)
{
    if (!icetSparseImageIsNull(image)) {
        const IceTEnum magic_num =
            ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAGIC_NUM_INDEX];
        /* Allow both layered and non-layered images by ignoring the flag. */
        const IceTEnum base_magic_num = magic_num & ~ICET_IMAGE_FLAG_LAYERED;
        if (base_magic_num != ICET_SPARSE_IMAGE_MAGIC_NUM ) {
            icetRaiseError(ICET_SANITY_CHECK_FAIL,
                           "Detected invalid image header (magic num = 0x%X).",
                           magic_num);
        }
    }
}
#else /*DEBUG*/
#define ICET_TEST_IMAGE_HEADER(image)
#define ICET_TEST_LAYERED_IMAGE_HEADER(image)
#define ICET_TEST_SPARSE_IMAGE_HEADER(image)
#endif /*DEBUG*/

#ifndef MIN
#define MIN(x, y)       ((x) < (y) ? (x) : (y))
#endif
#ifndef MAX
#define MAX(x, y)       ((x) < (y) ? (y) : (x))
#endif

#define BIT_REVERSE(result, x, max_val_plus_one)                              \
{                                                                             \
    int placeholder;                                                          \
    int input = (x);                                                          \
    (result) = 0;                                                             \
    for (placeholder=0x0001; placeholder<max_val_plus_one; placeholder<<=1) { \
        (result) <<= 1;                                                       \
        (result) += input & 0x0001;                                           \
        input >>= 1;                                                          \
    }                                                                         \
}

#ifdef _MSC_VER
#pragma warning(disable:4055)
#endif

/* Returns the size, in bytes, of a color/depth value for a single fragment. */
static IceTSizeType colorPixelSize(IceTEnum color_format);
static IceTSizeType depthPixelSize(IceTEnum depth_format);

/* Given a sparse image and a pointer to the end of the data, fill in the entry
   for the actual buffer size. */
static void icetSparseImageSetActualSize(IceTSparseImage image,
                                         const IceTVoid *data_end);

/* Given a pointer to a data element in a sparse image data buffer, the amount
 * of inactive pixels before this data element, and the number of active pixels
 * until the next run length, advance the pointer for the number of pixels given
 * and update the inactive and active parameters.  Note that the pointer may
 * point to a run length entry if the number of active is set to zero.  If
 * out_data_p is non-NULL, the data will also be written, in sparse format, to
 * the data pointed there and advanced.  It is up to the calling method to
 * handle the sparse image header.  The parameters mean the following.
 *
 * in_data_p (input/output): Points to the data part of a sparse image.
 *     This is where data will be read from.  When this function returns,
 *     this parameter will be set after the last data point read.
 * inactive_before_p (input/output): The input may be in the middle of a
 *     run length.  The number of inactive to be considered before in_data_p
 *     should be passed here.  Likewise, this parameter will be set based on
 *     where the data lies upon completion.
 * active_till_next_runl_p (input/output): The input may be in the middle of
 *     a run length.  The number of active pixels the advance in_data_p
 *     before getting to the next run length is passed in here.  If this is
 *     set to 0, then in_data_p must point to a run length (or be passed the
 *     edge of the image).  This parameter will be set based on where the
 *     data lies upon completion.
 * last_in_run_length_p (output): If non-NULL, the location of the last run
 *     length read from the input is stored here.  The intention is that
 *     an in-place copy may need to modify this run length.
 * pixels_to_skip (input): The number of pixels to advance (and optionally
 *     copy) in_data_p (and inactive_before_p and active_till_next_runl_p).
 * pixel_size (input): The size, in bytes, for the data of each pixel.
 * out_data_p (input/output): If the intention is to copy the data, this
 *     points to the end of a data part of another sparse image.  The
 *     scanned pixels will be copied to this buffer.  This parameter will
 *     be set to the location after where the portion of data is copied.
 *     This parameter is optional.  If set to NULL, it is ignored and no
 *     data is copied.
 * out_run_length_p (input/output): The output may be in the middle of a
 *     run length.  If so, this parameter can point to the last run length
 *     already in the output.  Likewise, this parameter will be set to point
 *     to the last run length.  This parameter is optional.  If set to NULL,
 *     it is assumed that out_data_p will initially point to a run length.
 *     This parameter is ignored if out_data_p is NULL.
 */
static void icetSparseImageScanPixels(const IceTVoid **in_data_p,
                                      IceTSizeType *inactive_before_p,
                                      IceTSizeType *active_till_next_runl_p,
                                      IceTVoid **last_in_run_length_p,
                                      IceTSizeType pixels_to_skip,
                                      IceTSizeType pixel_size,
                                      IceTVoid **out_data_p,
                                      IceTVoid **out_run_length_p);

/* Similar calling structure as icetSparseImageScanPixels except that the
   data is also copied to out_image. */
static void icetSparseImageCopyPixelsInternal(
                                          const IceTVoid **data_p,
                                          IceTSizeType *inactive_before_p,
                                          IceTSizeType *active_till_next_runl_p,
                                          IceTSizeType pixels_to_copy,
                                          IceTSizeType pixel_size,
                                          IceTSparseImage out_image);

/* Similar to icetSparseImageCopyPixelsInternal except that data_p should be
   pointing to the entry of the data in out_image and the inactive_before and
   active_till_next_runl should be 0.  The pixels in the input (and output since
   they are the same) will be skipped as normal except that the header
   information and last run length for the image will be adjusted so that it is
   equivalent to a copy. */
static void icetSparseImageCopyPixelsInPlaceInternal(
                                          const IceTVoid **data_p,
                                          IceTSizeType *inactive_before_p,
                                          IceTSizeType *active_till_next_runl_p,
                                          IceTSizeType pixels_to_copy,
                                          IceTSizeType pixel_size,
                                          IceTSparseImage out_image);

/* Choose the partitions (defined by offsets) for the given number of partitions
   and size.  The partitions are choosen such that if given a power of 2 as the
   number of partitions, you will get the same partitions if you recursively
   partition the size by 2s.  That is, creating 4 partitions is equivalent to
   creating 2 partitions and then recursively creating 2 more partitions.  If
   the size does not split evenly by 4, the remainder will be divided amongst
   the partitions in the same way. */
static void icetSparseImageSplitChoosePartitions(
                                           IceTInt num_partitions,
                                           IceTSizeType eventual_num_partitions,
                                           IceTSizeType size,
                                           IceTSizeType first_offset,
                                           IceTSizeType *offsets);

/* This function is used to pull a properly centered image from a rendered
   buffer. This function is used by icetGetTileImage to get the final image for
   a tile. When a tile is rendered, it might not be centered in the expected
   location due to, for example, a floating viewport. It might also need part
   of its output cleared out. */
static void getRenderedBufferImage(IceTImage rendered_image,
                                   IceTImage target_image,
                                   IceTInt *rendered_viewport,
                                   IceTInt *target_viewport);

/* This function is used to pull a sparse image from a rendered buffer. This
   function is used by icetGetCompressedTileImage to get the final image for a
   tile. When a tile is rendered, it might not be centered in the expected
   location due to, for example, a floating viewport. */
static IceTSparseImage getCompressedRenderedBufferImage(
        IceTImage rendered_image,
        IceTInt *rendered_viewport,
        IceTInt *target_viewport,
        IceTSizeType tile_width,
        IceTSizeType tile_height);

/* This function is used to get the image for a tile. It will either render
   the tile on demand (with renderTile) or get the image from a pre-rendered
   image (with prerenderedTile). The screen_viewport is set to the region of
   valid pixels in the returned image. The tile_viewport gives the region
   where the pixels reside in the tile. (The width and height of the two
   viewports will be the same.) Pixels outside of these viewports are
   undefined. */
static IceTImage generateTile(int tile,
                              IceTInt *screen_viewport,
                              IceTInt *target_viewport,
                              IceTImage tile_buffer);

/* Renders the geometry for a tile and returns an image of the rendered data.
   If IceT determines that it is most efficient to render the data directly to
   the tile projection, then screen_viewport and tile_viewport will be set to
   the same thing, which is a viewport of the valid pixels in the returned
   image.  Any pixels outside of this viewport are undefined and should be
   cleared to the background before used.  If tile_buffer is not a null image,
   that image will be used to render and be returned.  If IceT determines that
   it is most efficient to render a projection that does not exactly fit a tile,
   tile_buffer will be ignored and image with an internal buffer will be
   returned.  screen_viewport will give the offset and dimensions of the valid
   pixels in the returned buffer.  tile_viewport gives the offset and dimensions
   where these pixels reside in the tile.  (The width and height for both will
   be the same.)  As before, pixels outside of these viewports are undefined. */
static IceTImage renderTile(int tile,
                            IceTInt *screen_viewport,
                            IceTInt *target_viewport,
                            IceTImage tile_buffer);

/* Returns the pre-rendered image, the region of valid pixels in the tile in
   screen_viewport, and the region where the pixels reside in the tile in
   tile_viewport. */
static IceTImage prerenderedTile(int tile,
                                 IceTInt *screen_viewport,
                                 IceTInt *target_viewport);

/* Gets an image buffer attached to this context. */
static IceTImage getRenderBuffer(void);

static IceTSizeType colorPixelSize(IceTEnum color_format)
{
    switch (color_format) {
      case ICET_IMAGE_COLOR_RGBA_UBYTE: return 4;
      case ICET_IMAGE_COLOR_RGBA_FLOAT: return 4*sizeof(IceTFloat);
      case ICET_IMAGE_COLOR_RGB_FLOAT:  return 3*sizeof(IceTFloat);
      case ICET_IMAGE_COLOR_NONE:       return 0;
      default:
          icetRaiseError(ICET_INVALID_ENUM,
                         "Invalid color format 0x%X.", color_format);
          return 0;
    }
}

static IceTSizeType depthPixelSize(IceTEnum depth_format)
{
    switch (depth_format) {
      case ICET_IMAGE_DEPTH_FLOAT: return sizeof(IceTFloat);
      case ICET_IMAGE_DEPTH_NONE:  return 0;
      default:
          icetRaiseError(ICET_INVALID_ENUM,
                         "Invalid depth format 0x%X.", depth_format);
          return 0;
    }
}

IceTSizeType icetImageBufferSize(IceTSizeType width, IceTSizeType height)
{
    IceTEnum color_format, depth_format;

    icetGetEnumv(ICET_COLOR_FORMAT, &color_format);
    icetGetEnumv(ICET_DEPTH_FORMAT, &depth_format);

    return icetImageBufferSizeType(color_format, depth_format,
                                   width, height);
}

IceTSizeType icetImageBufferSizeType(IceTEnum color_format,
                                     IceTEnum depth_format,
                                     IceTSizeType width,
                                     IceTSizeType height)
{
    IceTSizeType color_pixel_size = colorPixelSize(color_format);
    IceTSizeType depth_pixel_size = depthPixelSize(depth_format);

    return (  ICET_IMAGE_DATA_START_INDEX*sizeof(IceTUInt)
            + width*height*(color_pixel_size + depth_pixel_size) );
}

IceTSizeType icetLayeredImageBufferSize(IceTSizeType width,
                                        IceTSizeType height,
                                        IceTLayerCount num_layers)
{
    IceTEnum color_format, depth_format;

    icetGetEnumv(ICET_COLOR_FORMAT, &color_format);
    icetGetEnumv(ICET_DEPTH_FORMAT, &depth_format);

    return icetLayeredImageBufferSizeType(color_format, depth_format,
                                          width, height, num_layers);
}

IceTSizeType icetLayeredImageBufferSizeType(IceTEnum color_format,
                                            IceTEnum depth_format,
                                            IceTSizeType width,
                                            IceTSizeType height,
                                            IceTLayerCount num_layers)
{
    const IceTSizeType fragment_size =
        colorPixelSize(color_format) + depthPixelSize(depth_format);

    return (  ICET_IMAGE_DATA_START_INDEX*sizeof(IceTUInt)  /* Common header. */
            + sizeof(IceTLayeredImageHeader)                /* Layered image sub-header. */
            + width*height*num_layers*fragment_size);       /* Fragments. */
}

IceTSizeType icetImagePointerBufferSize(void)
{
    return (  ICET_IMAGE_DATA_START_INDEX*sizeof(IceTUInt)
            + 2*(sizeof(const IceTVoid *)) );
}

IceTSizeType icetLayeredImagePointerBufferSize(void)
{
    return (  ICET_IMAGE_DATA_START_INDEX*sizeof(IceTUInt)  /* Common header. */
            + sizeof(IceTLayeredImagePointerData));         /* Sub-header and pointers. */
}

IceTSizeType icetSparseImageBufferSize(IceTSizeType width, IceTSizeType height)
{
    IceTEnum color_format, depth_format;

    icetGetEnumv(ICET_COLOR_FORMAT, &color_format);
    icetGetEnumv(ICET_DEPTH_FORMAT, &depth_format);

    return icetSparseImageBufferSizeType(color_format, depth_format,
                                         width, height);
}

IceTSizeType icetSparseImageBufferSizeType(IceTEnum color_format,
                                           IceTEnum depth_format,
                                           IceTSizeType width,
                                           IceTSizeType height)
{
    IceTSizeType size;
    IceTSizeType pixel_size;

    /* A sparse image full of active pixels will be the same size as a full
       image plus a set of run lengths. */
    size = (  RUN_LENGTH_SIZE
            + icetImageBufferSizeType(color_format,depth_format,width,height) );

    /* For most common image formats, this is as large as the sparse image may
       be.  When the size of the run length pair is no bigger than the size of a
       pixel (the amount of data saved by writing the run lengths), then even in
       the pathological case of every other pixel being active.  However, it is
       possible that the run lengths take more space to store than the pixel
       data.  Thus, if there is an inactive run length of one, it is possible to
       have the data set a little bigger.  It is extremely unlikely to need this
       much memory, but we will have to allocate it just in case.  I suppose we
       could change the compress functions to not allow run lengths of size 1,
       but that could increase the time to compress and would definitely
       increase the complexity of the code. */
    pixel_size = colorPixelSize(color_format) + depthPixelSize(depth_format);
    if (pixel_size < RUN_LENGTH_SIZE) {
        size += (RUN_LENGTH_SIZE - pixel_size)*((width*height+1)/2);
    }
    return size;
}

IceTSizeType icetSparseLayeredImageBufferSize(IceTSizeType width,
                                              IceTSizeType height,
                                              IceTLayerCount num_layers)
{
    IceTEnum color_format, depth_format;

    icetGetEnumv(ICET_COLOR_FORMAT, &color_format);
    icetGetEnumv(ICET_DEPTH_FORMAT, &depth_format);

    return icetSparseLayeredImageBufferSizeType(color_format, depth_format,
                                                width, height, num_layers);
}

IceTSizeType icetSparseLayeredImageBufferSizeType(IceTEnum color_format,
                                                  IceTEnum depth_format,
                                                  IceTSizeType width,
                                                  IceTSizeType height,
                                                  IceTLayerCount num_layers)
{
    /* Each fragment consists of a color and a depth value. */
    const IceTSizeType fragment_size =  colorPixelSize(color_format)
                                      + depthPixelSize(depth_format);
    /* Each pixel starts with the number of active fragments at that pixel,
     * followed by the fragments themselves.  A pixel is largest if all
     * fragments are active. */
    const IceTSizeType pixel_size =  sizeof(IceTLayerCount)
                                   + num_layers*fragment_size;

    /* Usually the maximum size will be that of an image with only active
     * fragments. */
    IceTSizeType size =
          ICET_IMAGE_DATA_START_INDEX*sizeof(IceTUInt)  /* Header. */
        + RUN_LENGTH_SIZE_LAYERED                       /* Run lengths. */
        + width*height*pixel_size;                      /* Active pixels. */

    /* If a set of run lengths is larger than a pixel, the biggest image is one
     * that maximizes the number of runs by alternating between active and
     * inactive pixels. */
    if (pixel_size < RUN_LENGTH_SIZE_LAYERED) {
        size += (RUN_LENGTH_SIZE_LAYERED - pixel_size)*((width*height+1)/2);
    }

    return size;
}

IceTImage icetGetStateBufferImage(IceTEnum pname,
                                  IceTSizeType width,
                                  IceTSizeType height)
{
    IceTVoid *buffer;
    IceTSizeType buffer_size;

    buffer_size = icetImageBufferSize(width, height);
    buffer = icetGetStateBuffer(pname, buffer_size);

    return icetImageAssignBuffer(buffer, width, height);
}

IceTImage icetRetrieveStateImage(IceTEnum pname)
{
    return icetImageUnpackageFromReceive(
                (IceTVoid *)icetUnsafeStateGetBuffer(pname));
}

IceTImage icetGetStatePointerImage(IceTEnum pname,
                                   IceTSizeType width,
                                   IceTSizeType height,
                                   const IceTVoid *color_buffer,
                                   const IceTVoid *depth_buffer)
{
    IceTVoid *buffer;
    IceTSizeType buffer_size;

    buffer_size = icetImagePointerBufferSize();
    buffer = icetGetStateBuffer(pname, buffer_size);

    return icetImagePointerAssignBuffer(
                buffer, width, height, color_buffer, depth_buffer);
}

IceTImage icetGetStatePointerLayeredImage(IceTEnum pname,
                                          IceTSizeType width,
                                          IceTSizeType height,
                                          IceTLayerCount num_layers,
                                          const IceTVoid *color_buffer,
                                          const IceTVoid *depth_buffer)
{
    IceTVoid *buffer;
    IceTSizeType buffer_size;

    buffer_size = icetLayeredImagePointerBufferSize();
    buffer = icetGetStateBuffer(pname, buffer_size);

    return icetLayeredImagePointerAssignBuffer(
                buffer, width, height, num_layers, color_buffer, depth_buffer);
}

IceTImage icetImageAssignBuffer(IceTVoid *buffer,
                                IceTSizeType width,
                                IceTSizeType height)
{
    IceTImage image;
    IceTEnum color_format, depth_format;
    IceTInt *header;

    image.opaque_internals = buffer;

    if (buffer == NULL) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Tried to create image with NULL buffer.");
        return icetImageNull();
    }

    icetGetEnumv(ICET_COLOR_FORMAT, &color_format);
    icetGetEnumv(ICET_DEPTH_FORMAT, &depth_format);

    header = ICET_IMAGE_HEADER(image);

    if (   (color_format != ICET_IMAGE_COLOR_RGBA_UBYTE)
        && (color_format != ICET_IMAGE_COLOR_RGBA_FLOAT)
        && (color_format != ICET_IMAGE_COLOR_RGB_FLOAT)
        && (color_format != ICET_IMAGE_COLOR_NONE) ) {
        icetRaiseError(ICET_INVALID_ENUM,
                       "Invalid color format 0x%X.", color_format);
        color_format = ICET_IMAGE_COLOR_NONE;
    }
    if (   (depth_format != ICET_IMAGE_DEPTH_FLOAT)
        && (depth_format != ICET_IMAGE_DEPTH_NONE) ) {
        icetRaiseError(ICET_INVALID_ENUM,
                       "Invalid depth format 0x%X.", depth_format);
        depth_format = ICET_IMAGE_DEPTH_NONE;
    }

    header[ICET_IMAGE_MAGIC_NUM_INDEX]          = ICET_IMAGE_MAGIC_NUM;
    header[ICET_IMAGE_COLOR_FORMAT_INDEX]       = color_format;
    header[ICET_IMAGE_DEPTH_FORMAT_INDEX]       = depth_format;
    header[ICET_IMAGE_WIDTH_INDEX]              = (IceTInt)width;
    header[ICET_IMAGE_HEIGHT_INDEX]             = (IceTInt)height;
    header[ICET_IMAGE_MAX_NUM_PIXELS_INDEX]     = (IceTInt)(width*height);
    header[ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX]
        = (IceTInt)icetImageBufferSizeType(color_format,
                                           depth_format,
                                           width,
                                           height);

    return image;
}

IceTImage icetImagePointerAssignBuffer(IceTVoid *buffer,
                                       IceTSizeType width,
                                       IceTSizeType height,
                                       const IceTVoid *color_buffer,
                                       const IceTVoid *depth_buffer)
{
    /* This is a bit hacky, but most of the entries for regular images and
     * pointer images are the same. Use that function to fill in most of
     * the entries and fix those that are different. */
    IceTImage image = icetImageAssignBuffer(buffer, width, height);

    {
        IceTInt *header = ICET_IMAGE_HEADER(image);
        /* Our magic number is different. */
        header[ICET_IMAGE_MAGIC_NUM_INDEX] = ICET_IMAGE_POINTERS_MAGIC_NUM;
        /* It is invalid to use this type of image as a single buffer. */
        header[ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX] = -1;
    }

    /* Check that the image buffers make sense. */
    if (icetImageGetColorFormat(image) == ICET_IMAGE_COLOR_NONE) {
        if (color_buffer != NULL) {
            icetRaiseError(
                ICET_INVALID_VALUE,
                "Given a color buffer when color format is set to none.");
        }
    } else {
        if (color_buffer == NULL) {
            icetRaiseError(
                ICET_INVALID_VALUE,
                "Not given a color buffer when color format requires one.");
        }
    }
    if (icetImageGetDepthFormat(image) == ICET_IMAGE_DEPTH_NONE) {
        if (depth_buffer != NULL) {
            icetRaiseError(
                ICET_INVALID_VALUE,
                "Given a depth buffer when depth format is set to none.");
        }
    } else {
        if (depth_buffer == NULL) {
            icetRaiseError(
                ICET_INVALID_VALUE,
                "Not given a depth buffer when depth format requires one.");
        }
    }

    {
        const IceTVoid **data = ICET_IMAGE_DATA(image);
        data[0] = color_buffer;
        data[1] = depth_buffer;
    }

    return image;
}

IceTImage icetLayeredImagePointerAssignBuffer(IceTVoid *buffer,
                                              IceTSizeType width,
                                              IceTSizeType height,
                                              IceTLayerCount num_layers,
                                              const IceTVoid *color_buffer,
                                              const IceTVoid *depth_buffer)
{
    /* Set common header fields. */
    IceTImage image = icetImageAssignBuffer(buffer, width, height);

    /* Set header fields different for layered images. */
    {
        IceTInt *header = ICET_IMAGE_HEADER(image);
        /* Mark image as layered. */
        header[ICET_IMAGE_MAGIC_NUM_INDEX] =
            ICET_IMAGE_POINTERS_MAGIC_NUM | ICET_IMAGE_FLAG_LAYERED;
        /* It is invalid to use this type of image as a single buffer. */
        header[ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX] = -1;
    }

    /* Check color buffer. */
    if (icetImageGetColorFormat(image) == ICET_IMAGE_COLOR_NONE) {
        if (color_buffer != NULL) {
            icetRaiseError(
                ICET_INVALID_VALUE,
                "Given a color buffer when color format is set to none.");
        }
    } else {
        if (color_buffer == NULL) {
            icetRaiseError(
                ICET_INVALID_VALUE,
                "Not given a color buffer when color format requires one.");
        }
    }

    /* Check that there is depth information. */
    if (icetImageGetDepthFormat(image) == ICET_IMAGE_DEPTH_NONE) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Layered images must contain depth information.");
    }
    if (depth_buffer == NULL) {
        icetRaiseError(ICET_INVALID_VALUE, "Missing depth buffer.");
    }

    /* Set sub-header and data. */
    {
        IceTLayeredImagePointerData *data = ICET_IMAGE_DATA(image);
        data->header.num_layers = num_layers;
        data->color_buffer = color_buffer;
        data->depth_buffer = depth_buffer;
    }

    return image;
}

IceTImage icetImageNull(void)
{
    IceTImage image;
    image.opaque_internals = NULL;
    return image;
}

IceTBoolean icetImageIsNull(const IceTImage image)
{
    if (image.opaque_internals == NULL) {
        return ICET_TRUE;
    } else {
        return ICET_FALSE;
    }
}

IceTBoolean icetImageIsLayered(const IceTImage image)
{
    return  ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAGIC_NUM_INDEX]
        & ICET_IMAGE_FLAG_LAYERED;
}

IceTSparseImage icetGetStateBufferSparseImage(IceTEnum pname,
                                              IceTSizeType width,
                                              IceTSizeType height)
{
    IceTVoid *buffer;
    IceTSizeType buffer_size;

    buffer_size = icetSparseImageBufferSize(width, height);
    buffer = icetGetStateBuffer(pname, buffer_size);

    return icetSparseImageAssignBuffer(buffer, width, height);
}

IceTSparseImage icetSparseImageAssignBuffer(IceTVoid *buffer,
                                            IceTSizeType width,
                                            IceTSizeType height)
{
    IceTSparseImage image;
    IceTEnum color_format, depth_format;
    IceTInt *header;

    image.opaque_internals = buffer;

    if (buffer == NULL) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Tried to create sparse image with NULL buffer.");
        return image;
    }

    icetGetEnumv(ICET_COLOR_FORMAT, &color_format);
    icetGetEnumv(ICET_DEPTH_FORMAT, &depth_format);

    header = ICET_IMAGE_HEADER(image);

    if (   (color_format != ICET_IMAGE_COLOR_RGBA_UBYTE)
        && (color_format != ICET_IMAGE_COLOR_RGBA_FLOAT)
        && (color_format != ICET_IMAGE_COLOR_RGB_FLOAT)
        && (color_format != ICET_IMAGE_COLOR_NONE) ) {
        icetRaiseError(ICET_INVALID_ENUM,
                       "Invalid color format 0x%X.", color_format);
        color_format = ICET_IMAGE_COLOR_NONE;
    }
    if (   (depth_format != ICET_IMAGE_DEPTH_FLOAT)
        && (depth_format != ICET_IMAGE_DEPTH_NONE) ) {
        icetRaiseError(ICET_INVALID_ENUM,
                       "Invalid depth format 0x%X.", depth_format);
        depth_format = ICET_IMAGE_DEPTH_NONE;
    }

    header[ICET_IMAGE_MAGIC_NUM_INDEX]          = ICET_SPARSE_IMAGE_MAGIC_NUM;
    header[ICET_IMAGE_COLOR_FORMAT_INDEX]       = color_format;
    header[ICET_IMAGE_DEPTH_FORMAT_INDEX]       = depth_format;
    header[ICET_IMAGE_WIDTH_INDEX]              = (IceTInt)width;
    header[ICET_IMAGE_HEIGHT_INDEX]             = (IceTInt)height;
    header[ICET_IMAGE_MAX_NUM_PIXELS_INDEX]     = (IceTInt)(width*height);
    header[ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX] = 0;

  /* Make sure the runlengths are valid. */
    icetClearSparseImage(image);

    return image;
}

IceTSparseImage icetGetStateBufferSparseLayeredImage(IceTEnum pname,
                                                     IceTSizeType width,
                                                     IceTSizeType height,
                                                     IceTLayerCount num_layers)
{
    IceTVoid *buffer;
    IceTSizeType buffer_size;

    buffer_size = icetSparseLayeredImageBufferSize(width, height, num_layers);
    buffer = icetGetStateBuffer(pname, buffer_size);

    return icetSparseLayeredImageAssignBuffer(buffer, width, height);
}

IceTSparseImage icetSparseLayeredImageAssignBuffer(IceTVoid *buffer,
                                                   IceTSizeType width,
                                                   IceTSizeType height)
{
    IceTSparseImage image;
    IceTEnum color_format, depth_format;

    /* Assign the image buffer and ensure that it is valid. */
    image.opaque_internals = buffer;

    if (buffer == NULL) {
        icetRaiseError(ICET_INVALID_VALUE,
            "Tried to create sparse layered image with NULL buffer.");
        return image;
    }

    /* Validate color and depth format. */
    {
        icetGetEnumv(ICET_COLOR_FORMAT, &color_format);
        icetGetEnumv(ICET_DEPTH_FORMAT, &depth_format);

        switch (color_format) {
        case ICET_IMAGE_COLOR_RGBA_UBYTE:
        case ICET_IMAGE_COLOR_RGBA_FLOAT:
        case ICET_IMAGE_COLOR_RGB_FLOAT:
        case ICET_IMAGE_COLOR_NONE:
            /* Format OK. */
            break;
        default:
            icetRaiseError(ICET_INVALID_ENUM,
                           "Invalid color format %#X.", color_format);
            color_format = ICET_IMAGE_COLOR_NONE;
        }

        switch (depth_format) {
        case ICET_IMAGE_DEPTH_FLOAT:
            /* Format OK. */
            break;
        case ICET_IMAGE_DEPTH_NONE:
            icetRaiseError(ICET_INVALID_VALUE,
                           "Layered images must contain depth information.");
            break;
        default:
            icetRaiseError(ICET_INVALID_ENUM,
                           "Invalid depth format %#X.", depth_format);
            depth_format = ICET_IMAGE_DEPTH_NONE;
        }
    }

    /* Set metadata. */
    {
        IceTInt *const header = ICET_IMAGE_HEADER(image);

        header[ICET_IMAGE_MAGIC_NUM_INDEX] =  ICET_SPARSE_IMAGE_MAGIC_NUM
                                            | ICET_IMAGE_FLAG_LAYERED;
        header[ICET_IMAGE_COLOR_FORMAT_INDEX]       = color_format;
        header[ICET_IMAGE_DEPTH_FORMAT_INDEX]       = depth_format;
        header[ICET_IMAGE_WIDTH_INDEX]              = (IceTInt)width;
        header[ICET_IMAGE_HEIGHT_INDEX]             = (IceTInt)height;
        header[ICET_IMAGE_MAX_NUM_PIXELS_INDEX]     = (IceTInt)(width*height);
        header[ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX] = 0;
    }

  /* Make sure the runlengths are valid. */
    icetClearSparseImage(image);

    return image;
}

IceTSparseImage icetSparseImageNull(void)
{
    IceTSparseImage image;
    image.opaque_internals = NULL;
    return image;
}

IceTBoolean icetSparseImageIsNull(const IceTSparseImage image)
{
    if (image.opaque_internals == NULL) {
        return ICET_TRUE;
    } else {
        return ICET_FALSE;
    }
}

IceTBoolean icetSparseImageIsLayered(const IceTSparseImage image)
{
    return  ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAGIC_NUM_INDEX]
          & ICET_IMAGE_FLAG_LAYERED;
}

void icetImageAdjustForOutput(IceTImage image)
{
    IceTEnum color_format;

    if (icetImageIsNull(image)) return;

    ICET_TEST_IMAGE_HEADER(image);

    /* Output images are never layered. */
    ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAGIC_NUM_INDEX] &=
        ~ICET_IMAGE_FLAG_LAYERED;

    if (icetIsEnabled(ICET_COMPOSITE_ONE_BUFFER)) {
        color_format = icetImageGetColorFormat(image);
        if (color_format != ICET_IMAGE_COLOR_NONE) {
          /* Set to no depth information. */
            ICET_IMAGE_HEADER(image)[ICET_IMAGE_DEPTH_FORMAT_INDEX]
                = ICET_IMAGE_DEPTH_NONE;
          /* Reset the image size (changes actual buffer size). */
            icetImageSetDimensions(image,
                                   icetImageGetWidth(image),
                                   icetImageGetHeight(image));
        }
    }
}

void icetImageAdjustForInput(IceTImage image)
{
    IceTEnum color_format, depth_format;

    if (icetImageIsNull(image)) return;

    ICET_TEST_IMAGE_HEADER(image);

    icetGetEnumv(ICET_COLOR_FORMAT, &color_format);
    icetGetEnumv(ICET_DEPTH_FORMAT, &depth_format);

  /* Reset to the specified image format. */
    ICET_IMAGE_HEADER(image)[ICET_IMAGE_COLOR_FORMAT_INDEX] = color_format;
    ICET_IMAGE_HEADER(image)[ICET_IMAGE_DEPTH_FORMAT_INDEX] = depth_format;

  /* Reset the image size (changes actual buffer size). */
    icetImageSetDimensions(image,
                           icetImageGetWidth(image),
                           icetImageGetHeight(image));
}

IceTEnum icetImageGetColorFormat(const IceTImage image)
{
    ICET_TEST_IMAGE_HEADER(image);
    if (!image.opaque_internals) return ICET_IMAGE_COLOR_NONE;
    return ICET_IMAGE_HEADER(image)[ICET_IMAGE_COLOR_FORMAT_INDEX];
}
IceTEnum icetImageGetDepthFormat(const IceTImage image)
{
    ICET_TEST_IMAGE_HEADER(image);
    if (!image.opaque_internals) return ICET_IMAGE_DEPTH_NONE;
    return ICET_IMAGE_HEADER(image)[ICET_IMAGE_DEPTH_FORMAT_INDEX];
}
IceTSizeType icetImageGetWidth(const IceTImage image)
{
    ICET_TEST_IMAGE_HEADER(image);
    if (!image.opaque_internals) return 0;
    return ICET_IMAGE_HEADER(image)[ICET_IMAGE_WIDTH_INDEX];
}
IceTSizeType icetImageGetHeight(const IceTImage image)
{
    ICET_TEST_IMAGE_HEADER(image);
    if (!image.opaque_internals) return 0;
    return ICET_IMAGE_HEADER(image)[ICET_IMAGE_HEIGHT_INDEX];
}
IceTSizeType icetImageGetNumPixels(const IceTImage image)
{
    ICET_TEST_IMAGE_HEADER(image);
    if (!image.opaque_internals) return 0;
    return (  ICET_IMAGE_HEADER(image)[ICET_IMAGE_WIDTH_INDEX]
            * ICET_IMAGE_HEADER(image)[ICET_IMAGE_HEIGHT_INDEX] );
}

const IceTLayeredImageHeader *icetLayeredImageGetHeaderConst(const IceTImage image)
{
    ICET_TEST_LAYERED_IMAGE_HEADER(image);
    if (!image.opaque_internals) return 0;
    return (const IceTLayeredImageHeader *)ICET_IMAGE_DATA(image);
}

IceTEnum icetSparseImageGetColorFormat(const IceTSparseImage image)
{
    ICET_TEST_SPARSE_IMAGE_HEADER(image);
    if (!image.opaque_internals) return ICET_IMAGE_COLOR_NONE;
    return ICET_IMAGE_HEADER(image)[ICET_IMAGE_COLOR_FORMAT_INDEX];
}
IceTEnum icetSparseImageGetDepthFormat(const IceTSparseImage image)
{
    ICET_TEST_SPARSE_IMAGE_HEADER(image);
    if (!image.opaque_internals) return ICET_IMAGE_DEPTH_NONE;
    return ICET_IMAGE_HEADER(image)[ICET_IMAGE_DEPTH_FORMAT_INDEX];
}
IceTSizeType icetSparseImageGetWidth(const IceTSparseImage image)
{
    ICET_TEST_SPARSE_IMAGE_HEADER(image);
    if (!image.opaque_internals) return 0;
    return ICET_IMAGE_HEADER(image)[ICET_IMAGE_WIDTH_INDEX];
}
IceTSizeType icetSparseImageGetHeight(const IceTSparseImage image)
{
    ICET_TEST_SPARSE_IMAGE_HEADER(image);
    if (!image.opaque_internals) return 0;
    return ICET_IMAGE_HEADER(image)[ICET_IMAGE_HEIGHT_INDEX];
}
IceTSizeType icetSparseImageGetNumPixels(const IceTSparseImage image)
{
    ICET_TEST_SPARSE_IMAGE_HEADER(image);
    if (!image.opaque_internals) return 0;
    return (  ICET_IMAGE_HEADER(image)[ICET_IMAGE_WIDTH_INDEX]
            * ICET_IMAGE_HEADER(image)[ICET_IMAGE_HEIGHT_INDEX] );
}
IceTSizeType icetSparseImageGetCompressedBufferSize(
                                             const IceTSparseImage image)
{
    ICET_TEST_SPARSE_IMAGE_HEADER(image);
    if (!image.opaque_internals) return 0;
    return ICET_IMAGE_HEADER(image)[ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX];
}

void icetImageSetDimensions(IceTImage image,
                            IceTSizeType width,
                            IceTSizeType height)
{
    ICET_TEST_IMAGE_HEADER(image);

    if (icetImageIsNull(image)) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Cannot set number of pixels on null image.");
        return;
    }

    if (   width*height
         > ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAX_NUM_PIXELS_INDEX] ){
        icetRaiseError(ICET_INVALID_VALUE,
                       "Cannot set an image size to greater than what the"
                       " image was originally created (%d > %d).",
                       width*height,
                       ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAX_NUM_PIXELS_INDEX]);
        return;
    }

    ICET_IMAGE_HEADER(image)[ICET_IMAGE_WIDTH_INDEX] = (IceTInt)width;
    ICET_IMAGE_HEADER(image)[ICET_IMAGE_HEIGHT_INDEX] = (IceTInt)height;

    switch (ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAGIC_NUM_INDEX]) {
    case ICET_IMAGE_MAGIC_NUM:
        ICET_IMAGE_HEADER(image)[ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX]
              = (IceTInt)icetImageBufferSizeType(icetImageGetColorFormat(image),
                                                 icetImageGetDepthFormat(image),
                                                 width,
                                                 height);
        break;
    case ICET_IMAGE_MAGIC_NUM | ICET_IMAGE_FLAG_LAYERED:
        ICET_IMAGE_HEADER(image)[ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX]
            = (IceTInt)icetLayeredImageBufferSizeType(
                icetImageGetColorFormat(image),
                icetImageGetDepthFormat(image),
                width,
                height,
                icetLayeredImageGetHeaderConst(image)->num_layers);
        break;
    }
}

void icetSparseImageSetDimensions(IceTSparseImage image,
                                  IceTSizeType width,
                                  IceTSizeType height)
{
    ICET_TEST_SPARSE_IMAGE_HEADER(image);

    if (image.opaque_internals == NULL) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Cannot set number of pixels on null image.");
        return;
    }

    if (   width*height
         > ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAX_NUM_PIXELS_INDEX] ){
        icetRaiseError(ICET_INVALID_VALUE,
                       "Cannot set an image size to greater than what the"
                       " image was originally created (%d > %d).",
                       width*height,
                       ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAX_NUM_PIXELS_INDEX]);
        return;
    }

    ICET_IMAGE_HEADER(image)[ICET_IMAGE_WIDTH_INDEX] = (IceTInt)width;
    ICET_IMAGE_HEADER(image)[ICET_IMAGE_HEIGHT_INDEX] = (IceTInt)height;

  /* Make sure the runlengths are valid. */
    icetClearSparseImage(image);
}

static void icetSparseImageSetActualSize(IceTSparseImage image,
                                         const IceTVoid *data_end)
{
    /* Compute the actual number of bytes used to store the image. */
    IceTPointerArithmetic buffer_begin
        =(IceTPointerArithmetic)ICET_IMAGE_HEADER(image);
    IceTPointerArithmetic buffer_end
        =(IceTPointerArithmetic)data_end;
    IceTPointerArithmetic compressed_size = buffer_end - buffer_begin;
    ICET_IMAGE_HEADER(image)[ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX]
        = (IceTInt)compressed_size;
}

const IceTVoid *icetImageGetColorConstVoid(const IceTImage image,
                                           IceTSizeType *pixel_size)
{
    const IceTEnum magic_number =
        ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAGIC_NUM_INDEX];

    if (pixel_size) {
        IceTEnum color_format = icetImageGetColorFormat(image);
        *pixel_size = colorPixelSize(color_format);

        if (icetImageIsLayered(image)) {
            *pixel_size *= icetLayeredImageGetHeaderConst(image)->num_layers;
        }
    }

    switch (magic_number) {
    case ICET_IMAGE_MAGIC_NUM:
        return ICET_IMAGE_DATA(image);
    case ICET_IMAGE_MAGIC_NUM | ICET_IMAGE_FLAG_LAYERED:
        return ((IceTLayeredImageBufferData *)ICET_IMAGE_DATA(image))->data;
    case ICET_IMAGE_POINTERS_MAGIC_NUM:
        return ((const IceTVoid **)ICET_IMAGE_DATA(image))[0];
    case ICET_IMAGE_POINTERS_MAGIC_NUM | ICET_IMAGE_FLAG_LAYERED:
        return ((IceTLayeredImagePointerData *)ICET_IMAGE_DATA(image))->color_buffer;
    default:
        icetRaiseError(ICET_SANITY_CHECK_FAIL,
                       "Detected invalid image header.");
        return NULL;
    }
}
IceTVoid *icetImageGetColorVoid(IceTImage image, IceTSizeType *pixel_size)
{
    const IceTVoid *const_buffer = icetImageGetColorConstVoid(image, pixel_size);

    /* Raise an exception for images made of pointers, which we set as constant
     * since all internally made pointers are single buffers. */
    if (   ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAGIC_NUM_INDEX]
        == ICET_IMAGE_POINTERS_MAGIC_NUM) {
        icetRaiseError(ICET_SANITY_CHECK_FAIL,
                       "Images of pointers are for reading only.");
    }

    /* This const cast is OK because we actually got the pointer from a
       non-const image. */
    return (IceTVoid *)const_buffer;
}

const IceTUByte *icetImageGetColorcub(const IceTImage image)
{
    IceTEnum color_format = icetImageGetColorFormat(image);

    if (color_format != ICET_IMAGE_COLOR_RGBA_UBYTE) {
        icetRaiseError(ICET_INVALID_OPERATION,
                       "Color format 0x%X is not of type ubyte.",
                       color_format);
        return NULL;
    }

    return icetImageGetColorConstVoid(image, NULL);
}
IceTUByte *icetImageGetColorub(IceTImage image)
{
    IceTEnum color_format = icetImageGetColorFormat(image);

    if (color_format != ICET_IMAGE_COLOR_RGBA_UBYTE) {
        icetRaiseError(ICET_INVALID_OPERATION,
                       "Color format 0x%X is not of type ubyte.",
                       color_format);
        return NULL;
    }

    return icetImageGetColorVoid(image, NULL);
}
const IceTUInt *icetImageGetColorcui(const IceTImage image)
{
    return (const IceTUInt *)icetImageGetColorcub(image);
}
IceTUInt *icetImageGetColorui(IceTImage image)
{
    return (IceTUInt *)icetImageGetColorub(image);
}
const IceTFloat *icetImageGetColorcf(const IceTImage image)
{
    IceTEnum color_format = icetImageGetColorFormat(image);

    if (   (color_format != ICET_IMAGE_COLOR_RGBA_FLOAT)
        && (color_format != ICET_IMAGE_COLOR_RGB_FLOAT)) {
        icetRaiseError(ICET_INVALID_OPERATION,
                       "Color format 0x%X is not of type float.",
                       color_format);
        return NULL;
    }

    return icetImageGetColorConstVoid(image, NULL);
}
IceTFloat *icetImageGetColorf(IceTImage image)
{
    IceTEnum color_format = icetImageGetColorFormat(image);

    if (   (color_format != ICET_IMAGE_COLOR_RGBA_FLOAT)
        && (color_format != ICET_IMAGE_COLOR_RGB_FLOAT)) {
        icetRaiseError(ICET_INVALID_OPERATION,
                       "Color format 0x%X is not of type float.",
                       color_format);
        return NULL;
    }

    return icetImageGetColorVoid(image, NULL);
}

const IceTVoid *icetImageGetDepthConstVoid(const IceTImage image,
                                           IceTSizeType *pixel_size)
{
    const IceTEnum magic_number =
        ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAGIC_NUM_INDEX];
    IceTEnum color_format = icetImageGetColorFormat(image);

    if (pixel_size) {
        IceTEnum depth_format = icetImageGetDepthFormat(image);
        *pixel_size = depthPixelSize(depth_format);
    }

    switch (magic_number) {
    case ICET_IMAGE_MAGIC_NUM:
    {
        IceTSizeType color_format_bytes = (  icetImageGetNumPixels(image)
                                           * colorPixelSize(color_format) );

        /* Cast to IceTByte to ensure pointer arithmetic is correct. */
        const IceTByte *image_data_pointer =
                (const IceTByte*)ICET_IMAGE_DATA(image);

        return image_data_pointer + color_format_bytes;
    }
    case ICET_IMAGE_MAGIC_NUM | ICET_IMAGE_FLAG_LAYERED:
    {
        const IceTLayeredImageBufferData *layered_data = ICET_IMAGE_DATA(image);
        IceTSizeType color_format_bytes = (  icetImageGetNumPixels(image)
                                           * colorPixelSize(color_format)
                                           * layered_data->header.num_layers);

        /* Cast to IceTByte to ensure pointer arithmetic is correct. */
        const IceTByte *image_data_pointer =
                (const IceTByte*)(layered_data->data);

        return image_data_pointer + color_format_bytes;
    }
    case ICET_IMAGE_POINTERS_MAGIC_NUM:
        return ((const IceTVoid **)ICET_IMAGE_DATA(image))[1];
    case ICET_IMAGE_POINTERS_MAGIC_NUM | ICET_IMAGE_FLAG_LAYERED:
        return ((IceTLayeredImagePointerData *)ICET_IMAGE_DATA(image))->depth_buffer;
    default:
        icetRaiseError(ICET_SANITY_CHECK_FAIL,
                       "Detected invalid image header (magic_num = 0x%X).",
                       magic_number);
        return NULL;
    }
}
IceTVoid *icetImageGetDepthVoid(IceTImage image, IceTSizeType *pixel_size)
{
    const IceTVoid *const_buffer =icetImageGetDepthConstVoid(image, pixel_size);

    /* Raise an exception for images made of pointers, which we set as constant
     * since all internally made pointers are single buffers. */
    if (   ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAGIC_NUM_INDEX]
        == ICET_IMAGE_POINTERS_MAGIC_NUM) {
        icetRaiseError(ICET_SANITY_CHECK_FAIL,
                       "Images of pointers are for reading only.");
    }

    /* This const cast is OK because we actually got the pointer from a
       non-const image. */
    return (IceTVoid *)const_buffer;
}
const IceTFloat *icetImageGetDepthcf(const IceTImage image)
{
    IceTEnum depth_format = icetImageGetDepthFormat(image);

    if (depth_format != ICET_IMAGE_DEPTH_FLOAT) {
        icetRaiseError(ICET_INVALID_OPERATION,
                       "Depth format is not of type float.");
        return NULL;
    }

    return icetImageGetDepthConstVoid(image, NULL);
}
IceTFloat *icetImageGetDepthf(IceTImage image)
{
    IceTEnum depth_format = icetImageGetDepthFormat(image);

    if (depth_format != ICET_IMAGE_DEPTH_FLOAT) {
        icetRaiseError(ICET_INVALID_OPERATION,
                       "Depth format 0x%X is not of type float.",
                       depth_format);
        return NULL;
    }

    return icetImageGetDepthVoid(image, NULL);
}

/* In a layered image, each pixel may contain multiple fragments, each made up
 * of a depth value and optionally a color.
 * For every combination of color and depth format, define a fragment type and
 * the correspondingly typed fragment buffer accessor function, using a naming
 * scheme based on OpenGL image formats.
 */
#define FRAGMENT_FORMAT(format, color_type, color_channels, depth_type) \
    typedef struct IceTFragment_##format {                              \
        color_type color[color_channels];                               \
        depth_type depth;                                               \
    } IceTFragment_##format;

FRAGMENT_FORMAT(RGBA8_D32F, IceTUnsignedInt8, 4, IceTFloat);
FRAGMENT_FORMAT(RGB32F_D32F, IceTFloat, 3, IceTFloat);
FRAGMENT_FORMAT(RGBA32F_D32F, IceTFloat, 4, IceTFloat);

#undef FRAGMENT_FORMAT

/* Fragment formats with no color must be defined separately, since `color[0]`
 * is not valid in ANSI C.
 */
#define FRAGMENT_FORMAT_NO_COLOR(format, depth_type)    \
    typedef struct IceTFragment_##format {              \
        depth_type depth;                               \
    } IceTFragment_##format;

FRAGMENT_FORMAT_NO_COLOR(D32F, float);

#undef FRAGMENT_FORMAT_NO_COLOR

void icetImageCopyColorub(const IceTImage image,
                          IceTUByte *color_buffer,
                          IceTEnum out_color_format)
{
    IceTEnum in_color_format = icetImageGetColorFormat(image);
    const IceTLayerCount num_layers = icetImageIsLayered(image)
        ? icetLayeredImageGetHeaderConst(image)->num_layers
        : 1;

    if (out_color_format != ICET_IMAGE_COLOR_RGBA_UBYTE) {
        icetRaiseError(ICET_INVALID_ENUM,
                       "Color format 0x%X is not of type ubyte.",
                       in_color_format);
        return;
    }
    if (in_color_format == ICET_IMAGE_COLOR_NONE) {
        icetRaiseError(ICET_INVALID_OPERATION,
                       "Input image has no color data.");
        return;
    }

    if (in_color_format == out_color_format) {
        const IceTUByte *in_buffer = icetImageGetColorcub(image);
        IceTSizeType color_format_bytes = (  icetImageGetNumPixels(image)
                                           * colorPixelSize(in_color_format)
                                           * num_layers );
        memcpy(color_buffer, in_buffer, color_format_bytes);
    } else if (   (in_color_format == ICET_IMAGE_COLOR_RGBA_FLOAT)
               && (out_color_format == ICET_IMAGE_COLOR_RGBA_UBYTE) ) {
        const IceTFloat *in_buffer = icetImageGetColorcf(image);
        IceTSizeType num_fragments = icetImageGetNumPixels(image)*num_layers;
        IceTSizeType i;
        const IceTFloat *in;
        IceTUByte *out;
        for (i = 0, in = in_buffer, out = color_buffer; i < 4*num_fragments;
             i++, in++, out++) {
            out[0] = (IceTUByte)(255*in[0]);
        }
    } else if (   (in_color_format == ICET_IMAGE_COLOR_RGB_FLOAT)
               && (out_color_format == ICET_IMAGE_COLOR_RGBA_UBYTE) ) {
        const IceTFloat *in_buffer = icetImageGetColorcf(image);
        IceTSizeType num_fragments = icetImageGetNumPixels(image)*num_layers;
        IceTSizeType i;
        const IceTFloat *in = in_buffer;
        IceTUByte *out = color_buffer;
        for (i = 0; i < num_fragments; i++) {
            out[0] = (IceTUByte)(255*in[0]);
            out[1] = (IceTUByte)(255*in[1]);
            out[2] = (IceTUByte)(255*in[2]);
            out[3] = (IceTUByte)255;
            in += 3;
            out += 4;
        }
    } else {
        icetRaiseError(ICET_SANITY_CHECK_FAIL,
                       "Encountered unexpected color format combination "
                       "(in format = 0x%X, out format = 0x%X).",
                       in_color_format, out_color_format);
    }
}

void icetImageCopyColorf(const IceTImage image,
                         IceTFloat *color_buffer,
                         IceTEnum out_color_format)
{
    IceTEnum in_color_format = icetImageGetColorFormat(image);
    const IceTLayerCount num_layers = icetImageIsLayered(image)
        ? icetLayeredImageGetHeaderConst(image)->num_layers
        : 1;

    if (   (out_color_format != ICET_IMAGE_COLOR_RGBA_FLOAT)
        && (out_color_format != ICET_IMAGE_COLOR_RGB_FLOAT)) {
        icetRaiseError(ICET_INVALID_ENUM,
                       "Color format 0x%X is not of type float.",
                       out_color_format);
        return;
    }
    if (in_color_format == ICET_IMAGE_COLOR_NONE) {
        icetRaiseError(ICET_INVALID_OPERATION,
                       "Input image has no color data.");
        return;
    }

    if (in_color_format == out_color_format) {
        const IceTFloat *in_buffer = icetImageGetColorcf(image);
        IceTSizeType color_format_bytes = (  icetImageGetNumPixels(image)
                                           * colorPixelSize(in_color_format)
                                           * num_layers );
        memcpy(color_buffer, in_buffer, color_format_bytes);
    } else if (   (in_color_format == ICET_IMAGE_COLOR_RGBA_UBYTE)
               && (out_color_format == ICET_IMAGE_COLOR_RGBA_FLOAT) ) {
        const IceTUByte *in_buffer = icetImageGetColorcub(image);
        IceTSizeType num_fragments = icetImageGetNumPixels(image)*num_layers;
        IceTSizeType i;
        const IceTUByte *in;
        IceTFloat *out;
        for (i = 0, in = in_buffer, out = color_buffer; i < 4*num_fragments;
             i++, in++, out++) {
            out[0] = (IceTFloat)in[0]/255.0f;
        }
    } else if (   (in_color_format == ICET_IMAGE_COLOR_RGBA_UBYTE)
               && (out_color_format == ICET_IMAGE_COLOR_RGB_FLOAT) ) {
        const IceTUByte *in_buffer = icetImageGetColorcub(image);
        IceTSizeType num_fragments = icetImageGetNumPixels(image)*num_layers;
        IceTSizeType i;
        const IceTUByte *in = in_buffer;
        IceTFloat *out = color_buffer;
        for (i = 0; i < num_fragments; i++) {
            out[0] = (IceTFloat)in[0]/255.0f;
            out[1] = (IceTFloat)in[1]/255.0f;
            out[2] = (IceTFloat)in[2]/255.0f;
            in += 4;
            out += 3;
        }
    } else if (   (in_color_format == ICET_IMAGE_COLOR_RGBA_FLOAT)
               && (out_color_format == ICET_IMAGE_COLOR_RGB_FLOAT) ) {
        const IceTFloat *in_buffer = icetImageGetColorcf(image);
        IceTSizeType num_fragments = icetImageGetNumPixels(image)*num_layers;
        IceTSizeType i;
        const IceTFloat *in = in_buffer;
        IceTFloat *out = color_buffer;
        for (i = 0; i < num_fragments; i++) {
            out[0] = (IceTFloat)in[0];
            out[1] = (IceTFloat)in[1];
            out[2] = (IceTFloat)in[2];
            in += 4;
            out += 3;
        }
    } else if (   (in_color_format == ICET_IMAGE_COLOR_RGB_FLOAT)
               && (out_color_format == ICET_IMAGE_COLOR_RGBA_FLOAT) ) {
        const IceTFloat *in_buffer = icetImageGetColorcf(image);
        IceTSizeType num_fragments = icetImageGetNumPixels(image)*num_layers;
        IceTSizeType i;
        const IceTFloat *in = in_buffer;
        IceTFloat *out = color_buffer;
        for (i = 0; i < num_fragments; i++) {
            out[0] = (IceTFloat)in[0];
            out[1] = (IceTFloat)in[1];
            out[2] = (IceTFloat)in[2];
            out[3] = 1.0f;
            in += 3;
            out += 4;
        }
    } else {
        icetRaiseError(ICET_SANITY_CHECK_FAIL,
                       "Unexpected format combination "
                       "(in format = 0x%X, out format = 0x%X).",
                       in_color_format, out_color_format);
    }
}

void icetImageCopyDepthf(const IceTImage image,
                         IceTFloat *depth_buffer,
                         IceTEnum out_depth_format)
{
    IceTEnum in_depth_format = icetImageGetDepthFormat(image);
    const IceTLayerCount num_layers = icetImageIsLayered(image)
        ? icetLayeredImageGetHeaderConst(image)->num_layers
        : 1;

    if (out_depth_format != ICET_IMAGE_DEPTH_FLOAT) {
        icetRaiseError(ICET_INVALID_ENUM,
                       "Depth format 0x%X is not of type float.",
                       out_depth_format);
        return;
    }
    if (in_depth_format == ICET_IMAGE_DEPTH_NONE) {
        icetRaiseError(ICET_INVALID_OPERATION,
                       "Input image has no depth data.");
        return;
    }

  /* Currently only possibility is
     in_color_format == out_color_format == ICET_IMAGE_DEPTH_FLOAT. */
    {
        const IceTFloat *in_buffer = icetImageGetDepthcf(image);
        IceTSizeType depth_format_bytes = (  icetImageGetNumPixels(image)
                                           * depthPixelSize(in_depth_format)
                                           * num_layers );
        memcpy(depth_buffer, in_buffer, depth_format_bytes);
    }
}

IceTBoolean icetImageEqual(const IceTImage image1, const IceTImage image2)
{
    return image1.opaque_internals == image2.opaque_internals;
}

void icetImageSwap(IceTImage *image1, IceTImage *image2)
{
    IceTImage old_image1 = *image1;
    *image1 = *image2;
    *image2 = old_image1;
}

void icetImageCopyPixels(const IceTImage in_image, IceTSizeType in_offset,
                         IceTImage out_image, IceTSizeType out_offset,
                         IceTSizeType num_pixels)
{
    IceTEnum color_format, depth_format;
    const IceTLayerCount in_layers = icetImageIsLayered(in_image)
        ? icetLayeredImageGetHeaderConst(in_image)->num_layers
        : 1;
    const IceTLayerCount out_layers = icetImageIsLayered(out_image)
        ? icetLayeredImageGetHeaderConst(out_image)->num_layers
        : 1;

    color_format = icetImageGetColorFormat(in_image);
    depth_format = icetImageGetDepthFormat(in_image);
    if (   (color_format != icetImageGetColorFormat(out_image))
        || (depth_format != icetImageGetDepthFormat(out_image)) ) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Cannot copy pixels of images with different formats.");
        return;
    }

    if (    (in_offset < 0)
         || (in_offset + num_pixels > icetImageGetNumPixels(in_image)) ) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Pixels to copy are outside of range of source image.");
        return;
    }
    if (    (out_offset < 0)
         || (out_offset + num_pixels > icetImageGetNumPixels(out_image)) ) {
        icetRaiseError(ICET_INVALID_VALUE,
            "Pixels to copy are outside of range of destination image.");
        return;
    }
    if (in_layers != out_layers) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Can only copy pixels between images with the same "
                       "number of layers.");
        return;
    }

    if (color_format != ICET_IMAGE_COLOR_NONE) {
        const IceTByte *in_colors;  /* Use IceTByte for pointer arithmetic */
        IceTByte *out_colors;
        IceTSizeType pixel_size;
        in_colors = icetImageGetColorConstVoid(in_image, &pixel_size);
        out_colors = icetImageGetColorVoid(out_image, NULL);
        memcpy(out_colors + pixel_size*out_offset,
               in_colors + pixel_size*in_offset,
               pixel_size*num_pixels);
    }

    if (depth_format != ICET_IMAGE_DEPTH_NONE) {
        const IceTByte *in_depths;  /* Use IceTByte for pointer arithmetic */
        IceTByte *out_depths;
        IceTSizeType pixel_size;
        in_depths = icetImageGetDepthConstVoid(in_image, &pixel_size);
        out_depths = icetImageGetDepthVoid(out_image, NULL);
        memcpy(out_depths + pixel_size*out_offset,
               in_depths + pixel_size*in_offset,
               pixel_size*num_pixels);
    }
}

void icetImageCopyRegion(const IceTImage in_image,
                         const IceTInt *in_viewport,
                         IceTImage out_image,
                         const IceTInt *out_viewport)
{
    IceTEnum color_format = icetImageGetColorFormat(in_image);
    IceTEnum depth_format = icetImageGetDepthFormat(in_image);
    const IceTLayerCount in_layers = icetImageIsLayered(in_image)
        ? icetLayeredImageGetHeaderConst(in_image)->num_layers
        : 1;
    const IceTLayerCount out_layers = icetImageIsLayered(out_image)
        ? icetLayeredImageGetHeaderConst(out_image)->num_layers
        : 1;

    if (    (color_format != icetImageGetColorFormat(out_image))
         || (depth_format != icetImageGetDepthFormat(out_image)) ) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "icetImageCopyRegion only supports copying images"
                       " of the same format.");
        return;
    }

    if (    (in_viewport[2] != out_viewport[2])
         || (in_viewport[3] != out_viewport[3]) ) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Sizes of input and output regions must be the same.");
        return;
    }

    if (in_layers != out_layers) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Can only copy pixels between images with the same "
                       "number of layers.");
        return;
    }

    if (color_format != ICET_IMAGE_COLOR_NONE) {
        IceTSizeType pixel_size;
        /* Use IceTByte for byte-based pointer arithmetic. */
        const IceTByte *src = icetImageGetColorConstVoid(in_image, &pixel_size);
        IceTByte *dest = icetImageGetColorVoid(out_image, &pixel_size);
        IceTSizeType y;

      /* Advance pointers up to vertical offset. */
        src  += in_viewport[1]*icetImageGetWidth(in_image)*pixel_size;
        dest += out_viewport[1]*icetImageGetWidth(out_image)*pixel_size;

      /* Advance pointers forward to horizontal offset. */
        src  += in_viewport[0]*pixel_size;
        dest += out_viewport[0]*pixel_size;

        for (y = 0; y < in_viewport[3]; y++) {
            memcpy(dest, src, in_viewport[2]*pixel_size);
            src  += icetImageGetWidth(in_image)*pixel_size;
            dest += icetImageGetWidth(out_image)*pixel_size;
        }
    }

    if (depth_format != ICET_IMAGE_DEPTH_NONE) {
        IceTSizeType pixel_size;
        /* Use IceTByte for byte-based pointer arithmetic. */
        const IceTByte *src = icetImageGetDepthConstVoid(in_image, &pixel_size);
        IceTByte *dest = icetImageGetDepthVoid(out_image, &pixel_size);
        IceTSizeType y;

      /* Advance pointers up to vertical offset. */
        src  += in_viewport[1]*icetImageGetWidth(in_image)*pixel_size;
        dest += out_viewport[1]*icetImageGetWidth(out_image)*pixel_size;

      /* Advance pointers forward to horizontal offset. */
        src  += in_viewport[0]*pixel_size;
        dest += out_viewport[0]*pixel_size;

        for (y = 0; y < in_viewport[3]; y++) {
            memcpy(dest, src, in_viewport[2]*pixel_size);
            src  += icetImageGetWidth(in_image)*pixel_size;
            dest += icetImageGetWidth(out_image)*pixel_size;
        }
    }
}

void icetImageClearAroundRegion(IceTImage image, const IceTInt *region)
{
    IceTSizeType width = icetImageGetWidth(image);
    IceTSizeType height = icetImageGetHeight(image);
    IceTLayerCount num_layers = icetImageIsLayered(image)
        ? icetLayeredImageGetHeaderConst(image)->num_layers
        : 1;
    IceTEnum color_format = icetImageGetColorFormat(image);
    IceTEnum depth_format = icetImageGetDepthFormat(image);
    IceTSizeType x, y;
    IceTLayerCount layer;

    if (color_format == ICET_IMAGE_COLOR_RGBA_UBYTE) {
        IceTUInt *color_buffer = icetImageGetColorui(image);
        IceTUInt background_color;

        icetGetIntegerv(ICET_BACKGROUND_COLOR_WORD,(IceTInt*)&background_color);

      /* Clear out bottom. */
        for (y = 0; y < region[1]; y++) {
            for (x = 0; x < width; x++) {
                for (layer = 0; layer < num_layers; layer++) {
                    color_buffer[(y*width + x) * num_layers + layer] =
                        background_color;
                }
            }
        }
      /* Clear out left and right. */
        if ((region[0] > 0) || (region[0]+region[2] < width)) {
            for (y = region[1]; y < region[1]+region[3]; y++) {
                for (x = 0; x < region[0]; x++) {
                    for (layer = 0; layer < num_layers; layer++) {
                        color_buffer[(y*width + x) * num_layers + layer] =
                            background_color;
                    }
                }
                for (x = region[0]+region[2]; x < width; x++) {
                    for (layer = 0; layer < num_layers; layer++) {
                        color_buffer[(y*width + x) * num_layers + layer] =
                            background_color;
                    }
                }
            }
        }
      /* Clear out top. */
        for (y = region[1]+region[3]; y < height; y++) {
            for (x = 0; x < width; x++) {
                for (layer = 0; layer < num_layers; layer++) {
                    color_buffer[(y*width + x) * num_layers + layer] =
                        background_color;
                }
            }
        }
    } else if (color_format == ICET_IMAGE_COLOR_RGBA_FLOAT) {
        IceTFloat *color_buffer = icetImageGetColorf(image);
        IceTFloat background_color[4];

        icetGetFloatv(ICET_BACKGROUND_COLOR, background_color);

      /* Clear out bottom. */
        for (y = 0; y < region[1]; y++) {
            for (x = 0; x < width; x++) {
                for (layer = 0; layer < num_layers; layer++) {
                    const IceTSizeType offset =
                        ((y*width + x) * num_layers + layer) * 4;
                    color_buffer[offset + 0] = background_color[0];
                    color_buffer[offset + 1] = background_color[1];
                    color_buffer[offset + 2] = background_color[2];
                    color_buffer[offset + 3] = background_color[3];
                }
            }
        }
      /* Clear out left and right. */
        if ((region[0] > 0) || (region[0]+region[2] < width)) {
            for (y = region[1]; y < region[1]+region[3]; y++) {
                for (x = 0; x < region[0]; x++) {
                    for (layer = 0; layer < num_layers; layer++) {
                        const IceTSizeType offset =
                            ((y*width + x) * num_layers + layer) * 4;
                        color_buffer[offset + 0] = background_color[0];
                        color_buffer[offset + 1] = background_color[1];
                        color_buffer[offset + 2] = background_color[2];
                        color_buffer[offset + 3] = background_color[3];
                    }
                }
                for (x = region[0]+region[2]; x < width; x++) {
                    for (layer = 0; layer < num_layers; layer++) {
                        const IceTSizeType offset =
                            ((y*width + x) * num_layers + layer) * 4;
                        color_buffer[offset + 0] = background_color[0];
                        color_buffer[offset + 1] = background_color[1];
                        color_buffer[offset + 2] = background_color[2];
                        color_buffer[offset + 3] = background_color[3];
                    }
                }
            }
        }
      /* Clear out top. */
        for (y = region[1]+region[3]; y < height; y++) {
            for (x = 0; x < width; x++) {
                for (layer = 0; layer < num_layers; layer++) {
                    const IceTSizeType offset =
                        ((y*width + x) * num_layers + layer) * 4;
                    color_buffer[offset + 0] = background_color[0];
                    color_buffer[offset + 1] = background_color[1];
                    color_buffer[offset + 2] = background_color[2];
                    color_buffer[offset + 3] = background_color[3];
                }
            }
        }
    } else if (color_format == ICET_IMAGE_COLOR_RGB_FLOAT) {
        IceTFloat *color_buffer = icetImageGetColorf(image);
        IceTFloat background_color[4];

        icetGetFloatv(ICET_BACKGROUND_COLOR, background_color);

      /* Clear out bottom. */
        for (y = 0; y < region[1]; y++) {
            for (x = 0; x < width; x++) {
                for (layer = 0; layer < num_layers; layer++) {
                    const IceTSizeType offset =
                        ((y*width + x) * num_layers + layer) * 3;
                    color_buffer[offset + 0] = background_color[0];
                    color_buffer[offset + 1] = background_color[1];
                    color_buffer[offset + 2] = background_color[2];
                }
            }
        }
      /* Clear out left and right. */
        if ((region[0] > 0) || (region[0]+region[2] < width)) {
            for (y = region[1]; y < region[1]+region[3]; y++) {
                for (x = 0; x < region[0]; x++) {
                    for (layer = 0; layer < num_layers; layer++) {
                        const IceTSizeType offset =
                            ((y*width + x) * num_layers + layer) * 3;
                        color_buffer[offset + 0] = background_color[0];
                        color_buffer[offset + 1] = background_color[1];
                        color_buffer[offset + 2] = background_color[2];
                    }
                }
                for (x = region[0]+region[2]; x < width; x++) {
                    for (layer = 0; layer < num_layers; layer++) {
                        const IceTSizeType offset =
                            ((y*width + x) * num_layers + layer) * 3;
                        color_buffer[offset + 0] = background_color[0];
                        color_buffer[offset + 1] = background_color[1];
                        color_buffer[offset + 2] = background_color[2];
                    }
                }
            }
        }
      /* Clear out top. */
        for (y = region[1]+region[3]; y < height; y++) {
            for (x = 0; x < width; x++) {
                for (layer = 0; layer < num_layers; layer++) {
                    const IceTSizeType offset =
                        ((y*width + x) * num_layers + layer) * 3;
                    color_buffer[offset + 0] = background_color[0];
                    color_buffer[offset + 1] = background_color[1];
                    color_buffer[offset + 2] = background_color[2];
                }
            }
        }
    } else if (color_format != ICET_IMAGE_COLOR_NONE) {
        icetRaiseError(ICET_SANITY_CHECK_FAIL,
                       "Invalid color format 0x%X.", color_format);
    }

    if (depth_format == ICET_IMAGE_DEPTH_FLOAT) {
        IceTFloat *depth_buffer = icetImageGetDepthf(image);

      /* Clear out bottom. */
        for (y = 0; y < region[1]; y++) {
            for (x = 0; x < width; x++) {
                for (layer = 0; layer < num_layers; layer++) {
                    depth_buffer[(y*width + x) * num_layers + layer] = 1.0f;
                }
            }
        }
      /* Clear out left and right. */
        if ((region[0] > 0) || (region[0]+region[2] < width)) {
            for (y = region[1]; y < region[1]+region[3]; y++) {
                for (x = 0; x < region[0]; x++) {
                    for (layer = 0; layer < num_layers; layer++) {
                        depth_buffer[(y*width + x) * num_layers + layer] = 1.0f;
                    }
                }
                for (x = region[0]+region[2]; x < width; x++) {
                    for (layer = 0; layer < num_layers; layer++) {
                        depth_buffer[(y*width + x) * num_layers + layer] = 1.0f;
                    }
                }
            }
        }
      /* Clear out top. */
        for (y = region[1]+region[3]; y < height; y++) {
            for (x = 0; x < width; x++) {
                for (layer = 0; layer < num_layers; layer++) {
                    depth_buffer[(y*width + x) * num_layers + layer] = 1.0f;
                }
            }
        }
    } else if (depth_format != ICET_IMAGE_DEPTH_NONE) {
        icetRaiseError(ICET_SANITY_CHECK_FAIL,
                       "Invalid depth format 0x%X.", depth_format);
    }
}

void icetImagePackageForSend(IceTImage image,
                             IceTVoid **buffer, IceTSizeType *size)
{
    IceTSizeType expected_size;

    ICET_TEST_IMAGE_HEADER(image);

    *buffer = image.opaque_internals;
    *size = ICET_IMAGE_HEADER(image)[ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX];

    if (*size < 0) {
        /* Images of pointers have less than zero size to alert they are not
         * real buffers. */
        icetRaiseError(
                 ICET_SANITY_CHECK_FAIL,
                 "Attempting to package an image that is not a single buffer.");
    }

    if (icetImageIsLayered(image)) {
        expected_size = icetLayeredImageBufferSizeType(
            icetImageGetColorFormat(image),
            icetImageGetDepthFormat(image),
            icetImageGetWidth(image),
            icetImageGetHeight(image),
            icetLayeredImageGetHeaderConst(image)->num_layers);
    } else {
        expected_size = icetImageBufferSizeType(icetImageGetColorFormat(image),
                                                icetImageGetDepthFormat(image),
                                                icetImageGetWidth(image),
                                                icetImageGetHeight(image));
    }

    if (*size != expected_size) {
        icetRaiseError(ICET_SANITY_CHECK_FAIL,
                       "Inconsistent buffer size detected.");
    }
}

IceTImage icetImageUnpackageFromReceive(IceTVoid *buffer)
{
    IceTImage image;
    IceTEnum magic_number, base_magic_num;
    IceTEnum color_format, depth_format;
    IceTSizeType buffer_size;

    image.opaque_internals = buffer;

  /* Check the image for validity. */
    magic_number = ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAGIC_NUM_INDEX];
    base_magic_num = magic_number & ~ICET_IMAGE_FLAG_LAYERED;
    if (   (base_magic_num != ICET_IMAGE_MAGIC_NUM)
        && (base_magic_num != ICET_IMAGE_POINTERS_MAGIC_NUM) ) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Invalid image buffer: no magic number (0x%X).",
                       magic_number);
        image.opaque_internals = NULL;
        return image;
    }

    color_format = icetImageGetColorFormat(image);
    if (    (color_format != ICET_IMAGE_COLOR_RGBA_UBYTE)
         && (color_format != ICET_IMAGE_COLOR_RGBA_FLOAT)
         && (color_format != ICET_IMAGE_COLOR_RGB_FLOAT)
         && (color_format != ICET_IMAGE_COLOR_NONE) ) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Invalid image buffer: invalid color format 0x%X.",
                       color_format);
        image.opaque_internals = NULL;
        return image;
    }

    depth_format = icetImageGetDepthFormat(image);
    if (    (depth_format != ICET_IMAGE_DEPTH_FLOAT)
         && (depth_format != ICET_IMAGE_DEPTH_NONE) ) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Invalid image buffer: invalid depth format 0x%X.",
                       depth_format);
        image.opaque_internals = NULL;
        return image;
    }

    buffer_size = ICET_IMAGE_HEADER(image)[ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX];

    switch (magic_number) {
    case ICET_IMAGE_MAGIC_NUM:
        if (   icetImageBufferSizeType(color_format, depth_format,
                                       icetImageGetWidth(image),
                                       icetImageGetHeight(image))
            != buffer_size ) {
            icetRaiseError(ICET_INVALID_VALUE,
                           "Inconsistent sizes in image data.");
            image.opaque_internals = NULL;
            return image;
        }
        break;

    case ICET_IMAGE_MAGIC_NUM | ICET_IMAGE_FLAG_LAYERED:
        if (   buffer_size
            != icetLayeredImageBufferSizeType(
                   color_format,
                   depth_format,
                   icetImageGetWidth(image),
                   icetImageGetHeight(image),
                   icetLayeredImageGetHeaderConst(image)->num_layers)) {
            icetRaiseError(ICET_INVALID_VALUE,
                           "Inconsistent sizes in image data.");
            image.opaque_internals = NULL;
            return image;
        }
        break;

    case ICET_IMAGE_POINTERS_MAGIC_NUM:
    case ICET_IMAGE_POINTERS_MAGIC_NUM | ICET_IMAGE_FLAG_LAYERED:
        if (buffer_size != -1) {
            icetRaiseError(ICET_INVALID_VALUE,
                           "Size information not consistent with image type.");
            image.opaque_internals = NULL;
            return image;
        }
        break;
    }

  /* The source may have used a bigger buffer than allocated here at the
     receiver.  Record only size that holds current image. */
    ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAX_NUM_PIXELS_INDEX]
        = (IceTInt)icetImageGetNumPixels(image);

  /* The image is valid (as far as we can tell). */
    return image;
}

void icetSparseImagePackageForSend(IceTSparseImage image,
                                   IceTVoid **buffer, IceTSizeType *size)
{
    ICET_TEST_SPARSE_IMAGE_HEADER(image);

    if (icetSparseImageIsNull(image)) {
        /* Should we return a Null pointer and 0 size without error?
           Would all versions of MPI accept that? */
        icetRaiseError(ICET_INVALID_VALUE,
                       "Cannot package NULL image for send.");
        *buffer = NULL;
        *size = 0;
        return;
    }

    *buffer = image.opaque_internals;
    *size = ICET_IMAGE_HEADER(image)[ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX];
}

IceTSparseImage icetSparseImageUnpackageFromReceive(IceTVoid *buffer)
{
    IceTSparseImage image;
    IceTEnum color_format, depth_format;

    image.opaque_internals = buffer;

  /* Check the image for validity. */
    if (    (ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAGIC_NUM_INDEX]
             & ~ICET_IMAGE_FLAG_LAYERED)
         != ICET_SPARSE_IMAGE_MAGIC_NUM ) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Invalid image buffer: no magic number.");
        image.opaque_internals = NULL;
        return image;
    }

    color_format = icetSparseImageGetColorFormat(image);
    if (    (color_format != ICET_IMAGE_COLOR_RGBA_UBYTE)
         && (color_format != ICET_IMAGE_COLOR_RGBA_FLOAT)
         && (color_format != ICET_IMAGE_COLOR_RGB_FLOAT)
         && (color_format != ICET_IMAGE_COLOR_NONE) ) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Invalid image buffer: invalid color format 0x%X.",
                       color_format);
        image.opaque_internals = NULL;
        return image;
    }

    depth_format = icetSparseImageGetDepthFormat(image);
    if (    (depth_format != ICET_IMAGE_DEPTH_FLOAT)
         && (depth_format != ICET_IMAGE_DEPTH_NONE) ) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Invalid image buffer: invalid depth format 0x%X.",
                       depth_format);
        image.opaque_internals = NULL;
        return image;
    }

    /* The size of sparse layered images can currently not be checked since
     * calculating their expected size requires the maximum number of layers,
     * which is not stored. */
    if (!icetSparseImageIsLayered(image) &&
           icetSparseImageBufferSizeType(color_format, depth_format,
                                         icetSparseImageGetWidth(image),
                                         icetSparseImageGetHeight(image))
         < ICET_IMAGE_HEADER(image)[ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX] ) {
        icetRaiseError(ICET_INVALID_VALUE, "Inconsistent sizes in image data.");
        image.opaque_internals = NULL;
        return image;
    }

  /* The source may have used a bigger buffer than allocated here at the
     receiver.  Record only size that holds current image. */
    ICET_IMAGE_HEADER(image)[ICET_IMAGE_MAX_NUM_PIXELS_INDEX]
        = (IceTInt)icetSparseImageGetNumPixels(image);

  /* The image is valid (as far as we can tell). */
    return image;
}

IceTBoolean icetSparseImageEqual(const IceTSparseImage image1,
                                 const IceTSparseImage image2)
{
    return image1.opaque_internals == image2.opaque_internals;
}

void icetSparseImageSwap(IceTSparseImage *image1, IceTSparseImage *image2)
{
    IceTSparseImage old_image1 = *image1;
    *image1 = *image2;
    *image2 = old_image1;
}

/* Given a pointer to a pixel in a sparse layered image, iterate over a given
 * number of consecutive pixels (must be in the same active run), counting their
 * fragments.
 */
static void icetSparseLayeredImageScanFragments(const IceTVoid **in_data_p,
                                                IceTSizeType pixels_to_skip,
                                                IceTSizeType fragment_size,
                                                IceTSizeType *num_fragments_p)
{
    const IceTByte *in_data = *in_data_p;
    IceTSizeType num_fragments = 0;

    while (pixels_to_skip > 0) {
        /* Count fragments at this pixel. */
        const IceTLayerCount pixel_frags = *(const IceTLayerCount *)in_data;
        num_fragments += pixel_frags;

        /* Skip pixel. */
        in_data += sizeof(IceTLayerCount) + pixel_frags*fragment_size;
        --pixels_to_skip;
    }

    /* Store output. */
    *in_data_p = in_data;
    *num_fragments_p = num_fragments;
}

static void icetSparseImageScanPixels(const IceTVoid **in_data_p,
                                      IceTSizeType *inactive_before_p,
                                      IceTSizeType *active_till_next_runl_p,
                                      IceTVoid **last_in_run_length_p,
                                      IceTSizeType pixels_to_skip,
                                      IceTSizeType pixel_size,
                                      IceTVoid **out_data_p,
                                      IceTVoid **out_run_length_p)
{
#include "sparse_image_scan_body.h"
}

static void icetSparseLayeredImageScanPixels(
    const IceTVoid **in_data_p,
    IceTSizeType *inactive_before_p,
    IceTSizeType *active_till_next_runl_p,
    IceTSizeType *active_frags_till_next_runl_p,
    IceTVoid **last_in_run_length_p,
    IceTSizeType pixels_to_skip,
    IceTSizeType fragment_size,
    IceTVoid **out_data_p,
    IceTVoid **out_run_length_p)
{
#define SIS_LAYERED
#include "sparse_image_scan_body.h"
}

static void icetSparseImageCopyPixelsInternal(
                                          const IceTVoid **in_data_p,
                                          IceTSizeType *inactive_before_p,
                                          IceTSizeType *active_till_next_runl_p,
                                          IceTSizeType pixels_to_copy,
                                          IceTSizeType pixel_size,
                                          IceTSparseImage out_image)
{
    IceTVoid *out_data = ICET_IMAGE_DATA(out_image);

    icetSparseImageSetDimensions(out_image, pixels_to_copy, 1);

    icetSparseImageScanPixels(in_data_p,
                              inactive_before_p,
                              active_till_next_runl_p,
                              NULL,
                              pixels_to_copy,
                              pixel_size,
                              &out_data,
                              NULL);

    icetSparseImageSetActualSize(out_image, out_data);
}

static void icetSparseLayeredImageCopyPixelsInternal(
    const IceTVoid **in_data_p,
    IceTSizeType *inactive_before_p,
    IceTSizeType *active_till_next_runl_p,
    IceTSizeType *active_frags_till_next_runl_p,
    IceTSizeType pixels_to_copy,
    IceTSizeType pixel_size,
    IceTSparseImage out_image)
{
    IceTVoid *out_data = ICET_IMAGE_DATA(out_image);

    icetSparseImageSetDimensions(out_image, pixels_to_copy, 1);

    icetSparseLayeredImageScanPixels(in_data_p,
                                     inactive_before_p,
                                     active_till_next_runl_p,
                                     active_frags_till_next_runl_p,
                                     NULL,
                                     pixels_to_copy,
                                     pixel_size,
                                     &out_data,
                                     NULL);

    icetSparseImageSetActualSize(out_image, out_data);
}

static void icetSparseImageCopyPixelsInPlaceInternal(
                                          const IceTVoid **in_data_p,
                                          IceTSizeType *inactive_before_p,
                                          IceTSizeType *active_till_next_runl_p,
                                          IceTSizeType pixels_to_copy,
                                          IceTSizeType pixel_size,
                                          IceTSparseImage out_image)
{
    IceTVoid *last_run_length = NULL;

#ifdef DEBUG
    if (   (*in_data_p != ICET_IMAGE_DATA(out_image))
        || (*inactive_before_p != 0)
        || (*active_till_next_runl_p != 0) ) {
        icetRaiseError(ICET_SANITY_CHECK_FAIL,
                       "icetSparseImageCopyPixelsInPlaceInternal not called"
                       " at beginning of buffer.");
    }
#endif

    icetSparseImageScanPixels(in_data_p,
                              inactive_before_p,
                              active_till_next_runl_p,
                              &last_run_length,
                              pixels_to_copy,
                              pixel_size,
                              NULL,
                              NULL);

    ICET_IMAGE_HEADER(out_image)[ICET_IMAGE_WIDTH_INDEX]
        = (IceTInt)pixels_to_copy;
    ICET_IMAGE_HEADER(out_image)[ICET_IMAGE_HEIGHT_INDEX] = (IceTInt)1;

    if (last_run_length != NULL) {
        INACTIVE_RUN_LENGTH(last_run_length) -= *inactive_before_p;
        ACTIVE_RUN_LENGTH(last_run_length) -= *active_till_next_runl_p;
    }

    icetSparseImageSetActualSize(out_image, *in_data_p);
}

static void icetSparseLayeredImageCopyPixelsInPlaceInternal(
                                    const IceTVoid **in_data_p,
                                    IceTSizeType *inactive_before_p,
                                    IceTSizeType *active_till_next_runl_p,
                                    IceTSizeType *active_frags_till_next_runl_p,
                                    IceTSizeType pixels_to_copy,
                                    IceTSizeType pixel_size,
                                    IceTSparseImage out_image)
{
    IceTVoid *last_run_length = NULL;

#ifdef DEBUG
    if (   (*in_data_p != ICET_IMAGE_DATA(out_image))
        || (*inactive_before_p != 0)
        || (*active_till_next_runl_p != 0)
        || (*active_frags_till_next_runl_p != 0)) {
        icetRaiseError(ICET_SANITY_CHECK_FAIL,
                       "icetSparseLayeredImageCopyPixelsInPlaceInternal not"
                       " called at beginning of buffer.");
    }
#endif

    icetSparseLayeredImageScanPixels(in_data_p,
                                     inactive_before_p,
                                     active_till_next_runl_p,
                                     active_frags_till_next_runl_p,
                                     &last_run_length,
                                     pixels_to_copy,
                                     pixel_size,
                                     NULL,
                                     NULL);

    ICET_IMAGE_HEADER(out_image)[ICET_IMAGE_WIDTH_INDEX]
        = (IceTInt)pixels_to_copy;
    ICET_IMAGE_HEADER(out_image)[ICET_IMAGE_HEIGHT_INDEX] = (IceTInt)1;

    if (last_run_length != NULL) {
        INACTIVE_RUN_LENGTH(last_run_length) -= *inactive_before_p;
        ACTIVE_RUN_LENGTH(last_run_length) -= *active_till_next_runl_p;
        ACTIVE_RUN_LENGTH_FRAGMENTS(last_run_length)
            -= *active_frags_till_next_runl_p;
    }

    icetSparseImageSetActualSize(out_image, *in_data_p);
}

void icetSparseImageCopyPixels(const IceTSparseImage in_image,
                               IceTSizeType in_offset,
                               IceTSizeType num_pixels,
                               IceTSparseImage out_image)
{
    IceTEnum color_format;
    IceTEnum depth_format;
    IceTBoolean is_layered;
    IceTSizeType fragment_size;

    const IceTVoid *in_data;
    IceTSizeType start_inactive;
    IceTSizeType start_active;

    icetTimingCompressBegin();

    color_format = icetSparseImageGetColorFormat(in_image);
    depth_format = icetSparseImageGetDepthFormat(in_image);
    is_layered = icetSparseImageIsLayered(in_image);
    if (   (color_format != icetSparseImageGetColorFormat(out_image))
        || (depth_format != icetSparseImageGetDepthFormat(out_image))
        || (is_layered != icetSparseImageIsLayered(out_image)) ) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Cannot copy pixels of images with different formats.");
        icetTimingCompressEnd();
        return;
    }

    if (   (in_offset == 0)
        && (num_pixels == icetSparseImageGetNumPixels(in_image)) ) {
        /* Special case, copying image in its entirety.  Using the standard
         * method will work, but doing a raw data copy can be faster. */
        IceTSizeType bytes_to_copy
            = ICET_IMAGE_HEADER(in_image)[ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX];
        IceTSizeType max_pixels
            = ICET_IMAGE_HEADER(out_image)[ICET_IMAGE_MAX_NUM_PIXELS_INDEX];

        ICET_TEST_SPARSE_IMAGE_HEADER(out_image);

        if (max_pixels < num_pixels) {
            icetRaiseError(ICET_INVALID_VALUE,
                           "Cannot set an image size to greater than what the"
                           " image was originally created.");
            icetTimingCompressEnd();
            return;
        }

        memcpy(ICET_IMAGE_HEADER(out_image),
               ICET_IMAGE_HEADER(in_image),
               bytes_to_copy);

        ICET_IMAGE_HEADER(out_image)[ICET_IMAGE_MAX_NUM_PIXELS_INDEX]
            = max_pixels;

        icetTimingCompressEnd();
        return;
    }

    fragment_size = colorPixelSize(color_format) + depthPixelSize(depth_format);

    in_data = ICET_IMAGE_DATA(in_image);
    start_inactive = start_active = 0;

    if (is_layered) {
        IceTSizeType start_active_frags = 0;

        icetSparseLayeredImageScanPixels(&in_data,
                                         &start_inactive,
                                         &start_active,
                                         &start_active_frags,
                                         NULL,
                                         in_offset,
                                         fragment_size,
                                         NULL,
                                         NULL);

        icetSparseLayeredImageCopyPixelsInternal(&in_data,
                                                 &start_inactive,
                                                 &start_active,
                                                 &start_active_frags,
                                                 num_pixels,
                                                 fragment_size,
                                                 out_image);
    } else {
        icetSparseImageScanPixels(&in_data,
                                  &start_inactive,
                                  &start_active,
                                  NULL,
                                  in_offset,
                                  fragment_size,
                                  NULL,
                                  NULL);

        icetSparseImageCopyPixelsInternal(&in_data,
                                          &start_inactive,
                                          &start_active,
                                          num_pixels,
                                          fragment_size,
                                          out_image);
    }

    icetTimingCompressEnd();
}

IceTSizeType icetSparseImageSplitPartitionNumPixels(
                                                IceTSizeType input_num_pixels,
                                                IceTInt num_partitions,
                                                IceTInt eventual_num_partitions)
{
    IceTInt sub_partitions = eventual_num_partitions/num_partitions;

#ifdef DEBUG
    if (eventual_num_partitions%num_partitions != 0) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "num_partitions not a factor"
                       " of eventual_num_partitions.");
    }
#endif

    return input_num_pixels/num_partitions + sub_partitions;
}

static void icetSparseImageSplitChoosePartitions(
                                                IceTInt num_partitions,
                                                IceTInt eventual_num_partitions,
                                                IceTSizeType size,
                                                IceTSizeType first_offset,
                                                IceTSizeType *offsets)
{
    IceTSizeType remainder = size%eventual_num_partitions;
    IceTInt sub_partitions = eventual_num_partitions/num_partitions;
    IceTSizeType partition_lower_size
        = (size/eventual_num_partitions)*sub_partitions;
    IceTSizeType this_offset = first_offset;
    IceTInt partition_idx;

#ifdef DEBUG
    if (eventual_num_partitions%num_partitions != 0) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "num_partitions not a factor"
                       " of eventual_num_partitions.");
    }
#endif

    for (partition_idx = 0; partition_idx < num_partitions; partition_idx++) {
        offsets[partition_idx] = this_offset;
        this_offset += partition_lower_size;
        if (remainder > sub_partitions) {
            this_offset += sub_partitions;
            remainder -= sub_partitions;
        } else {
            this_offset += remainder;
            remainder = 0;
        }
    }
}

void icetSparseImageSplit(const IceTSparseImage in_image,
                          IceTSizeType in_image_offset,
                          IceTInt num_partitions,
                          IceTInt eventual_num_partitions,
                          IceTSparseImage *out_images,
                          IceTSizeType *offsets)
{
    IceTSizeType total_num_pixels;

    IceTEnum color_format;
    IceTEnum depth_format;
    IceTSizeType fragment_size;
    IceTBoolean is_layered;

    const IceTVoid *in_data;
    IceTSizeType start_inactive;
    IceTSizeType start_active;
    IceTSizeType start_active_frags;

    IceTInt partition;

    icetTimingCompressBegin();

    if (num_partitions < 2) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "It does not make sense to call icetSparseImageSplit"
                       " with less than 2 partitions.");
        icetTimingCompressEnd();
        return;
    }

    total_num_pixels = icetSparseImageGetNumPixels(in_image);

    color_format = icetSparseImageGetColorFormat(in_image);
    depth_format = icetSparseImageGetDepthFormat(in_image);
    fragment_size = colorPixelSize(color_format) + depthPixelSize(depth_format);
    is_layered = icetSparseImageIsLayered(in_image);

    in_data = ICET_IMAGE_DATA(in_image);
    start_inactive = start_active = start_active_frags = 0;

    icetSparseImageSplitChoosePartitions(num_partitions,
                                         eventual_num_partitions,
                                         total_num_pixels,
                                         in_image_offset,
                                         offsets);

    for (partition = 0; partition < num_partitions; partition++) {
        IceTSparseImage out_image = out_images[partition];
        IceTSizeType partition_num_pixels;

        if (   (color_format != icetSparseImageGetColorFormat(out_image))
            || (depth_format != icetSparseImageGetDepthFormat(out_image))
            || (is_layered != icetSparseImageIsLayered(out_image)) ) {
            icetRaiseError(ICET_INVALID_VALUE,
                           "Cannot copy pixels of images with different"
                           " formats.");
            icetTimingCompressEnd();
            return;
        }

        if (partition < num_partitions-1) {
            partition_num_pixels = offsets[partition+1] - offsets[partition];
        } else {
            partition_num_pixels
                = total_num_pixels + in_image_offset - offsets[partition];
        }

        if (icetSparseImageEqual(in_image, out_image)) {
            if (partition == 0) {
                if (is_layered) {
                    icetSparseLayeredImageCopyPixelsInPlaceInternal(
                                                           &in_data,
                                                           &start_inactive,
                                                           &start_active,
                                                           &start_active_frags,
                                                           partition_num_pixels,
                                                           fragment_size,
                                                           out_image);
                } else {
                    icetSparseImageCopyPixelsInPlaceInternal(
                                                           &in_data,
                                                           &start_inactive,
                                                           &start_active,
                                                           partition_num_pixels,
                                                           fragment_size,
                                                           out_image);
                }
            } else {
                icetRaiseError(ICET_INVALID_VALUE,
                               "icetSparseImageSplit copy in place only allowed"
                               " in first partition.");
            }
        } else {
            if (is_layered) {
                icetSparseLayeredImageCopyPixelsInternal(&in_data,
                                                        &start_inactive,
                                                        &start_active,
                                                        &start_active_frags,
                                                        partition_num_pixels,
                                                        fragment_size,
                                                        out_image);
            } else {
                icetSparseImageCopyPixelsInternal(&in_data,
                                                  &start_inactive,
                                                  &start_active,
                                                  partition_num_pixels,
                                                  fragment_size,
                                                  out_image);
            }
        }
    }

#ifdef DEBUG
    if (   (start_inactive != 0)
        || (start_active != 0)
        || (is_layered && start_active_frags != 0) ) {
        icetRaiseError(ICET_SANITY_CHECK_FAIL, "Counting problem.");
    }
#endif

    icetTimingCompressEnd();
}

void icetSparseImageSplitAlloc(const IceTSparseImage in_image,
                               IceTSizeType in_image_offset,
                               IceTInt num_partitions,
                               IceTInt eventual_num_partitions,
                               IceTEnum out_buffer_pname,
                               IceTSparseImage *out_images,
                               IceTSizeType *offsets)
{
    IceTSizeType total_num_pixels;

    IceTEnum color_format;
    IceTEnum depth_format;
    IceTSizeType fragment_size;
    IceTBoolean is_layered;

    const IceTVoid *in_data;
    IceTByte *out_data;
    IceTSparseImage out_image;
#ifdef DEBUG
    const IceTByte *out_buffer;
#endif
    IceTSizeType out_buffer_size;
    IceTSizeType start_inactive;
    IceTSizeType start_active;
    IceTSizeType start_active_frags;

    IceTInt partition = 0;

    icetTimingCompressBegin();

    if (num_partitions < 2) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "It does not make sense to call icetSparseImageSplit"
                       " with less than 2 partitions.");
        icetTimingCompressEnd();
        return;
    }

    total_num_pixels = icetSparseImageGetNumPixels(in_image);

    color_format = icetSparseImageGetColorFormat(in_image);
    depth_format = icetSparseImageGetDepthFormat(in_image);
    fragment_size = colorPixelSize(color_format) + depthPixelSize(depth_format);
    is_layered = icetSparseImageIsLayered(in_image);

    in_data = ICET_IMAGE_DATA(in_image);
    start_inactive = start_active = start_active_frags = 0;

    icetSparseImageSplitChoosePartitions(num_partitions,
                                         eventual_num_partitions,
                                         total_num_pixels,
                                         in_image_offset,
                                         offsets);

    /* Calculate the buffer size required to store all partitions. */
    out_buffer_size =
          /* Header of the first partition, all run lengths and pixels. */
          ICET_IMAGE_HEADER(in_image)[ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX]
        +  (num_partitions - 1) /* For each additional partition: */
          *(  ICET_IMAGE_DATA_START_INDEX*sizeof(IceTInt) /* Header. */
            + RUN_LENGTH_SIZE_LAYERED ); /* Initial run lengths. */

    /* Copy the first partition in place when possible. */
    out_image = out_images[0];

    if (icetSparseImageEqual(in_image, out_image)) {
        IceTSizeType partition_num_pixels;

        /* Safe, because num_partitions >= 2 at this point. */
        partition_num_pixels = offsets[1] - offsets[0];

        if (is_layered) {
            icetSparseLayeredImageCopyPixelsInPlaceInternal(
                                                           &in_data,
                                                           &start_inactive,
                                                           &start_active,
                                                           &start_active_frags,
                                                           partition_num_pixels,
                                                           fragment_size,
                                                           out_image);
        } else {
            icetSparseImageCopyPixelsInPlaceInternal(&in_data,
                                                     &start_inactive,
                                                     &start_active,
                                                     partition_num_pixels,
                                                     fragment_size,
                                                     out_image);
        }

        /* The ouput buffer will not contain the first image. */
        out_buffer_size -=
            ICET_IMAGE_HEADER(out_image)[ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX];
        ++partition;
    }

    /* Allocate output buffer. */
    out_data = icetGetStateBuffer(out_buffer_pname, out_buffer_size);
#ifdef DEBUG
    out_buffer = out_data;
#endif

    for (; partition < num_partitions; partition++) {
        IceTSizeType partition_num_pixels;
        IceTInt *header;

        /* Ensure that no existing image will be overwritten. */
        if (!icetSparseImageIsNull(out_images[partition])) {
            icetRaiseError(ICET_INVALID_VALUE, "Partition images must be null");
        }

        /* Calculate the number of pixels in this partition. */
        if (partition < num_partitions-1) {
            partition_num_pixels = offsets[partition+1] - offsets[partition];
        } else {
            partition_num_pixels
                = total_num_pixels + in_image_offset - offsets[partition];
        }

        /* Allocate a new image in the output buffer. */
        out_image = out_images[partition] = is_layered
            ? icetSparseLayeredImageAssignBuffer(out_data, partition_num_pixels, 1)
            : icetSparseImageAssignBuffer(out_data, partition_num_pixels, 1);

        /* Set output format to match input. */
        header = ICET_IMAGE_HEADER(out_image);
        header[ICET_IMAGE_COLOR_FORMAT_INDEX] = color_format;
        header[ICET_IMAGE_DEPTH_FORMAT_INDEX] = depth_format;

        /* Copy data. */
        if (is_layered) {
            icetSparseLayeredImageCopyPixelsInternal(&in_data,
                                                    &start_inactive,
                                                    &start_active,
                                                    &start_active_frags,
                                                    partition_num_pixels,
                                                    fragment_size,
                                                    out_image);
        } else {
            icetSparseImageCopyPixelsInternal(&in_data,
                                              &start_inactive,
                                              &start_active,
                                              partition_num_pixels,
                                              fragment_size,
                                              out_image);
        }

        /* The out buffer is only large enough for this specific image, so
         * trigger an error when attempting to resize it. */
        header[ICET_IMAGE_MAX_NUM_PIXELS_INDEX] = 0;

        /* Advance write pointer past the partition. */
        out_data += header[ICET_IMAGE_ACTUAL_BUFFER_SIZE_INDEX];

        /* Sanity check to detect buffer overruns. */
#ifdef DEBUG
        if (out_data > out_buffer + out_buffer_size) {
            icetRaiseError(ICET_SANITY_CHECK_FAIL, "Buffer overrun.");
        }
#endif
    }

#ifdef DEBUG
    if (   (start_inactive != 0)
        || (start_active != 0)
        || (is_layered && start_active_frags != 0) ) {
        icetRaiseError(ICET_SANITY_CHECK_FAIL, "Counting problem.");
    }
#endif

    icetTimingCompressEnd();
}

void icetSparseImageInterlace(const IceTSparseImage in_image,
                              IceTInt eventual_num_partitions,
                              IceTEnum scratch_state_buffer,
                              IceTSparseImage out_image)
{
    IceTSizeType num_pixels = icetSparseImageGetNumPixels(in_image);
    IceTEnum color_format = icetSparseImageGetColorFormat(in_image);
    IceTEnum depth_format = icetSparseImageGetDepthFormat(in_image);
    IceTBoolean is_layered = icetSparseImageIsLayered(in_image);
    IceTSizeType lower_partition_size = num_pixels/eventual_num_partitions;
    IceTSizeType remaining_pixels = num_pixels%eventual_num_partitions;
    IceTSizeType fragment_size;
    IceTInt original_partition_idx;
    IceTInt interlaced_partition_idx;
    const IceTVoid **in_data_array;
    IceTSizeType *inactive_before_array;
    IceTSizeType *active_till_next_runl_array;
    /* Only valid when using layered images, do not dereference otherwise! */
    IceTSizeType *active_frags_till_next_runl_array;
    const IceTVoid *in_data;
    IceTVoid *out_data;
    IceTSizeType inactive_before;
    IceTSizeType active_till_next_runl;
    IceTSizeType active_frags_till_next_runl;
    IceTVoid *last_run_length;

    /* Special case, nothing to do. */
    if (eventual_num_partitions < 2) {
        icetSparseImageCopyPixels(in_image, 0, num_pixels, out_image);
        return;
    }

    if (   (color_format != icetSparseImageGetColorFormat(out_image))
        || (depth_format != icetSparseImageGetDepthFormat(out_image))
        || (is_layered != icetSparseImageIsLayered(out_image)) ) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Cannot copy pixels of images with different formats.");
        return;
    }

    icetTimingInterlaceBegin();

    fragment_size = colorPixelSize(color_format) + depthPixelSize(depth_format);

    {
        IceTByte *buffer;
        IceTSizeType buffer_size =
              eventual_num_partitions*sizeof(IceTVoid*)
            + eventual_num_partitions*RUN_LENGTH_SIZE_LAYERED;

        buffer = icetGetStateBuffer(scratch_state_buffer, buffer_size);
        in_data_array = (const IceTVoid **)buffer;
        inactive_before_array
            = (IceTSizeType *)(  buffer
                               + eventual_num_partitions*sizeof(IceTVoid*));
        active_till_next_runl_array
            = inactive_before_array + eventual_num_partitions;
        active_frags_till_next_runl_array
            = active_till_next_runl_array + eventual_num_partitions;
    }

    /* Run through the input data and figure out where each interlaced
       partition needs to read from. */
    in_data = ICET_IMAGE_DATA(in_image);
    inactive_before = 0;
    active_till_next_runl = 0;
    active_frags_till_next_runl = 0;
    for (original_partition_idx = 0;
         original_partition_idx < eventual_num_partitions;
         original_partition_idx++) {
        IceTSizeType pixels_to_skip;

        BIT_REVERSE(interlaced_partition_idx,
                    original_partition_idx,
                    eventual_num_partitions);
        if (eventual_num_partitions <= interlaced_partition_idx) {
            interlaced_partition_idx = original_partition_idx;
        }

        pixels_to_skip = lower_partition_size;
        if (interlaced_partition_idx < remaining_pixels) {
            pixels_to_skip += 1;
        }

        in_data_array[interlaced_partition_idx] = in_data;
        inactive_before_array[interlaced_partition_idx] = inactive_before;
        active_till_next_runl_array[interlaced_partition_idx]
            = active_till_next_runl;

        if (is_layered) {
            active_frags_till_next_runl_array[interlaced_partition_idx]
                = active_frags_till_next_runl;
        }

        if (original_partition_idx < eventual_num_partitions-1) {
            if (is_layered) {
                icetSparseLayeredImageScanPixels((const IceTVoid**)&in_data,
                                                  &inactive_before,
                                                  &active_till_next_runl,
                                                  &active_frags_till_next_runl,
                                                  NULL,
                                                  pixels_to_skip,
                                                  fragment_size,
                                                  NULL,
                                                  NULL);
            } else {
                icetSparseImageScanPixels((const IceTVoid**)&in_data,
                                          &inactive_before,
                                          &active_till_next_runl,
                                          NULL,
                                          pixels_to_skip,
                                          fragment_size,
                                          NULL,
                                          NULL);
            }
        }
    }

    /* Set up output image. */
    icetSparseImageSetDimensions(out_image,
                                 icetSparseImageGetWidth(in_image),
                                 icetSparseImageGetHeight(in_image));
    out_data = ICET_IMAGE_DATA(out_image);
    last_run_length = out_data;

    INACTIVE_RUN_LENGTH(out_data) = 0;
    ACTIVE_RUN_LENGTH(out_data) = 0;

    if (is_layered) {
        ACTIVE_RUN_LENGTH_FRAGMENTS(out_data) = 0;
        out_data = (IceTByte*)out_data + RUN_LENGTH_SIZE_LAYERED;
    } else {
        out_data = (IceTByte*)out_data + RUN_LENGTH_SIZE;
    }

    for (interlaced_partition_idx = 0;
         interlaced_partition_idx < eventual_num_partitions;
         interlaced_partition_idx++) {
        IceTSizeType pixels_left;

        pixels_left = lower_partition_size;
        if (interlaced_partition_idx < remaining_pixels) {
            pixels_left += 1;
        }

        in_data = in_data_array[interlaced_partition_idx];
        inactive_before = inactive_before_array[interlaced_partition_idx];
        active_till_next_runl
            = active_till_next_runl_array[interlaced_partition_idx];

        if (is_layered) {
            active_frags_till_next_runl
                = active_frags_till_next_runl_array[interlaced_partition_idx];

            icetSparseLayeredImageScanPixels((const IceTVoid **)&in_data,
                                             &inactive_before,
                                             &active_till_next_runl,
                                             &active_frags_till_next_runl,
                                             NULL,
                                             pixels_left,
                                             fragment_size,
                                             (IceTVoid **)&out_data,
                                             &last_run_length);
        } else {
            icetSparseImageScanPixels((const IceTVoid **)&in_data,
                                      &inactive_before,
                                      &active_till_next_runl,
                                      NULL,
                                      pixels_left,
                                      fragment_size,
                                      (IceTVoid **)&out_data,
                                      &last_run_length);
        }
    }

    icetSparseImageSetActualSize(out_image, out_data);

    icetTimingInterlaceEnd();
}

IceTSparseImage icetSparseImageInterlaceAlloc(const IceTSparseImage in_image,
                                              IceTInt eventual_num_partitions,
                                              IceTEnum scratch_state_buffer,
                                              IceTEnum out_buffer_pname)
{
    IceTSparseImage out_image;
    IceTInt *header;

    /* Account for additional run lengths at the start of each partition. */
    IceTSizeType out_buffer_size =
          icetSparseImageGetCompressedBufferSize(in_image)
        + eventual_num_partitions*RUN_LENGTH_SIZE_LAYERED;
    IceTVoid *out_buffer = icetGetStateBuffer(out_buffer_pname,
                                              out_buffer_size);

    /* Allocate result image. */
    if (icetSparseImageIsLayered(in_image)) {
        out_image = icetSparseLayeredImageAssignBuffer(
                                            out_buffer,
                                            icetSparseImageGetWidth(in_image),
                                            icetSparseImageGetHeight(in_image));
    } else {
        out_image = icetSparseImageAssignBuffer(
                                            out_buffer,
                                            icetSparseImageGetWidth(in_image),
                                            icetSparseImageGetHeight(in_image));
    }

    header = ICET_IMAGE_HEADER(out_image);

    /* Match image format. */
    header[ICET_IMAGE_COLOR_FORMAT_INDEX]
        = icetSparseImageGetColorFormat(in_image);
    header[ICET_IMAGE_DEPTH_FORMAT_INDEX]
        = icetSparseImageGetDepthFormat(in_image);

    /* Perform interlace. */
    icetSparseImageInterlace(in_image,
                             eventual_num_partitions,
                             scratch_state_buffer,
                             out_image);

    /* The out buffer is large enough for this specific image, but may be too
     * small for images with the same size but more active pixels.  To be safe,
     * trigger an error whenever the image is resized. */
    header[ICET_IMAGE_MAX_NUM_PIXELS_INDEX] = 0;
    return out_image;
}

IceTSizeType icetGetInterlaceOffset(IceTInt partition_index,
                                    IceTInt eventual_num_partitions,
                                    IceTSizeType original_image_size)
{
    IceTSizeType lower_partition_size;
    IceTSizeType remaining_pixels;
    IceTSizeType offset;
    IceTInt original_partition_idx;

    if ((partition_index < 0) || (eventual_num_partitions <= partition_index)) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Invalid partition for interlace offset");
        return 0;
    }

    icetTimingInterlaceBegin();

    lower_partition_size = original_image_size/eventual_num_partitions;
    remaining_pixels = original_image_size%eventual_num_partitions;

    offset = 0;
    for (original_partition_idx = 0;
         original_partition_idx < eventual_num_partitions;
         original_partition_idx++) {
        IceTInt interlaced_partition_idx;
        IceTSizeType partition_size;

        BIT_REVERSE(interlaced_partition_idx,
                    original_partition_idx,
                    eventual_num_partitions);
        if (eventual_num_partitions <= interlaced_partition_idx) {
            interlaced_partition_idx = original_partition_idx;
        }

        if (interlaced_partition_idx == partition_index) {
            /* Found any partitions before this one. */
            icetTimingInterlaceEnd();
            return offset;
        }

        partition_size = lower_partition_size;
        if (interlaced_partition_idx < remaining_pixels) {
            partition_size += 1;
        }

        offset += partition_size;
    }

    /* Should never get here. */
    icetRaiseError(ICET_SANITY_CHECK_FAIL, "Could not find partition index.");
    icetTimingInterlaceEnd();
    return 0;
}

void icetClearImage(IceTImage image)
{
    IceTInt region[4] = {0, 0, 0, 0};
    icetImageClearAroundRegion(image, region);
}

void icetClearSparseImage(IceTSparseImage image)
{
    IceTByte *data;
    IceTVoid *data_end;

    ICET_TEST_SPARSE_IMAGE_HEADER(image);

    if (icetSparseImageIsNull(image)) { return; }

    /* Use IceTByte for byte-based pointer arithmetic. */
    data = ICET_IMAGE_DATA(image);
    INACTIVE_RUN_LENGTH(data) = icetSparseImageGetNumPixels(image);
    ACTIVE_RUN_LENGTH(data) = 0;

    /* Layered images have an additional run length field. */
    if (icetSparseImageIsLayered(image)) {
        ACTIVE_RUN_LENGTH_FRAGMENTS(data) = 0;
        data_end = data+RUN_LENGTH_SIZE_LAYERED;
    } else {
        data_end = data+RUN_LENGTH_SIZE;
    }

    icetSparseImageSetActualSize(image, data_end);
}

void icetSetColorFormat(IceTEnum color_format)
{
    IceTBoolean isDrawing;

    icetGetBooleanv(ICET_IS_DRAWING_FRAME, &isDrawing);
    if (isDrawing) {
        icetRaiseError(ICET_INVALID_OPERATION,
                       "Attempted to change the color format while drawing."
                       " This probably means that you called icetSetColorFormat"
                       " in a drawing callback. You cannot do that. Call this"
                       " function before starting the draw operation.");
        return;
    }

    if (   (color_format == ICET_IMAGE_COLOR_RGBA_UBYTE)
        || (color_format == ICET_IMAGE_COLOR_RGBA_FLOAT)
        || (color_format == ICET_IMAGE_COLOR_RGB_FLOAT)
        || (color_format == ICET_IMAGE_COLOR_NONE) ) {
        icetStateSetInteger(ICET_COLOR_FORMAT, color_format);
    } else {
        icetRaiseError(ICET_INVALID_ENUM, "Invalid IceT color format.");
    }
}

void icetSetDepthFormat(IceTEnum depth_format)
{
    IceTBoolean isDrawing;

    icetGetBooleanv(ICET_IS_DRAWING_FRAME, &isDrawing);
    if (isDrawing) {
        icetRaiseError(ICET_INVALID_OPERATION,
                       "Attempted to change the depth format while drawing."
                       " This probably means that you called icetSetDepthFormat"
                       " in a drawing callback. You cannot do that. Call this"
                       " function before starting the draw operation.");
        return;
    }

    if (   (depth_format == ICET_IMAGE_DEPTH_FLOAT)
        || (depth_format == ICET_IMAGE_DEPTH_NONE) ) {
        icetStateSetInteger(ICET_DEPTH_FORMAT, depth_format);
    } else {
        icetRaiseError(ICET_INVALID_ENUM, "Invalid IceT depth format.");
    }
}

void icetGetTileImage(IceTInt tile, IceTImage image)
{
    IceTInt screen_viewport[4], target_viewport[4];
    const IceTInt *viewports;
    IceTSizeType width, height;
    IceTImage rendered_image;

    viewports = icetUnsafeStateGetInteger(ICET_TILE_VIEWPORTS);
    width = viewports[4*tile+2];
    height = viewports[4*tile+3];
    icetImageSetDimensions(image, width, height);

    rendered_image =
            generateTile(tile, screen_viewport, target_viewport, image);

    getRenderedBufferImage(
        rendered_image, image, screen_viewport, target_viewport);
}

static void getRenderedBufferImage(IceTImage rendered_image,
                                   IceTImage target_image,
                                   IceTInt *rendered_viewport,
                                   IceTInt *target_viewport)
{
    if (*icetUnsafeStateGetBoolean(ICET_RENDER_LAYER_HOLDS_BUFFER))
    {
        IceTVoid* getImagePointer;
        IceTGetRenderedBufferImage getImage;

        icetGetPointerv(ICET_GET_RENDERED_BUFFER_IMAGE, &getImagePointer);
        getImage = (IceTGetRenderedBufferImage)getImagePointer;
        (*getImage)(target_image, rendered_viewport, target_viewport);
        return;
    }

    icetTimingBufferReadBegin();

    if (icetImageEqual(rendered_image, target_image)) {
      /* Check to make sure the screen and target viewports are also equal. */
        if (    (rendered_viewport[0] != target_viewport[0])
             || (rendered_viewport[1] != target_viewport[1])
             || (rendered_viewport[2] != target_viewport[2])
             || (rendered_viewport[3] != target_viewport[3]) ) {
            icetRaiseError(ICET_SANITY_CHECK_FAIL,
                           "Inconsistent values returned from generateTile.");
        }
    } else {
      /* Copy the appropriate part of the image to the output buffer. */
        icetImageCopyRegion(rendered_image, rendered_viewport,
                            target_image, target_viewport);
    }

    icetImageClearAroundRegion(target_image, target_viewport);

    icetTimingBufferReadEnd();
}

IceTSparseImage icetGetCompressedTileImage(IceTInt tile)
{
    IceTInt screen_viewport[4], target_viewport[4];
    IceTImage raw_image;
    const IceTInt *viewports;
    IceTSizeType width, height;

    viewports = icetUnsafeStateGetInteger(ICET_TILE_VIEWPORTS);
    width = viewports[4*tile+2];
    height = viewports[4*tile+3];

    raw_image = generateTile(tile, screen_viewport, target_viewport,
                             icetImageNull());

    if ((target_viewport[2] < 1) || (target_viewport[3] < 1)) {
        /* Tile empty.  Just clear result. */
        IceTSparseImage empty = icetGetStateBufferSparseImage(
                    ICET_SPARSE_TILE_BUFFER, width, height);
        icetClearSparseImage(empty);
        return empty;
    }

    return getCompressedRenderedBufferImage(
        raw_image, screen_viewport, target_viewport, width, height);
}

static IceTSparseImage getCompressedRenderedBufferImage(
        IceTImage rendered_image,
        IceTInt *rendered_viewport,
        IceTInt *target_viewport,
        IceTSizeType tile_width,
        IceTSizeType tile_height)
{
    IceTEnum composite_mode;
    IceTSparseImage sparseImage;

    if (*icetUnsafeStateGetBoolean(ICET_RENDER_LAYER_HOLDS_BUFFER))
    {
        IceTVoid* getImagePointer;
        IceTGetCompressedRenderedBufferImage getImage;

        icetGetPointerv(
            ICET_GET_COMPRESSED_RENDERED_BUFFER_IMAGE, &getImagePointer);
        getImage = (IceTGetCompressedRenderedBufferImage)getImagePointer;
        return (*getImage)(
            rendered_viewport, target_viewport, tile_width, tile_height);
    }

    icetGetEnumv(ICET_COMPOSITE_MODE, &composite_mode);

    /* In general, compressing a layered image produces a layered sparse image.
     * If, however, a commutative compositing operator is used, each input image
     * can immediately be reduced to a single layer. */
    if (   icetImageIsLayered(rendered_image)
        && composite_mode == ICET_COMPOSITE_MODE_BLEND) {
        const IceTLayerCount num_layers =
                    icetLayeredImageGetHeaderConst(rendered_image)->num_layers;
        sparseImage = icetGetStateBufferSparseLayeredImage(
                                                        ICET_SPARSE_TILE_BUFFER,
                                                        tile_width,
                                                        tile_height,
                                                        num_layers);
    } else {
        sparseImage = icetGetStateBufferSparseImage(
                    ICET_SPARSE_TILE_BUFFER, tile_width, tile_height);
    }

    icetCompressImageRegion(rendered_image,
                            rendered_viewport,
                            target_viewport,
                            tile_width,
                            tile_height,
                            sparseImage);

    return sparseImage;
}

void icetCompressImage(const IceTImage image,
                       IceTSparseImage compressed_image)
{
    icetCompressSubImage(image, 0, icetImageGetNumPixels(image),
                         compressed_image);

  /* This is a hack to get the width/height of the compressed image to agree
     with the original image. */
    ICET_IMAGE_HEADER(compressed_image)[ICET_IMAGE_WIDTH_INDEX]
        = (IceTInt)icetImageGetWidth(image);
    ICET_IMAGE_HEADER(compressed_image)[ICET_IMAGE_HEIGHT_INDEX]
        = (IceTInt)icetImageGetHeight(image);
}

void icetCompressSubImage(const IceTImage image,
                          IceTSizeType offset, IceTSizeType pixels,
                          IceTSparseImage compressed_image)
{
    ICET_TEST_IMAGE_HEADER(image);
    ICET_TEST_SPARSE_IMAGE_HEADER(compressed_image);

    icetSparseImageSetDimensions(compressed_image, pixels, 1);

#define INPUT_IMAGE             image
#define OUTPUT_SPARSE_IMAGE     compressed_image
#define OFFSET                  offset
#define PIXEL_COUNT             pixels
#include "compress_func_body.h"
}

void icetCompressImageRegion(const IceTImage source_image,
                             IceTInt *source_viewport,
                             IceTInt *target_viewport,
                             IceTSizeType width,
                             IceTSizeType height,
                             IceTSparseImage compressed_image)
{
    IceTSizeType space_left, space_right, space_bottom, space_top;

    space_left = target_viewport[0];
    space_right = width - target_viewport[2] - space_left;
    space_bottom = target_viewport[1];
    space_top = height - target_viewport[3] - space_bottom;

#define INPUT_IMAGE             source_image
#define OUTPUT_SPARSE_IMAGE     compressed_image
#define PADDING
#define SPACE_BOTTOM            space_bottom
#define SPACE_TOP               space_top
#define SPACE_LEFT              space_left
#define SPACE_RIGHT             space_right
#define FULL_WIDTH              width
#define FULL_HEIGHT             height
#define REGION
#define REGION_OFFSET_X         source_viewport[0]
#define REGION_OFFSET_Y         source_viewport[1]
#define REGION_WIDTH            source_viewport[2]
#define REGION_HEIGHT           source_viewport[3]
#include "compress_func_body.h"
}

void icetDecompressImage(const IceTSparseImage compressed_image,
                         IceTImage image)
{
    icetImageSetDimensions(image,
                           icetSparseImageGetWidth(compressed_image),
                           icetSparseImageGetHeight(compressed_image));

    icetDecompressSubImage(compressed_image, 0, image);
}

void icetDecompressSubImage(const IceTSparseImage compressed_image,
                            IceTSizeType offset,
                            IceTImage image)
{
    ICET_TEST_IMAGE_HEADER(image);
    ICET_TEST_SPARSE_IMAGE_HEADER(compressed_image);

#define INPUT_SPARSE_IMAGE      compressed_image
#define OUTPUT_IMAGE            image
#define TIME_DECOMPRESSION
#define OFFSET                  offset
#define PIXEL_COUNT             icetSparseImageGetNumPixels(compressed_image)
#include "decompress_func_body.h"
}

void icetDecompressImageCorrectBackground(const IceTSparseImage compressed_image,
                                          IceTImage image)
{
    icetImageSetDimensions(image,
                           icetSparseImageGetWidth(compressed_image),
                           icetSparseImageGetHeight(compressed_image));

    icetDecompressSubImageCorrectBackground(compressed_image, 0, image);
}

void icetDecompressSubImageCorrectBackground(
                                         const IceTSparseImage compressed_image,
                                         IceTSizeType offset,
                                         IceTImage image)
{
    IceTBoolean need_correction;

    icetGetBooleanv(ICET_NEED_BACKGROUND_CORRECTION, &need_correction);
    if (!need_correction) {
        /* Do a normal decompress. */
        icetDecompressSubImage(compressed_image, offset, image);
    }

    ICET_TEST_IMAGE_HEADER(image);
    ICET_TEST_SPARSE_IMAGE_HEADER(compressed_image);

#define INPUT_SPARSE_IMAGE      compressed_image
#define OUTPUT_IMAGE            image
#define TIME_DECOMPRESSION
#define OFFSET                  offset
#define PIXEL_COUNT             icetSparseImageGetNumPixels(compressed_image)
#define CORRECT_BACKGROUND
#include "decompress_func_body.h"
}


void icetComposite(IceTImage destBuffer, const IceTImage srcBuffer,
                   int srcOnTop)
{
    IceTSizeType pixels;
    IceTSizeType i;
    IceTEnum composite_mode;
    IceTEnum color_format, depth_format;

    if (icetImageIsLayered(srcBuffer)) {
        icetRaiseError(ICET_INVALID_OPERATION,
                       "icetComposite is not implemented for layered images"
                       " yet. Please composite compressed images instead.");
    }

    pixels = icetImageGetNumPixels(destBuffer);
    if (pixels != icetImageGetNumPixels(srcBuffer)) {
        icetRaiseError(ICET_SANITY_CHECK_FAIL,
                       "Source and destination sizes don't match (%d != %d).",
                       pixels, icetImageGetNumPixels(destBuffer));
        return;
    }

    color_format = icetImageGetColorFormat(destBuffer);
    depth_format = icetImageGetDepthFormat(destBuffer);

    if (   (color_format != icetImageGetColorFormat(srcBuffer))
        || (depth_format != icetImageGetDepthFormat(srcBuffer)) ) {
        icetRaiseError(ICET_SANITY_CHECK_FAIL,
                       "Source and destination types don't match.");
        return;
    }

    icetGetEnumv(ICET_COMPOSITE_MODE, &composite_mode);

    icetTimingBlendBegin();

    if (composite_mode == ICET_COMPOSITE_MODE_Z_BUFFER) {
        if (depth_format == ICET_IMAGE_DEPTH_FLOAT) {
            const IceTFloat *srcDepthBuffer = icetImageGetDepthf(srcBuffer);
            IceTFloat *destDepthBuffer = icetImageGetDepthf(destBuffer);

            if (color_format == ICET_IMAGE_COLOR_RGBA_UBYTE) {
                const IceTUInt *srcColorBuffer=icetImageGetColorui(srcBuffer);
                IceTUInt *destColorBuffer = icetImageGetColorui(destBuffer);
                for (i = 0; i < pixels; i++) {
                    if (srcDepthBuffer[i] < destDepthBuffer[i]) {
                        destDepthBuffer[i] = srcDepthBuffer[i];
                        destColorBuffer[i] = srcColorBuffer[i];
                    }
                }
            } else if (color_format == ICET_IMAGE_COLOR_RGBA_FLOAT) {
                const IceTFloat *srcColorBuffer = icetImageGetColorf(srcBuffer);
                IceTFloat *destColorBuffer = icetImageGetColorf(destBuffer);
                for (i = 0; i < pixels; i++) {
                    if (srcDepthBuffer[i] < destDepthBuffer[i]) {
                        destDepthBuffer[i] = srcDepthBuffer[i];
                        destColorBuffer[4*i+0] = srcColorBuffer[4*i+0];
                        destColorBuffer[4*i+1] = srcColorBuffer[4*i+1];
                        destColorBuffer[4*i+2] = srcColorBuffer[4*i+2];
                        destColorBuffer[4*i+3] = srcColorBuffer[4*i+3];
                    }
                }
            } else if (color_format == ICET_IMAGE_COLOR_RGB_FLOAT) {
                const IceTFloat *srcColorBuffer = icetImageGetColorf(srcBuffer);
                IceTFloat *destColorBuffer = icetImageGetColorf(destBuffer);
                for (i = 0; i < pixels; i++) {
                    if (srcDepthBuffer[i] < destDepthBuffer[i]) {
                        destDepthBuffer[i] = srcDepthBuffer[i];
                        destColorBuffer[3*i+0] = srcColorBuffer[3*i+0];
                        destColorBuffer[3*i+1] = srcColorBuffer[3*i+1];
                        destColorBuffer[3*i+2] = srcColorBuffer[3*i+2];
                    }
                }
            } else if (color_format == ICET_IMAGE_COLOR_NONE) {
                for (i = 0; i < pixels; i++) {
                    if (srcDepthBuffer[i] < destDepthBuffer[i]) {
                        destDepthBuffer[i] = srcDepthBuffer[i];
                    }
                }
            } else {
                icetRaiseError(ICET_SANITY_CHECK_FAIL,
                               "Encountered invalid color format 0x%X.",
                               color_format);
            }
        } else if (depth_format == ICET_IMAGE_DEPTH_NONE) {
            icetRaiseError(ICET_INVALID_OPERATION,
                           "Cannot use Z buffer compositing operation with no"
                           " Z buffer.");
        } else {
            icetRaiseError(ICET_SANITY_CHECK_FAIL,
                           "Encountered invalid depth format 0x%X.",
                           depth_format);
        }
    } else if (composite_mode == ICET_COMPOSITE_MODE_BLEND) {
        if (depth_format != ICET_IMAGE_DEPTH_NONE) {
            icetRaiseWarning(ICET_INVALID_VALUE,
                             "Z buffer ignored during blend composite"
                             " operation.  Output z buffer meaningless.");
        }
        if (color_format == ICET_IMAGE_COLOR_RGBA_UBYTE) {
            const IceTUByte *srcColorBuffer = icetImageGetColorcub(srcBuffer);
            IceTUByte *destColorBuffer = icetImageGetColorub(destBuffer);
            if (srcOnTop) {
                for (i = 0; i < pixels; i++) {
                    ICET_OVER_UBYTE(srcColorBuffer + i*4,
                                    destColorBuffer + i*4);
                }
            } else {
                for (i = 0; i < pixels; i++) {
                    ICET_UNDER_UBYTE(srcColorBuffer + i*4,
                                     destColorBuffer + i*4);
                }
            }
        } else if (color_format == ICET_IMAGE_COLOR_RGBA_FLOAT) {
            const IceTFloat *srcColorBuffer = icetImageGetColorcf(srcBuffer);
            IceTFloat *destColorBuffer = icetImageGetColorf(destBuffer);
            if (srcOnTop) {
                for (i = 0; i < pixels; i++) {
                    ICET_OVER_FLOAT(srcColorBuffer + i*4,
                                    destColorBuffer + i*4);
                }
            } else {
                for (i = 0; i < pixels; i++) {
                    ICET_UNDER_FLOAT(srcColorBuffer + i*4,
                                     destColorBuffer + i*4);
                }
            }
        } else if (color_format == ICET_IMAGE_COLOR_RGB_FLOAT) {
            const IceTFloat *srcColorBuffer = icetImageGetColorf(srcBuffer);
            IceTFloat *destColorBuffer = icetImageGetColorf(destBuffer);
            icetRaiseWarning(ICET_INVALID_VALUE,
                             "No alpha channel for blending. "
                             "On top image used.");
            if (srcOnTop) {
                for (i = 0; i < pixels; i++) {
                    destColorBuffer[3*i+0] = srcColorBuffer[3*i+0];
                    destColorBuffer[3*i+1] = srcColorBuffer[3*i+1];
                    destColorBuffer[3*i+2] = srcColorBuffer[3*i+2];
                }
            }
        } else if (color_format == ICET_IMAGE_COLOR_NONE) {
            icetRaiseWarning(ICET_INVALID_OPERATION,
                             "Compositing image with no data.");
        } else {
            icetRaiseError(ICET_SANITY_CHECK_FAIL,
                           "Encountered invalid color format.");
        }
    } else {
        icetRaiseError(ICET_SANITY_CHECK_FAIL,
                       "Encountered invalid composite mode.");
    }

    icetTimingBlendEnd();
}

void icetCompressedComposite(IceTImage destBuffer,
                             const IceTSparseImage srcBuffer,
                             int srcOnTop)
{
    if (    icetImageGetNumPixels(destBuffer)
         != icetSparseImageGetNumPixels(srcBuffer) ) {
        icetRaiseError(ICET_INVALID_VALUE,
                       "Size of input and output buffers do not agree "
                       "(%d != %d).",
                       icetImageGetNumPixels(destBuffer),
                       icetSparseImageGetNumPixels(srcBuffer));
    }
    icetCompressedSubComposite(destBuffer, 0, srcBuffer, srcOnTop);
}
void icetCompressedSubComposite(IceTImage destBuffer,
                                IceTSizeType offset,
                                const IceTSparseImage srcBuffer,
                                int srcOnTop)
{
    icetTimingBlendBegin();

    if (srcOnTop) {
#define INPUT_SPARSE_IMAGE      srcBuffer
#define OUTPUT_IMAGE            destBuffer
#define OFFSET                  offset
#define PIXEL_COUNT             icetSparseImageGetNumPixels(srcBuffer)
#define COMPOSITE
#define BLEND_RGBA_UBYTE        ICET_OVER_UBYTE
#define BLEND_RGBA_FLOAT        ICET_OVER_FLOAT
#include "decompress_func_body.h"
    } else {
#define INPUT_SPARSE_IMAGE      srcBuffer
#define OUTPUT_IMAGE            destBuffer
#define OFFSET                  offset
#define PIXEL_COUNT             icetSparseImageGetNumPixels(srcBuffer)
#define COMPOSITE
#define BLEND_RGBA_UBYTE        ICET_UNDER_UBYTE
#define BLEND_RGBA_FLOAT        ICET_UNDER_FLOAT
#include "decompress_func_body.h"
    }

    icetTimingBlendEnd();
}

void icetCompressedCompressedComposite(const IceTSparseImage front_buffer,
                                       const IceTSparseImage back_buffer,
                                       IceTSparseImage dest_buffer)
{
    if (   icetSparseImageEqual(front_buffer, back_buffer)
        || icetSparseImageEqual(front_buffer, dest_buffer)
        || icetSparseImageEqual(back_buffer, dest_buffer) ) {
        icetRaiseError(ICET_SANITY_CHECK_FAIL,
                       "Detected reused buffer in"
                       " compressed-compressed composite.");
    }

    icetTimingBlendBegin();

#define FRONT_SPARSE_IMAGE front_buffer
#define BACK_SPARSE_IMAGE back_buffer
#define DEST_SPARSE_IMAGE dest_buffer
#include "cc_composite_func_body.h"

    icetTimingBlendEnd();
}

IceTSparseImage icetCompressedCompressedCompositeAlloc(
                                              const IceTSparseImage front_image,
                                              const IceTSparseImage back_image,
                                              IceTEnum dest_buffer_pname)
{
    IceTVoid *dest_buffer;
    IceTSparseImage dest_image;

    /* The largest possible image is one where the active pixels sets of the
     * input images are disjoint. */
    IceTSizeType dest_image_size =
          icetSparseImageGetCompressedBufferSize(front_image)
        + icetSparseImageGetCompressedBufferSize(back_image);

    IceTBoolean is_layered = icetSparseImageIsLayered(front_image);

    if (! is_layered) {
        /* For flat images, overlapping active pixels are blended immediately,
         * so the images' extent can give a tighter upper bound. */
        dest_image_size = MIN(dest_image_size, icetSparseImageBufferSize(
                                        icetSparseImageGetWidth(front_image),
                                        icetSparseImageGetHeight(front_image)));
    }

    /* Initialize the result image in a newly allocated buffer. */
    dest_buffer = icetGetStateBuffer(dest_buffer_pname, dest_image_size);
    dest_image = is_layered
            ? icetSparseLayeredImageAssignBuffer(
                                           dest_buffer,
                                           icetSparseImageGetWidth(front_image),
                                           icetSparseImageGetHeight(back_image))
            : icetSparseImageAssignBuffer(dest_buffer,
                                          icetSparseImageGetWidth(front_image),
                                          icetSparseImageGetHeight(back_image));

    /* Composite into the newly created image. */
    icetCompressedCompressedComposite(front_image, back_image, dest_image);

    /* The result buffer is only guaranteed to be large enough for this specific
     * image, so resizing it should trigger an error. */
    ICET_IMAGE_HEADER(dest_image)[ICET_IMAGE_MAX_NUM_PIXELS_INDEX] = 0;

    return dest_image;
}

void icetImageCorrectBackground(IceTImage image)
{
    IceTBoolean need_correction;
    IceTSizeType num_fragments;
    IceTEnum color_format;

    icetGetBooleanv(ICET_NEED_BACKGROUND_CORRECTION, &need_correction);
    if (!need_correction) { return; }

    num_fragments = icetImageGetNumPixels(image);

    if (icetImageIsLayered(image)) {
        num_fragments *= icetLayeredImageGetHeaderConst(image)->num_layers;
    }

    color_format = icetImageGetColorFormat(image);

    icetTimingBlendBegin();

    if (color_format == ICET_IMAGE_COLOR_RGBA_UBYTE) {
        IceTUByte *color = icetImageGetColorub(image);
        IceTInt background_color_word;
        IceTUByte *bc;
        IceTSizeType p;

        icetGetIntegerv(ICET_TRUE_BACKGROUND_COLOR_WORD,
                        &background_color_word);
        bc = (IceTUByte *)(&background_color_word);

        for (p = 0; p < num_fragments; p++) {
            ICET_UNDER_UBYTE(bc, color);
            color += 4;
        }
    } else if (color_format == ICET_IMAGE_COLOR_RGBA_FLOAT) {
        IceTFloat *color = icetImageGetColorf(image);
        IceTFloat background_color[4];
        IceTSizeType p;

        icetGetFloatv(ICET_TRUE_BACKGROUND_COLOR, background_color);

        for (p = 0; p < num_fragments; p++) {
            ICET_UNDER_FLOAT(background_color, color);
            color += 4;
        }
    } else if (color_format == ICET_IMAGE_COLOR_RGB_FLOAT) {
      /* Nothing to fix. */
    } else {
        icetRaiseError(ICET_SANITY_CHECK_FAIL,
                       "Encountered invalid color buffer type 0x%X"
                       " with color blending.",
                       color_format);
    }

    icetTimingBlendEnd();
}

void icetClearImageTrueBackground(IceTImage image)
{
    IceTFloat true_background[4];
    IceTInt true_background_word;
    IceTFloat original_background[4];
    IceTInt original_background_word;

    icetGetFloatv(ICET_TRUE_BACKGROUND_COLOR, true_background);
    icetGetIntegerv(ICET_TRUE_BACKGROUND_COLOR_WORD, &true_background_word);

    icetGetFloatv(ICET_BACKGROUND_COLOR, original_background);
    icetGetIntegerv(ICET_BACKGROUND_COLOR_WORD, &original_background_word);

    icetStateSetFloatv(ICET_BACKGROUND_COLOR, 4, true_background);
    icetStateSetInteger(ICET_BACKGROUND_COLOR_WORD, true_background_word);

    icetClearImage(image);

    icetStateSetFloatv(ICET_BACKGROUND_COLOR, 4, original_background);
    icetStateSetInteger(ICET_BACKGROUND_COLOR_WORD, original_background_word);
}

static IceTImage generateTile(int tile,
                              IceTInt *screen_viewport,
                              IceTInt *target_viewport,
                              IceTImage tile_buffer)
{
    IceTBoolean use_prerender;
    icetGetBooleanv(ICET_PRE_RENDERED, &use_prerender);
    if (use_prerender) {
        return prerenderedTile(tile, screen_viewport, target_viewport);
    } else {
        return renderTile(tile, screen_viewport, target_viewport, tile_buffer);
    }
}

static IceTImage renderTile(int tile,
                            IceTInt *screen_viewport,
                            IceTInt *target_viewport,
                            IceTImage tile_buffer)
{
    const IceTInt *contained_viewport;
    const IceTInt *tile_viewport;
    const IceTBoolean *contained_mask;
    IceTInt physical_width, physical_height;
    IceTBoolean use_floating_viewport;
    IceTDrawCallbackType drawfunc;
    IceTVoid *value;
    IceTInt readback_viewport[4];
    IceTImage render_buffer;
    IceTDouble projection_matrix[16];
    IceTDouble modelview_matrix[16];
    IceTFloat background_color[4];

    icetRaiseDebug("Rendering tile %d", tile);
    contained_viewport = icetUnsafeStateGetInteger(ICET_CONTAINED_VIEWPORT);
    tile_viewport = icetUnsafeStateGetInteger(ICET_TILE_VIEWPORTS) + 4*tile;
    contained_mask = icetUnsafeStateGetBoolean(ICET_CONTAINED_TILES_MASK);
    use_floating_viewport = icetIsEnabled(ICET_FLOATING_VIEWPORT);

    icetGetIntegerv(ICET_PHYSICAL_RENDER_WIDTH, &physical_width);
    icetGetIntegerv(ICET_PHYSICAL_RENDER_HEIGHT, &physical_height);

    icetRaiseDebug("contained viewport: %d %d %d %d",
                   (int)contained_viewport[0], (int)contained_viewport[1],
                   (int)contained_viewport[2], (int)contained_viewport[3]);
    icetRaiseDebug("tile viewport: %d %d %d %d",
                   (int)tile_viewport[0], (int)tile_viewport[1],
                   (int)tile_viewport[2], (int)tile_viewport[3]);

    render_buffer = tile_buffer;

    if (   !contained_mask[tile]
        || (contained_viewport[0] + contained_viewport[2] < tile_viewport[0])
        || (contained_viewport[1] + contained_viewport[3] < tile_viewport[1])
        || (contained_viewport[0] > tile_viewport[0] + tile_viewport[2])
        || (contained_viewport[1] > tile_viewport[1] + tile_viewport[3]) ) {
      /* Case 0: geometry completely outside tile. */
        icetRaiseDebug("Case 0: geometry completely outside tile.");
        readback_viewport[0] = screen_viewport[0] = target_viewport[0] = 0;
        readback_viewport[1] = screen_viewport[1] = target_viewport[1] = 0;
        readback_viewport[2] = screen_viewport[2] = target_viewport[2] = 0;
        readback_viewport[3] = screen_viewport[3] = target_viewport[3] = 0;
        if (!icetIsEnabled(ICET_RENDER_EMPTY_IMAGES)) {
          /* Don't bother to render. */
            return tile_buffer;
        } else {
          /* Give renderer right projection even if we ignore the result. */
            icetProjectTile(tile, projection_matrix);
        }
#if 1
    } else if (   (contained_viewport[0] >= tile_viewport[0])
               && (contained_viewport[1] >= tile_viewport[1])
               && (   contained_viewport[2]+contained_viewport[0]
                   <= tile_viewport[2]+tile_viewport[0])
               && (   contained_viewport[3]+contained_viewport[1]
                   <= tile_viewport[3]+tile_viewport[1]) ) {
      /* Case 1: geometry fits entirely within tile. */
        icetRaiseDebug("Case 1: geometry fits entirely within tile.");

        icetProjectTile(tile, projection_matrix);
        icetStateSetIntegerv(ICET_RENDERED_VIEWPORT, 4, tile_viewport);
        screen_viewport[0] = target_viewport[0]
            = contained_viewport[0] - tile_viewport[0];
        screen_viewport[1] = target_viewport[1]
            = contained_viewport[1] - tile_viewport[1];
        screen_viewport[2] = target_viewport[2] = contained_viewport[2];
        screen_viewport[3] = target_viewport[3] = contained_viewport[3];

        readback_viewport[0] = screen_viewport[0];
        readback_viewport[1] = screen_viewport[1];
        readback_viewport[2] = screen_viewport[2];
        readback_viewport[3] = screen_viewport[3];
#endif
    } else if (   !use_floating_viewport
               || (contained_viewport[2] > physical_width)
               || (contained_viewport[3] > physical_height) ) {
      /* Case 2: Can't use floating viewport due to use selection or image
         does not fit. */
        icetRaiseDebug("Case 2: Can't use floating viewport.");

        icetProjectTile(tile, projection_matrix);
        icetStateSetIntegerv(ICET_RENDERED_VIEWPORT, 4, tile_viewport);
        if (contained_viewport[0] <= tile_viewport[0]) {
            screen_viewport[0] = target_viewport[0] = 0;
            screen_viewport[2] = target_viewport[2]
                = MIN(tile_viewport[2],
                      contained_viewport[0] + contained_viewport[2]
                      - tile_viewport[0]);
        } else {
            screen_viewport[0] = target_viewport[0]
                = contained_viewport[0] - tile_viewport[0];
            screen_viewport[2] = target_viewport[2]
                = MIN(contained_viewport[2],
                      tile_viewport[0] + tile_viewport[2]
                      - contained_viewport[0]);
        }

        if (contained_viewport[1] <= tile_viewport[1]) {
            screen_viewport[1] = target_viewport[1] = 0;
            screen_viewport[3] = target_viewport[3]
                = MIN(tile_viewport[3],
                      contained_viewport[1] + contained_viewport[3]
                      - tile_viewport[1]);
        } else {
            screen_viewport[1] = target_viewport[1]
                = contained_viewport[1] - tile_viewport[1];
            screen_viewport[3] = target_viewport[3]
                = MIN(contained_viewport[3],
                      tile_viewport[1] + tile_viewport[3]
                      - contained_viewport[1]);
        }

        readback_viewport[0] = screen_viewport[0];
        readback_viewport[1] = screen_viewport[1];
        readback_viewport[2] = screen_viewport[2];
        readback_viewport[3] = screen_viewport[3];
    } else {
      /* Case 3: Using floating viewport. */
        IceTDouble viewport_project_matrix[16];
        IceTDouble global_projection_matrix[16];
        IceTInt rendered_viewport[4];
        icetRaiseDebug("Case 3: Using floating viewport.");

      /* This is the viewport in the global tiled display that we will be
         rendering. */
        rendered_viewport[0] = contained_viewport[0];
        rendered_viewport[1] = contained_viewport[1];
        rendered_viewport[2] = physical_width;
        rendered_viewport[3] = physical_height;

      /* This is the area that has valid pixels.  The screen_viewport will be a
         subset of this. */
        readback_viewport[0] = 0;
        readback_viewport[1] = 0;
        readback_viewport[2] = contained_viewport[2];
        readback_viewport[3] = contained_viewport[3];

        if (contained_viewport[0] < tile_viewport[0]) {
            screen_viewport[0] = tile_viewport[0] - contained_viewport[0];
            screen_viewport[2] = MIN(contained_viewport[2] - screen_viewport[0],
                                     tile_viewport[2]);
            target_viewport[0] = 0;
            target_viewport[2] = screen_viewport[2];
        } else {
            target_viewport[0] = contained_viewport[0] - tile_viewport[0];
            target_viewport[2] = MIN(tile_viewport[2] - target_viewport[0],
                                     contained_viewport[2]);
            screen_viewport[0] = 0;
            screen_viewport[2] = target_viewport[2];
        }
        if (contained_viewport[1] < tile_viewport[1]) {
            screen_viewport[1] = tile_viewport[1] - contained_viewport[1];
            screen_viewport[3] = MIN(contained_viewport[3] - screen_viewport[1],
                                     tile_viewport[3]);
            target_viewport[1] = 0;
            target_viewport[3] = screen_viewport[3];
        } else {
            target_viewport[1] = contained_viewport[1] - tile_viewport[1];
            target_viewport[3] = MIN(tile_viewport[3] - target_viewport[1],
                                     contained_viewport[3]);
            screen_viewport[1] = 0;
            screen_viewport[3] = target_viewport[3];
        }

      /* Floating viewport must be stored in our own buffer so subsequent tiles
         can be read from it. */
        render_buffer = getRenderBuffer();

      /* Check to see if we already rendered the floating viewport.  The whole
         point of the floating viewport is to do one actual render and reuse the
         image to grab all the actual tile images. */
        if (  icetStateGetTime(ICET_RENDERED_VIEWPORT)
            > icetStateGetTime(ICET_IS_DRAWING_FRAME) ) {
          /* Already rendered image for this tile. */
            const IceTInt *old_rendered_viewport
                = icetUnsafeStateGetInteger(ICET_RENDERED_VIEWPORT);
            IceTBoolean old_rendered_viewport_valid
                = (   (old_rendered_viewport[0] == rendered_viewport[0])
                   || (old_rendered_viewport[1] == rendered_viewport[1])
                   || (old_rendered_viewport[2] == rendered_viewport[2])
                   || (old_rendered_viewport[3] == rendered_viewport[3]) );
            if (!old_rendered_viewport_valid) {
                icetRaiseError(ICET_SANITY_CHECK_FAIL,
                               "Rendered floating viewport became invalidated");
            } else {
                icetRaiseDebug("Already rendered floating viewport.");
                return render_buffer;
            }
        }
        icetStateSetIntegerv(ICET_RENDERED_VIEWPORT, 4, rendered_viewport);

      /* Setup render for this tile. */

        icetGetViewportProject(rendered_viewport[0], rendered_viewport[1],
                               rendered_viewport[2], rendered_viewport[3],
                               viewport_project_matrix);
        icetGetDoublev(ICET_PROJECTION_MATRIX, global_projection_matrix);
        icetMatrixMultiply(projection_matrix,
                           viewport_project_matrix,
                           global_projection_matrix);
    }

  /* Make sure that the current render_buffer is sized appropriately for the
     physical viewport.  If not, use our own buffer. */
    if (    (icetImageGetWidth(render_buffer) != physical_width)
         || (icetImageGetHeight(render_buffer) != physical_height) ) {
        render_buffer = getRenderBuffer();
    }

  /* Now we can actually start to render an image. */
    icetGetDoublev(ICET_MODELVIEW_MATRIX, modelview_matrix);
    icetGetFloatv(ICET_BACKGROUND_COLOR, background_color);

  /* Draw the geometry. */
    icetGetPointerv(ICET_DRAW_FUNCTION, &value);
    drawfunc = (IceTDrawCallbackType)value;
    icetRaiseDebug("Calling draw function.");
    icetTimingRenderBegin();
    (*drawfunc)(projection_matrix, modelview_matrix, background_color,
                readback_viewport, render_buffer);
    icetTimingRenderEnd();

    return render_buffer;
}

static IceTImage prerenderedTile(int tile,
                                 IceTInt *screen_viewport,
                                 IceTInt *target_viewport)
{
    const IceTInt *contained_viewport;
    const IceTInt *tile_viewport;

    icetRaiseDebug("Getting viewport for tile %d in prerendered image", tile);
    contained_viewport = icetUnsafeStateGetInteger(ICET_CONTAINED_VIEWPORT);
    tile_viewport = icetUnsafeStateGetInteger(ICET_TILE_VIEWPORTS) + 4*tile;

    /* The screen viewport is the intersection of the tile viewport with the
     * contained viewport. */
    icetIntersectViewports(tile_viewport, contained_viewport, screen_viewport);

    /* The target viewport is the same width-height as the screen viewport.
     * It is offset by the same amount the screen viewport is offset from the
     * tile viewport. */
    target_viewport[0] = screen_viewport[0] - tile_viewport[0];
    target_viewport[1] = screen_viewport[1] - tile_viewport[1];
    target_viewport[2] = screen_viewport[2];
    target_viewport[3] = screen_viewport[3];

    return icetRetrieveStateImage(ICET_RENDER_BUFFER);
}

static IceTImage getRenderBuffer(void)
{
    if (*icetUnsafeStateGetBoolean(ICET_RENDER_LAYER_HOLDS_BUFFER)) {
        return icetImageNull();
    }

    /* Check to see if we are in the same frame as the last time we returned
       this buffer.  In that case, just restore the buffer because it still has
       the image we need. */
    if (  icetStateGetTime(ICET_RENDER_BUFFER)
        > icetStateGetTime(ICET_IS_DRAWING_FRAME) ) {
        return icetRetrieveStateImage(ICET_RENDER_BUFFER);
    } else {
        IceTInt dim[2];

        icetGetIntegerv(ICET_PHYSICAL_RENDER_WIDTH, &dim[0]);
        icetGetIntegerv(ICET_PHYSICAL_RENDER_HEIGHT, &dim[1]);

        /* Create a new image object. */
        return icetGetStateBufferImage(ICET_RENDER_BUFFER, dim[0], dim[1]);
    }
}
