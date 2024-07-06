#include <IceT.h>

#include <IceTDevCommunication.h>
#include <IceTDevDiagnostics.h>
#include <IceTDevImage.h>


void icetGpuBswapCompose(const IceTInt *compose_group,
                         IceTInt group_size,
                         IceTInt image_dest,
                         IceTSparseImage input_image,
                         IceTSparseImage *result_image,
                         IceTSizeType *piece_offset)
{
    icetRaiseDebug("In GPU binary-swap compose");

    /* Silence "unused" warnings. */
    (void)compose_group;
    (void)group_size;
    (void)image_dest;
    (void)input_image;
    (void)piece_offset;

    /* Just return an empty image for now. */
    *result_image = icetSparseImageNull();
}
