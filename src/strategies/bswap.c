/* -*- c -*- *******************************************************/
/*
 * Copyright (C) 2010 Sandia Corporation
 * Under the terms of Contract DE-AC04-94AL85000 with Sandia Corporation,
 * the U.S. Government retains certain rights in this software.
 *
 * This source code is released under the New BSD License.
 */

#include <IceT.h>

#include <IceTDevCommunication.h>
#include <IceTDevDiagnostics.h>
#include <IceTDevImage.h>

#include <string.h>

#define BSWAP_INCOMING_IMAGES_BUFFER    ICET_SI_STRATEGY_BUFFER_0
#define BSWAP_WORKING_IMAGE_BUFFER_1    ICET_SI_STRATEGY_BUFFER_1
#define BSWAP_WORKING_IMAGE_BUFFER_2    ICET_SI_STRATEGY_BUFFER_2
#define BSWAP_IMAGE_ARRAY               ICET_SI_STRATEGY_BUFFER_3
#define BSWAP_DUMMY_ARRAY               ICET_SI_STRATEGY_BUFFER_4
#define BSWAP_COMPOSE_GROUP_BUFFER      ICET_SI_STRATEGY_BUFFER_5

#define BSWAP_SWAP_IMAGES 21
#define BSWAP_TELESCOPE 22
#define BSWAP_FOLD 23

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

/* Finds the largest power of 2 equal to or smaller than x. */
static IceTInt bswapFindPower2(IceTInt x)
{
    IceTInt pow2;
    for (pow2 = 1; pow2 <= x; pow2 = pow2 << 1);
    pow2 = pow2 >> 1;
    return pow2;
}

#if 0

This function is no longer needed because image splits are handled internally
in image functions.

/* Adjusts the number of pixels for a region of a given offset and size within
 * an image of a given size. */
static void bswapAdjustRegionPixels(IceTSizeType num_total_pixels,
                                    IceTSizeType offset,
                                    IceTSizeType *num_pixels_p)
{
    if ((offset + *num_pixels_p) > num_total_pixels) {
        if (offset >= num_total_pixels) {
            *num_pixels_p = 0;
        } else {
            *num_pixels_p = num_total_pixels - offset;
        }
    }
}

These collection functions are no longer used, but I am keeping them in
the source code because they contain a good example of determining the
size/offset of each partition without communication.

static void bswapCollectFinalImages(const IceTInt *compose_group,
                                    IceTInt group_size,
                                    IceTInt group_rank,
                                    IceTImage image,
                                    IceTSizeType pixel_count)
{
    IceTEnum color_format, depth_format;
    IceTSizeType num_pixels;
    IceTCommRequest *requests;
    int i;

  /* Adjust image for output as some buffers, such as depth, might be
     dropped. */
    icetImageAdjustForOutput(image);

  /* All processors have the same number for pixels and their offset
   * is group_rank*offset. */
    color_format = icetImageGetColorFormat(image);
    depth_format = icetImageGetDepthFormat(image);
    num_pixels = icetImageGetNumPixels(image);
    requests = malloc((group_size)*sizeof(IceTCommRequest));

    if (color_format != ICET_IMAGE_COLOR_NONE) {
      /* Use IceTByte for byte-based pointer arithmetic. */
        IceTByte *colorBuffer;
        IceTSizeType pixel_size;
        colorBuffer = icetImageGetColorVoid(image, &pixel_size);
        icetRaiseDebug("Collecting image data.");
        for (i = 0; i < group_size; i++) {
            IceTInt src;
            IceTSizeType offset;
            IceTBoolean receive_from_src;

            /* Actual piece is located at the bit reversal of i. */
            BIT_REVERSE(src, i, group_size);

            offset = pixel_count*i;

            receive_from_src = ICET_TRUE;
            if (src == group_rank) receive_from_src = ICET_FALSE;
            if (offset >= num_pixels) receive_from_src = ICET_FALSE;

            if (receive_from_src) {
                requests[i] =
                    icetCommIrecv(colorBuffer + pixel_size*offset,
                                  pixel_size*pixel_count, ICET_BYTE,
                                  compose_group[src], SWAP_IMAGE_DATA);
            } else {
                requests[i] = ICET_COMM_REQUEST_NULL;
            }
        }
        icetCommWaitall(group_size, requests);
    }

    if (depth_format != ICET_IMAGE_DEPTH_NONE) {
      /* Use IceTByte for byte-based pointer arithmetic. */
        IceTByte *depthBuffer;
        IceTSizeType pixel_size;
        depthBuffer = icetImageGetDepthVoid(image, &pixel_size);
        icetRaiseDebug("Collecting depth data.");
        for (i = 0; i < group_size; i++) {
            IceTInt src;
            IceTSizeType offset;
            IceTBoolean receive_from_src;

            /* Actual peice is located at the bit reversal of i. */
            BIT_REVERSE(src, i, group_size);

            offset = pixel_count*i;

            receive_from_src = ICET_TRUE;
            if (src == group_rank) receive_from_src = ICET_FALSE;
            if (offset >= num_pixels) receive_from_src = ICET_FALSE;

            if (receive_from_src) {
                requests[i] =
                    icetCommIrecv(depthBuffer + pixel_size*offset,
                                  pixel_size*pixel_count, ICET_BYTE,
                                  compose_group[src], SWAP_DEPTH_DATA);
            } else {
                requests[i] = ICET_COMM_REQUEST_NULL;
            }
        }
        icetCommWaitall(group_size, requests);
    }
    free(requests);
}

static void bswapSendFinalImage(const IceTInt *compose_group,
                                IceTInt image_dest,
                                IceTImage image,
                                IceTSizeType pixel_count,
                                IceTSizeType offset)
{
    IceTEnum color_format, depth_format;
    IceTSizeType num_pixels;

  /* Adjust image for output as some buffers, such as depth, might be
     dropped. */
    icetImageAdjustForOutput(image);

    color_format = icetImageGetColorFormat(image);
    depth_format = icetImageGetDepthFormat(image);
    num_pixels = icetImageGetNumPixels(image);

    if (offset >= num_pixels) {
        icetRaiseDebug("No pixels to send to bswap collection.");
        return;
    }

  /* Correct for last piece that may overrun image size. */
    bswapAdjustRegionPixels(num_pixels, offset, &pixel_count);

    if (color_format != ICET_IMAGE_COLOR_NONE) {
      /* Use IceTByte for byte-based pointer arithmetic. */
        IceTByte *colorBuffer;
        IceTSizeType pixel_size;
        colorBuffer = icetImageGetColorVoid(image, &pixel_size);
        icetRaiseDebug("Sending image data.");
        icetCommSend(colorBuffer + pixel_size*offset,
                     pixel_size*pixel_count, ICET_BYTE,
                     compose_group[image_dest], SWAP_IMAGE_DATA);
    }

    if (depth_format != ICET_IMAGE_DEPTH_NONE) {
      /* Use IceTByte for byte-based pointer arithmetic. */
        IceTByte *depthBuffer;
        IceTSizeType pixel_size;
        depthBuffer = icetImageGetDepthVoid(image, &pixel_size);
        icetRaiseDebug("Sending depth data.");
        icetCommSend(depthBuffer + pixel_size*offset,
                     pixel_size*pixel_count, ICET_BYTE,
                     compose_group[image_dest], SWAP_DEPTH_DATA);
    }
}
#endif

/* Completes the end part of the telescoping process where this process, located
 * in the upper group, splits its image and sends the partitions to processes in
 * the lower group.  Both the upper and lower group are assumed to be of sizes
 * power of 2.  Also, the upper group is smaller than the lower group.
 * Preconditions:
 *      scratch_buffer is unused.
 * Postconditions:
 *      scratch_buffer is unused. */
static void bswapSendFromUpperGroup(const IceTInt *lower_group,
                                    IceTInt lower_group_size,
                                    const IceTInt *upper_group,
                                    IceTInt upper_group_size,
                                    IceTInt largest_group_size,
                                    IceTSparseImage working_image,
                                    IceTEnum scratch_buffer)
{
    IceTInt num_pieces = lower_group_size/upper_group_size;
    IceTInt eventual_num_pieces = largest_group_size/upper_group_size;
    IceTSparseImage *image_partitions;
    IceTSizeType *dummy_array;
    IceTInt piece;
    IceTInt upper_group_rank;

    upper_group_rank = icetFindMyRankInGroup(upper_group, upper_group_size);

    /* Partition the image into pieces to send to each proess in the lower
       group. */
    {
        dummy_array = icetGetStateBuffer(BSWAP_DUMMY_ARRAY,
                                         num_pieces * sizeof(IceTSizeType));
        image_partitions
            = icetGetStateBuffer(BSWAP_IMAGE_ARRAY,
                                 num_pieces * sizeof(IceTSparseImage));
        image_partitions[0] = working_image;
        for (piece = 1; piece < num_pieces; piece++) {
            image_partitions[piece] = icetSparseImageNull();
        }
        icetSparseImageSplitAlloc(working_image,
                                  0,
                                  num_pieces,
                                  eventual_num_pieces,
                                  scratch_buffer,
                                  image_partitions,
                                  dummy_array);
    }

    /* Trying to figure out what processes to send to is tricky.  We
     * can do this by getting the piece number (bit reversal of
     * upper_group_rank), multiply this by num_pieces, add the number
     * of each local piece to get the piece number for the lower
     * half, and finally reverse the bits again.  Equivocally, we can
     * just reverse the bits of the local piece num, multiply by
     * num_pieces and add that to upper_group_rank to get the final
     * location. */
    for (piece = 0; piece < num_pieces; piece++) {
        IceTVoid *package_buffer;
        IceTSizeType package_size;
        IceTInt dest_rank;

        BIT_REVERSE(dest_rank, piece, num_pieces);
        dest_rank = dest_rank*upper_group_size + upper_group_rank;
        icetRaiseDebug("Sending piece %d to %d", piece, dest_rank);

        icetSparseImagePackageForSend(image_partitions[piece],
                                      &package_buffer, &package_size);
        /* Send to processor in lower "half" that has same part of image. */
        icetCommSend(package_buffer,
                     package_size,
                     ICET_BYTE,
                     lower_group[dest_rank],
                     BSWAP_TELESCOPE);
    }
}

/* Completes the end part of the telescoping process where this process, located
 * in the lower group, receives an image from the upper group and composites it
 * to its own.  Both the upper and lower group are assumed to be of sizes power
 * of 2.  Also, the upper group is smaller than the lower group.
 * Preconditions:
 *      *working_image_p is the local image.
 *      *working_buffer_p holds *working_image_p or is unused.
 *      *spare_buffer_p is unused.
 * Postconditions:
 *      *working_image_p is the result of compositing the local and received
            image.
 *      *working_buffer_p holds *working_image_p or is unused.
 *      *spare_buffer_p is unused. */
static void bswapReceiveFromUpperGroup(const IceTInt *lower_group,
                                       IceTInt lower_group_size,
                                       const IceTInt *upper_group,
                                       IceTInt upper_group_size,
                                       IceTSparseImage *working_image_p,
                                       IceTEnum *working_buffer_p,
                                       IceTEnum *spare_buffer_p)
{
    /* To get the processor where the extra stuff is located, I could
     * reverse the bits of the local process, divide by the appropriate
     * amount, and reverse the bits again.  However, the equivalent to
     * this is just clearing out the upper bits. */
    if (upper_group_size > 0) {
        IceTInt lower_group_rank;
        IceTInt src;
        IceTVoid *in_image_buffer;
        IceTSparseImage in_image;

        lower_group_rank = icetFindMyRankInGroup(lower_group, lower_group_size);

        src = lower_group_rank & (upper_group_size-1);
        icetRaiseDebug("Absorbing image from %d", (int)src);

        in_image_buffer = icetCommRecvAlloc(BSWAP_INCOMING_IMAGES_BUFFER,
                                            ICET_BYTE,
                                            upper_group[src],
                                            BSWAP_TELESCOPE);
        in_image = icetSparseImageUnpackageFromReceive(in_image_buffer);

        {
            IceTEnum old_working_buffer = *working_buffer_p;
            IceTEnum result_buffer = *spare_buffer_p;

            *working_image_p = icetCompressedCompressedCompositeAlloc(
                                                               *working_image_p,
                                                               in_image,
                                                               result_buffer);
            *working_buffer_p = result_buffer;
            *spare_buffer_p = old_working_buffer;
        }
    }
}

/* Does a binary swap on a group that is of size power of 2.  This is not
 * checked but must be true or else the operation will fail (probably in
 * deadlock).
 * Preconditions:
 *      *working_image_p is the local image.
 *      *working_buffer_p holds *working_image_p or is unused.
 *      *spare_buffer_p is unused.
 * Postconditions:
 *      *working_image_p is the compositing result for this group.
 *      *working_buffer_p holds *working_image_p or is unused.
 *      *spare_buffer_p is unused. */
static void bswapComposePow2(const IceTInt *compose_group,
                             IceTInt group_size,
                             IceTInt largest_group_size,
                             IceTSparseImage *working_image_p,
                             IceTEnum *working_buffer_p,
                             IceTEnum *spare_buffer_p,
                             IceTSizeType *piece_offset)
{
    IceTInt bitmask;
    IceTInt group_rank;
    IceTSparseImage image_data = *working_image_p;
    IceTEnum working_buffer = *working_buffer_p;
    IceTEnum spare_buffer = *spare_buffer_p;

    *piece_offset = 0;

    if (group_size < 2) {
        return;
    }

    group_rank = icetFindMyRankInGroup(compose_group, group_size);

    /* To do the ordering correct, at iteration i we must swap with a
     * process 2^i units away.  The easiest way to find the process to
     * pair with is to simply xor the group_rank with a value with the
     * ith bit set. */

    for (bitmask = 0x0001; bitmask < group_size; bitmask <<= 1) {
        IceTSparseImage outgoing_images[2];
        IceTInt outgoing_offsets[2];

        IceTInt pair;
        IceTInt inOnTop;
        IceTSparseImage send_image;
        IceTSparseImage keep_image;
        IceTEnum send_buffer;
        IceTEnum keep_buffer;

        /* Allocate outgoing buffers and split working image. */
        {
            outgoing_images[0] = image_data;
            outgoing_images[1] = icetSparseImageNull();

            icetSparseImageSplitAlloc(image_data,
                                      *piece_offset,
                                      2,
                                      largest_group_size/bitmask,
                                      spare_buffer,
                                      outgoing_images,
                                      outgoing_offsets);
        }

        /* Find pair process and decide which half of the image to send. */
        {
            pair = group_rank ^ bitmask;

            if (group_rank < pair) {
                send_image = outgoing_images[1];
                keep_image = outgoing_images[0];
                send_buffer = spare_buffer;
                keep_buffer = working_buffer;
                *piece_offset = outgoing_offsets[0];
                inOnTop = 0;
            } else {
                send_image = outgoing_images[0];
                keep_image = outgoing_images[1];
                send_buffer = working_buffer;
                keep_buffer = spare_buffer;
                *piece_offset = outgoing_offsets[1];
                inOnTop = 1;
            }
        }

        /* Swap image with partner and composite incoming. */
        {
            IceTVoid *package_buffer;
            IceTSizeType package_size;
            IceTVoid *in_image_buffer;
            IceTSparseImage in_image;

            icetSparseImagePackageForSend(send_image,
                                          &package_buffer,
                                          &package_size);

            in_image_buffer = icetCommSendrecvAlloc(
                                                   package_buffer,
                                                   package_size,
                                                   ICET_BYTE,
                                                   compose_group[pair],
                                                   BSWAP_SWAP_IMAGES,
                                                   BSWAP_INCOMING_IMAGES_BUFFER,
                                                   ICET_BYTE,
                                                   compose_group[pair],
                                                   BSWAP_SWAP_IMAGES);

            in_image
                = icetSparseImageUnpackageFromReceive(in_image_buffer);

            if (inOnTop) {
                image_data = icetCompressedCompressedCompositeAlloc(
                                                                   in_image,
                                                                   keep_image,
                                                                   send_buffer);
            } else {
                image_data = icetCompressedCompressedCompositeAlloc(
                                                                   keep_image,
                                                                   in_image,
                                                                   send_buffer);
            }

            working_buffer = send_buffer;
            spare_buffer   = keep_buffer;
        }
    }

    *working_image_p = image_data;
    *working_buffer_p = working_buffer;
    *spare_buffer_p = spare_buffer;
}

/* Does binary swap, but does not combine the images in the end.  Instead,
 * working_image is broken into pieces and each process returns a sub piece at
 * the given offset.  The pieces are mutually exclusive.  If pow2size is the
 * largest power of 2 less than or equal to group_size, then the first pow2size
 * processes in the group will have image pieces.  The rest will return empty
 * pieces.  Of the first pow2size processes in the group, each process contains
 * the ith piece, where i is group_rank with the bits reversed (which is
 * necessary to get the ordering correct).  If both color and depth buffers are
 * inputs, both are located in the uncollected images regardless of what buffers
 * are selected for outputs.
 * Preconditions:
 *      *working_image_p is the local image.
 *      *working_buffer_p holds *working_image_p or is unused.
 *      *spare_buffer_p is unused.
 * Postconditions:
 *      *working_image_p is the compositing result for this group.
 *      *working_buffer_p holds *working_image_p or is unused.
 *      *spare_buffer_p is unused. */
static void bswapComposeNoCombine(const IceTInt *compose_group,
                                  IceTInt group_size,
                                  IceTInt largest_group_size,
                                  IceTSparseImage *working_image_p,
                                  IceTEnum *working_buffer_p,
                                  IceTEnum *spare_buffer_p,
                                  IceTSizeType *piece_offset)
{
    IceTInt group_rank = icetFindMyRankInGroup(compose_group, group_size);
    IceTInt pow2size = bswapFindPower2(group_size);
    IceTInt extra_proc = group_size - pow2size;
    IceTInt extra_pow2size = bswapFindPower2(extra_proc);
    IceTSparseImage working_image = *working_image_p;
    IceTEnum working_buffer = *working_buffer_p;
    IceTEnum spare_buffer = *spare_buffer_p;

    /* Fix largest group size if necessary. */
    if (largest_group_size == -1) {
        largest_group_size = pow2size;
    }

    if (group_rank >= pow2size) {
        IceTInt upper_group_rank = group_rank - pow2size;
        /* I am part of the extra stuff.  Recurse to run bswap on my part. */
        bswapComposeNoCombine(compose_group + pow2size,
                              extra_proc,
                              largest_group_size,
                              &working_image,
                              &working_buffer,
                              &spare_buffer,
                              piece_offset);
        /* Now I may have some image data to send to lower group. */
        if (upper_group_rank < extra_pow2size) {
            bswapSendFromUpperGroup(compose_group,
                                    pow2size,
                                    compose_group + pow2size,
                                    extra_pow2size,
                                    largest_group_size,
                                    working_image,
                                    spare_buffer);
        }
        /* Report I have no image. */
        icetSparseImageSetDimensions(working_image, 0, 0);
        *piece_offset = 0;
    } else {
        IceTBoolean use_interlace;
        IceTSizeType total_num_pixels
            = icetSparseImageGetNumPixels(working_image);

        use_interlace
            = (largest_group_size > 2) && icetIsEnabled(ICET_INTERLACE_IMAGES);
        if (use_interlace) {
            IceTEnum old_working_buffer = working_buffer;
            working_image = icetSparseImageInterlaceAlloc(working_image,
                                                          largest_group_size,
                                                          BSWAP_DUMMY_ARRAY,
                                                          spare_buffer);

            working_buffer = spare_buffer;
            spare_buffer = old_working_buffer;
        }


        /* I am part of the lower group.  Do the actual binary swap. */
        bswapComposePow2(compose_group,
                         pow2size,
                         largest_group_size,
                         &working_image,
                         &working_buffer,
                         &spare_buffer,
                         piece_offset);

      /* Now absorb any image that was part of extra stuff. */
        bswapReceiveFromUpperGroup(compose_group,
                                   pow2size,
                                   compose_group + pow2size,
                                   extra_pow2size,
                                   &working_image,
                                   &working_buffer,
                                   &spare_buffer);

        if (use_interlace) {
           /* piece_offset happens to be ignored if this group is not the
              highest group. */
            IceTInt global_partition;
            BIT_REVERSE(global_partition, group_rank, largest_group_size);
            *piece_offset = icetGetInterlaceOffset(global_partition,
                                                   largest_group_size,
                                                   total_num_pixels);
        }
    }

    *working_image_p = working_image;
    *working_buffer_p = working_buffer;
    *spare_buffer_p = spare_buffer;
}

void icetBswapCompose(const IceTInt *compose_group,
                      IceTInt group_size,
                      IceTInt image_dest,
                      IceTSparseImage input_image,
                      IceTSparseImage *result_image,
                      IceTSizeType *piece_offset)
{
    icetRaiseDebug("In binary-swap compose");

    /* Remove warning about unused parameter.  Binary swap leaves images evenly
     * partitioned, so we have no use of the image_dest parameter. */
    (void)image_dest;

    /* Do actual bswap. */
    {
        IceTSparseImage working_image = input_image;
        IceTEnum working_buffer = BSWAP_WORKING_IMAGE_BUFFER_1;
        IceTEnum spare_buffer = BSWAP_WORKING_IMAGE_BUFFER_2;

        bswapComposeNoCombine(compose_group,
                              group_size,
                              -1,
                              &working_image,
                              &working_buffer,
                              &spare_buffer,
                              piece_offset);

        *result_image = working_image;
    }
}


void icetBswapFoldingCompose(const IceTInt *compose_group,
                             IceTInt group_size,
                             IceTInt image_dest,
                             IceTSparseImage input_image,
                             IceTSparseImage *result_image,
                             IceTSizeType *piece_offset)
{
    IceTInt group_rank = icetFindMyRankInGroup(compose_group, group_size);
    IceTInt pow2size = bswapFindPower2(group_size);
    IceTInt extra_proc = group_size - pow2size;
    IceTBoolean use_interlace;
    IceTSparseImage working_image;
    IceTEnum working_buffer = BSWAP_WORKING_IMAGE_BUFFER_1;
    IceTEnum spare_buffer = BSWAP_WORKING_IMAGE_BUFFER_2;
    IceTSizeType total_num_pixels = icetSparseImageGetNumPixels(input_image);
    IceTInt *pow2group;

    icetRaiseDebug("In binary-swap folding compose");

    (void)image_dest;  /* not used */

    if (group_size < 2) {
        *result_image = input_image;
        *piece_offset = 0;
        return;
    }

    /* Interlace images when requested. */
    use_interlace = (pow2size > 2) && icetIsEnabled(ICET_INTERLACE_IMAGES);
    if (use_interlace) {
        working_image = icetSparseImageInterlaceAlloc(input_image,
                                                      pow2size,
                                                      BSWAP_DUMMY_ARRAY,
                                                      working_buffer);
    } else {
        working_image = input_image;
    }

    /* Fold the existing number of processes into a subset that is the maximum
     * power of 2. */
    pow2group = icetGetStateBuffer(BSWAP_COMPOSE_GROUP_BUFFER,
                                   sizeof(IceTInt)*pow2size);
    {
        IceTInt whole_group_index = 0;
        IceTInt pow2group_index = 0;
        while (pow2group_index < extra_proc) {
            pow2group[pow2group_index] = compose_group[whole_group_index];

            if (group_rank == whole_group_index) {
                /* I need to receive a folded image and composite it. */
                IceTEnum old_working_buffer = working_buffer;
                IceTSparseImage in_image;

                IceTVoid *in_data = icetCommRecvAlloc(
                                             BSWAP_INCOMING_IMAGES_BUFFER,
                                             ICET_BYTE,
                                             compose_group[whole_group_index+1],
                                             BSWAP_FOLD);
                in_image = icetSparseImageUnpackageFromReceive(in_data);

                working_image = icetCompressedCompressedCompositeAlloc(
                                                                  working_image,
                                                                  in_image,
                                                                  spare_buffer);
                working_buffer = spare_buffer;
                spare_buffer = old_working_buffer;
            } else if (group_rank == whole_group_index + 1) {
                /* I need to send my image to get folded then drop out. */
                IceTVoid *package_buffer;
                IceTSizeType package_size;

                icetSparseImagePackageForSend(working_image,
                                              &package_buffer, &package_size);

                icetCommSend(package_buffer,
                             package_size,
                             ICET_BYTE,
                             compose_group[whole_group_index],
                             BSWAP_FOLD);

                *result_image = icetSparseImageNull();
                *piece_offset = 0;
                return;
            }

            whole_group_index += 2;
            pow2group_index++;
        }

        /* That handles all the folded images. The rest of the group can just
         * copy over. Do a sanity check too to make sure that we haven't messed
         * up our indexing. */
        if ((group_size - whole_group_index) != (pow2size - pow2group_index)) {
            icetRaiseError(ICET_SANITY_CHECK_FAIL,
                           "Miscounted indices while folding.");
        }
        memcpy(&pow2group[pow2group_index],
               &compose_group[whole_group_index],
               sizeof(IceTInt)*(group_size-whole_group_index));
    }

    /* Time to do the actual binary-swap on our new power of two group. */
    bswapComposePow2(pow2group,
                     pow2size,
                     pow2size,
                     &working_image,
                     &working_buffer,
                     &spare_buffer,
                     piece_offset);
    *result_image = working_image;

    if (use_interlace) {
        IceTInt global_partition;
        IceTInt pow2rank = icetFindMyRankInGroup(pow2group, pow2size);
        BIT_REVERSE(global_partition, pow2rank, pow2size);
        *piece_offset = icetGetInterlaceOffset(global_partition,
                                               pow2size,
                                               total_num_pixels);
    }
}
