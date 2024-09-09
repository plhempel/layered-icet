/* This file implements the functions icetSparseImageScanPixels and
 * icetSparseLayeredImageScanPixels and should only be included as their body.
 * The layered version is selected by defining the macro SIS_LAYERED, which is
 * undefined at the end of this file.
 */
{
    const IceTByte *in_data = *in_data_p; /* IceTByte for byte-pointer arithmetic. */
    IceTSizeType inactive_before = *inactive_before_p;
    IceTSizeType active_till_next_runl = *active_till_next_runl_p;

#ifdef SIS_LAYERED
    IceTSizeType active_frags_till_next_runl = *active_frags_till_next_runl_p;
#endif

    IceTSizeType pixels_left = pixels_to_skip;
    const IceTVoid *last_in_run_length = NULL;
    IceTByte *out_data;
    IceTVoid *last_out_run_length;

    if (pixels_left < 1) { return; }    /* Nothing to do. */

    /* Define local macros. */
#ifdef SIS_LAYERED
#define SIS_RUN_LENGTH_SIZE RUN_LENGTH_SIZE_LAYERED
#else
#define SIS_RUN_LENGTH_SIZE RUN_LENGTH_SIZE
#endif

#define ADVANCE_OUT_RUN_LENGTH_BASE()                   \
    {                                                   \
        last_out_run_length = out_data;                 \
        out_data += SIS_RUN_LENGTH_SIZE;                \
        INACTIVE_RUN_LENGTH(last_out_run_length) = 0;   \
        ACTIVE_RUN_LENGTH(last_out_run_length) = 0;     \
    }

#ifdef SIS_LAYERED
#define ADVANCE_OUT_RUN_LENGTH()                                \
        ADVANCE_OUT_RUN_LENGTH_BASE()                           \
        ACTIVE_RUN_LENGTH_FRAGMENTS(last_out_run_length) = 0;
#else
#define ADVANCE_OUT_RUN_LENGTH() ADVANCE_OUT_RUN_LENGTH_BASE()
#endif

    if (out_data_p != NULL) {
        out_data = *out_data_p;
        if (out_run_length_p != NULL) {
            last_out_run_length = *out_run_length_p;
        } else /* out_run_length_p == NULL */ {
            ADVANCE_OUT_RUN_LENGTH();
        }
    } else /* out_data_p == NULL */ {
        out_data = NULL;
        last_out_run_length = NULL;
    }

    while (pixels_left > 0) {
        IceTSizeType count, num_bytes;
#ifdef SIS_LAYERED
        IceTSizeType frag_count;
#endif

        if ((inactive_before == 0) && (active_till_next_runl == 0)) {
            last_in_run_length = in_data;
            inactive_before = INACTIVE_RUN_LENGTH(in_data);
            active_till_next_runl = ACTIVE_RUN_LENGTH(in_data);
#ifdef SIS_LAYERED
            active_frags_till_next_runl = ACTIVE_RUN_LENGTH_FRAGMENTS(in_data);
#endif
            in_data += SIS_RUN_LENGTH_SIZE;
        }

        count = MIN(inactive_before, pixels_left);
        if (count > 0) {
            if (out_data != NULL) {
                if (ACTIVE_RUN_LENGTH(last_out_run_length) > 0) {
                    ADVANCE_OUT_RUN_LENGTH();
                }
                INACTIVE_RUN_LENGTH(last_out_run_length) += count;
            }
            inactive_before -= count;
            pixels_left -= count;
        }

#ifdef SIS_LAYERED
        if (active_till_next_runl <= pixels_left) {
            count = active_till_next_runl;
            frag_count = active_frags_till_next_runl;
        } else {
            /* With an incomplete run, the only way to determine the number of
             * fragments is to iterate over each pixel. */
            const IceTVoid *data = in_data;
            icetSparseLayeredImageScanFragments(&data,
                                                pixels_left,
                                                fragment_size,
                                                &frag_count);
            count = pixels_left;
        }

        num_bytes = count*sizeof(IceTLayerCount) + frag_count*fragment_size;
#else
        count = MIN(active_till_next_runl, pixels_left);
        num_bytes = count*pixel_size;
#endif
        if (count > 0) {
            if (out_data != NULL) {
                ACTIVE_RUN_LENGTH(last_out_run_length) += count;
#ifdef SIS_LAYERED
                ACTIVE_RUN_LENGTH_FRAGMENTS(last_out_run_length) += frag_count;
#endif
                memcpy(out_data, in_data, num_bytes);
                out_data += num_bytes;
            }
            in_data += num_bytes;
            active_till_next_runl -= count;
#ifdef SIS_LAYERED
            active_frags_till_next_runl -= frag_count;
#endif
            pixels_left -= count;
        }
    }
    if (pixels_left < 0) {
        icetRaiseError(ICET_SANITY_CHECK_FAIL, "Miscounted pixels");
    }

    *in_data_p = in_data;
    *inactive_before_p = inactive_before;
    *active_till_next_runl_p = active_till_next_runl;
#ifdef SIS_LAYERED
    *active_frags_till_next_runl_p = active_frags_till_next_runl;
#endif
    if (last_in_run_length_p) {
        *last_in_run_length_p = (IceTVoid *)last_in_run_length;
    }
    if (out_data_p) {
        *out_data_p = out_data;
    }
    if (out_run_length_p) {
        *out_run_length_p = last_out_run_length;
    }

/* Undefine local macros. */
#undef ADVANCE_OUT_RUN_LENGTH
#undef SIS_LAYERED
#undef SIS_RUN_LENGTH_SIZE
}
