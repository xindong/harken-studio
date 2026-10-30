#ifndef HARKEN_STUDIO_WAV_LOADER_H
#define HARKEN_STUDIO_WAV_LOADER_H

#include "miniaudio.h"
#include "../../Common/Utf8Path.h"

#include <cstdint>
#include <string>
#include <vector>

namespace harkenstudiojson {

struct LoadedWav {
    std::vector<float> samples; // interleaved f32, native rate
    std::uint32_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint64_t frames = 0;
};
inline bool LoadWav(const std::string& path, LoadedWav* out,
                    bool allowMultichannel, std::string& diagnostics) {
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    config.encodingFormat = ma_encoding_format_wav;
    ma_decoder decoder;
#ifdef _WIN32
    const ma_result openResult = ma_decoder_init_file_w(
        Utf8Path(path).c_str(), &config, &decoder);
#else
    const ma_result openResult = ma_decoder_init_file(path.c_str(), &config, &decoder);
#endif
    if (openResult != MA_SUCCESS) {
        diagnostics += "error: cannot open or decode '" + path + "'\n";
        return false;
    }
    out->channels = decoder.outputChannels;
    out->sampleRate = decoder.outputSampleRate;
    if (!allowMultichannel && out->channels != 1 &&
        out->channels != 2) {
        diagnostics += "error: '" + path + "' has " + std::to_string(out->channels)
            + " channels; only mono and stereo are supported\n";
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
            diagnostics += "error: decoding '" + path + "' failed mid-stream (ma_result "
                + std::to_string(static_cast<int>(result)) + ")\n";
            ma_decoder_uninit(&decoder);
            return false;
        }
        if (read == 0) {
            break; // defensive: success with no progress and no end mark
        }
    }
    ma_decoder_uninit(&decoder);
    if (out->samples.empty()) {
        diagnostics += "error: '" + path + "' decoded to zero frames\n";
        return false;
    }
    out->frames = out->samples.size() / out->channels;
    return true;
}

} // namespace harkenstudiojson

#endif // HARKEN_HOST_WAV_LOADER_H
