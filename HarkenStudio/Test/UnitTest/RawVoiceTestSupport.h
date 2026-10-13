#ifndef HARKEN_TESTS_TESTSUPPORT_RAW_VOICE_TEST_SUPPORT_H
#define HARKEN_TESTS_TESTSUPPORT_RAW_VOICE_TEST_SUPPORT_H

#include "HarkenApiInternal.h"

namespace harkentest {

inline VnResult PlayRaw(const VnPlayDesc* desc,
                        VnVoiceHandle* outHandle) noexcept {
    return harken::detail::PlayVoiceRaw(desc, outHandle);
}

}

#endif
