// The single miniaudio implementation translation unit. Exactly one TU
// in the whole build defines MINIAUDIO_IMPLEMENTATION; it lives inside
// the engine library so the device backend and the host tool (which
// links the library and uses ma_decoder for wav loading) share one set
// of symbols — a second definition anywhere is an ODR violation.
//
// No compile-time feature trimming: miniaudio's configuration macros
// alter struct contents inside the vendored header, so they would have
// to be defined identically for every TU that parses it. Restricting to
// WASAPI happens at runtime instead, where it cannot cause layout
// divergence (see the device backend's context initialization).
#define MA_COINIT_VALUE 2 /* COINIT_APARTMENTTHREADED: compatible with Qt GUI threads. */
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

extern "C" ma_uint32
VnMiniaudioWasapiPublishedAvrtTaskIndex(ma_device* device) noexcept {
#if defined(_WIN32)
    if (device == nullptr) {
        return 0;
    }
    return ma_atomic_uint32_get(
        &device->wasapi.publishedAvrtTaskIndex);
#else
    (void)device;
    return 0;
#endif
}

extern "C" ma_bool32
VnMiniaudioAtomicBool32Get(ma_atomic_bool32* value) noexcept {
    return value != nullptr ? ma_atomic_bool32_get(value) : MA_FALSE;
}
