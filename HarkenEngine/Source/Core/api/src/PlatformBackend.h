// Compile-time selection of the real-device audio backend(s) per platform.
//
// This header carries no miniaudio dependency on purpose: the selection is a
// plain enum list so unit tests can assert the platform choice (and the "an OUT
// platform enables no real backend" invariant) without parsing the vendored
// single header. The device backend translation unit maps these values onto the
// concrete ma_backend constants.
//
// CI and all automated tests run on the engine's own null backend and never
// touch this list; it describes the real-hardware path only (base spec §16 /
// M5 spec §7.2). Backend selection is compile-time per platform (M5 spec §7.1):
// Windows keeps WASAPI shared-mode; macOS and iOS select Core Audio; Android
// prefers AAudio and falls back to OpenSL ES on API levels below AAudio. Linux
// and other desktops are OUT of M5 (spec §1/§7) and enable NO real backend — the
// list is empty there, so a real-device request fails at Vn_Init rather than
// opening anything (the engine's own null backend is a separate device mode,
// never a silent real-device fallback).
#ifndef HARKEN_API_PLATFORM_BACKEND_H
#define HARKEN_API_PLATFORM_BACKEND_H

#include <cstddef>
#include <cstdint>

namespace harken {

// A platform-neutral name for one miniaudio backend the build may open. Mapped
// to the concrete ma_backend value inside the device backend TU. `Null` names
// miniaudio's null backend; it is intentionally NOT a member of any platform's
// list (see below) — the engine's own null backend is reached through the null
// device mode, not through this real-device selection.
enum class PlatformAudioBackend : std::uint8_t {
    Null = 0,
    Wasapi,
    CoreAudio,
    AAudio,
    OpenSL,
};

// The backend list is a fixed-capacity array plus an explicit count, so the OUT
// platform is a real count of 0 without an (ISO-illegal) zero-length array. Only
// the first kPlatformBackendCount entries are meaningful; any tail is unused
// storage, never opened. Capacity is the largest list any platform needs
// (Android's AAudio + OpenSL ES).
inline constexpr std::size_t kMaxPlatformBackends = 2;

// The ordered list of real-device backends this build enables. The first entry
// is the preferred backend; later entries are fallbacks tried in order (Android
// lists AAudio then OpenSL ES, so miniaudio uses AAudio where the device runtime
// provides it and falls back to OpenSL ES otherwise). An OUT platform lists none
// (count 0): an attempt to open a device surfaces as VN_DEVICE_ERROR at Vn_Init.
#if defined(_WIN32)
inline constexpr PlatformAudioBackend kPlatformBackends[kMaxPlatformBackends] = {
    PlatformAudioBackend::Wasapi,
};
inline constexpr std::size_t kPlatformBackendCount = 1;
#elif defined(__APPLE__)
inline constexpr PlatformAudioBackend kPlatformBackends[kMaxPlatformBackends] = {
    PlatformAudioBackend::CoreAudio,
};
inline constexpr std::size_t kPlatformBackendCount = 1;
#elif defined(__ANDROID__)
inline constexpr PlatformAudioBackend kPlatformBackends[kMaxPlatformBackends] = {
    PlatformAudioBackend::AAudio,
    PlatformAudioBackend::OpenSL,
};
inline constexpr std::size_t kPlatformBackendCount = 2;
#else
inline constexpr PlatformAudioBackend kPlatformBackends[kMaxPlatformBackends] = {};
inline constexpr std::size_t kPlatformBackendCount = 0;
#endif

// A shipping platform always names a concrete backend first; an OUT platform
// lists none. These compile-time asserts fail the build if the per-platform
// selection is ever broken or a branch is miscompiled onto the wrong platform
// (the machine gate for "platform-branch backend selection stays correct and is
// not miscompiled into another platform").
#if defined(_WIN32)
static_assert(kPlatformBackendCount == 1 &&
                  kPlatformBackends[0] == PlatformAudioBackend::Wasapi,
              "Windows must select WASAPI shared-mode as its only backend");
#elif defined(__APPLE__)
static_assert(kPlatformBackendCount == 1 &&
                  kPlatformBackends[0] == PlatformAudioBackend::CoreAudio,
              "macOS/iOS must select Core Audio as their only backend");
#elif defined(__ANDROID__)
static_assert(kPlatformBackendCount == 2 &&
                  kPlatformBackends[0] == PlatformAudioBackend::AAudio &&
                  kPlatformBackends[1] == PlatformAudioBackend::OpenSL,
              "Android must prefer AAudio and fall back to OpenSL ES");
#else
static_assert(kPlatformBackendCount == 0,
              "Linux and other platforms are OUT of M5 and enable no real "
              "device backend");
#endif

} // namespace harken

#endif // HARKEN_API_PLATFORM_BACKEND_H
