#include "../../../HarkenStudio/ThirdParty/libvorbis/lib/codec_internal.h"

/* Keep the reference library's C-only internal declarations out of C++ tests. */
int harken_test_vorbis_floor_supported(const vorbis_info* info) {
    const codec_setup_info* setup = (const codec_setup_info*)info->codec_setup;
    int i;
    if (!setup || setup->floors <= 0) return 0;
    for (i = 0; i < setup->floors; ++i)
        if (setup->floor_type[i] != 1) return 0;
    return 1;
}
