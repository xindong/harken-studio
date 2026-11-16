// Minimal PCM16 RIFF/WAVE writer for the host tool's --synth mode: the
// demo synthesizes its sources, writes them as ordinary wav files, and
// then loads them back through the same decoder path a user-supplied
// file takes — so "loads wav files" is exercised for real, not shortcut
// with in-memory buffers. Host-side only; never part of the engine.
#ifndef HARKEN_HOST_WAV_WRITER_H
#define HARKEN_HOST_WAV_WRITER_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace harkenhost {

// Writes interleaved float samples as a 16-bit PCM wav file. Floats are
// clamped to [-1, 1] and scaled to int16. Returns false on any I/O
// failure. x64 Windows is little-endian, matching the RIFF layout, so
// fields are written directly.
inline bool WriteWav16(const std::string& path,
                       const std::vector<float>& interleaved,
                       std::uint32_t channels, std::uint32_t sampleRate) {
    if (channels == 0 || interleaved.size() % channels != 0) {
        return false;
    }
    std::FILE* file = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&file, path.c_str(), "wb") != 0) {
        file = nullptr;
    }
#else
    file = std::fopen(path.c_str(), "wb");
#endif
    if (file == nullptr) {
        return false;
    }

    const std::uint32_t bytesPerSample = 2;
    const std::uint32_t blockAlign = channels * bytesPerSample;
    const std::uint32_t dataBytes =
        static_cast<std::uint32_t>(interleaved.size() * bytesPerSample);
    const std::uint32_t byteRate = sampleRate * blockAlign;
    const std::uint32_t riffSize = 36 + dataBytes;
    const std::uint16_t formatPcm = 1;
    const std::uint16_t bitsPerSample = 16;
    const std::uint32_t fmtSize = 16;
    const std::uint16_t channels16 = static_cast<std::uint16_t>(channels);

    bool ok = true;
    auto write = [&](const void* data, std::size_t bytes) {
        ok = ok && std::fwrite(data, 1, bytes, file) == bytes;
    };
    write("RIFF", 4);
    write(&riffSize, 4);
    write("WAVE", 4);
    write("fmt ", 4);
    write(&fmtSize, 4);
    write(&formatPcm, 2);
    write(&channels16, 2);
    write(&sampleRate, 4);
    write(&byteRate, 4);
    write(&blockAlign, 2);
    write(&bitsPerSample, 2);
    write("data", 4);
    write(&dataBytes, 4);

    std::vector<std::int16_t> pcm(interleaved.size());
    for (std::size_t i = 0; i < interleaved.size(); ++i) {
        float sample = interleaved[i];
        if (sample > 1.0f) {
            sample = 1.0f;
        } else if (sample < -1.0f) {
            sample = -1.0f;
        }
        pcm[i] = static_cast<std::int16_t>(sample * 32767.0f);
    }
    if (!pcm.empty()) {
        write(pcm.data(), pcm.size() * sizeof(std::int16_t));
    }

    ok = std::fclose(file) == 0 && ok;
    return ok;
}

} // namespace harkenhost

#endif // HARKEN_HOST_WAV_WRITER_H
