// Shared wav decoding for the host tools (demo driver and pack compiler):
// decodes a wav file to interleaved float32 at its native channel count
// and sample rate through miniaudio. Host-side only; never part of the
// engine. The miniaudio implementation TU lives in the engine library the
// tools link against — this header uses declarations only.
#ifndef HARKEN_HOST_WAV_LOADER_H
#define HARKEN_HOST_WAV_LOADER_H

#include "miniaudio.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace harkenhost {

struct LoadedWav {
    std::vector<float> samples; // interleaved f32, native rate
    std::uint32_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint64_t frames = 0;
};

// Decodes a wav to f32 at its native channel count and sample rate.
// The decoder is pinned to miniaudio's wav backend, so non-wav input is
// rejected at open. The normal host path accepts only mono and stereo;
// PackC opts into wider layouts only for entries whose policy needs to
// issue an explicit unsupported-downmix refusal.
// Errors print to stderr with the offending path.
inline bool LoadWav(const std::string& path, LoadedWav* out,
                    bool allowMultichannel = false) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    config.encodingFormat = ma_encoding_format_wav;
    ma_decoder decoder;
    if (ma_decoder_init_file(path.c_str(), &config, &decoder) !=
        MA_SUCCESS) {
        std::fprintf(stderr, "error: cannot open or decode '%s'\n",
                     path.c_str());
        return false;
    }
    out->channels = decoder.outputChannels;
    out->sampleRate = decoder.outputSampleRate;
    if (!allowMultichannel && out->channels != 1 &&
        out->channels != 2) {
        std::fprintf(stderr,
                     "error: '%s' has %u channels; only mono and stereo "
                     "are supported\n",
                     path.c_str(), out->channels);
        ma_decoder_uninit(&decoder);
        return false;
    }
    const ma_uint64 kChunkFrames = 4096;
    std::vector<float> chunk(kChunkFrames * out->channels);
    for (;;) {
        ma_uint64 read = 0;
        const ma_result result = ma_decoder_read_pcm_frames(
            &decoder, chunk.data(), kChunkFrames, &read);
        if (read > 0) {
            out->samples.insert(
                out->samples.end(), chunk.begin(),
                chunk.begin() +
                    static_cast<std::size_t>(read * out->channels));
        }
        if (result == MA_AT_END) {
            break; // the one normal way a decode ends
        }
        if (result != MA_SUCCESS) {
            // A real decode error is not end-of-file: report it instead
            // of silently truncating the source.
            std::fprintf(stderr,
                         "error: decoding '%s' failed mid-stream "
                         "(ma_result %d)\n",
                         path.c_str(), static_cast<int>(result));
            ma_decoder_uninit(&decoder);
            return false;
        }
        if (read == 0) {
            break; // defensive: success with no progress and no end mark
        }
    }
    ma_decoder_uninit(&decoder);
    if (out->samples.empty()) {
        std::fprintf(stderr, "error: '%s' decoded to zero frames\n",
                     path.c_str());
        return false;
    }
    out->frames = out->samples.size() / out->channels;
    return true;
}

} // namespace harkenhost

#endif // HARKEN_HOST_WAV_LOADER_H
