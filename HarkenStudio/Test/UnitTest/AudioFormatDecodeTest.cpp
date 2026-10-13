#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/WaveformCache.h"
#include "RuntimeBridge/PackagingBridge.h"

#include <gtest/gtest.h>

#include <ogg/ogg.h>
#include <vorbis/vorbisenc.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <array>
#include <cstdint>
#include <vector>

namespace {

void appendLe16(QByteArray& out, uint16_t value)
{
    out.append(static_cast<char>(value));
    out.append(static_cast<char>(value >> 8));
}

void appendLe32(QByteArray& out, uint32_t value)
{
    out.append(static_cast<char>(value));
    out.append(static_cast<char>(value >> 8));
    out.append(static_cast<char>(value >> 16));
    out.append(static_cast<char>(value >> 24));
}

void appendBe16(QByteArray& out, uint16_t value)
{
    out.append(static_cast<char>(value >> 8));
    out.append(static_cast<char>(value));
}

void appendBe32(QByteArray& out, uint32_t value)
{
    out.append(static_cast<char>(value >> 24));
    out.append(static_cast<char>(value >> 16));
    out.append(static_cast<char>(value >> 8));
    out.append(static_cast<char>(value));
}

bool writeBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

bool writeWav(const QString& path)
{
    constexpr uint16_t channels = 1;
    constexpr uint32_t sampleRate = 8000;
    const std::array<int16_t, 8> samples{-32768, -16384, 0, 16384,
                                         32767, 16384, 0, -16384};
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));

    QByteArray bytes;
    bytes.append("RIFF", 4);
    appendLe32(bytes, 36 + dataBytes);
    bytes.append("WAVEfmt ", 8);
    appendLe32(bytes, 16);
    appendLe16(bytes, 1);
    appendLe16(bytes, channels);
    appendLe32(bytes, sampleRate);
    appendLe32(bytes, sampleRate * channels * sizeof(int16_t));
    appendLe16(bytes, channels * sizeof(int16_t));
    appendLe16(bytes, 16);
    bytes.append("data", 4);
    appendLe32(bytes, dataBytes);
    for (int16_t sample : samples)
        appendLe16(bytes, static_cast<uint16_t>(sample));
    return writeBytes(path, bytes);
}

QByteArray makeAiff(uint32_t declaredFrames = 8)
{
    const std::array<int16_t, 8> samples{-32768, -16384, 0, 16384,
                                         32767, 16384, 0, -16384};
    const uint32_t sampleBytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));

    QByteArray bytes;
    bytes.append("FORM", 4);
    appendBe32(bytes, 4 + 8 + 18 + 8 + 8 + sampleBytes);
    bytes.append("AIFFCOMM", 8);
    appendBe32(bytes, 18);
    appendBe16(bytes, 1);
    appendBe32(bytes, declaredFrames);
    appendBe16(bytes, 16);
    const std::array<unsigned char, 10> rate{
        0x40, 0x0B, 0xFA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    bytes.append(reinterpret_cast<const char*>(rate.data()), rate.size());
    bytes.append("SSND", 4);
    appendBe32(bytes, 8 + sampleBytes);
    appendBe32(bytes, 0);
    appendBe32(bytes, 0);
    for (int16_t sample : samples)
        appendBe16(bytes, static_cast<uint16_t>(sample));
    return bytes;
}

bool writeOggPage(QFile& file, const ogg_page& page)
{
    return file.write(reinterpret_cast<const char*>(page.header), page.header_len) ==
               page.header_len &&
           file.write(reinterpret_cast<const char*>(page.body), page.body_len) ==
               page.body_len;
}

bool writeOgg(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;

    vorbis_info info{};
    vorbis_info_init(&info);
    if (vorbis_encode_init_vbr(&info, 1, 8000, 0.2f) != 0)
    {
        vorbis_info_clear(&info);
        return false;
    }

    vorbis_comment comment{};
    vorbis_comment_init(&comment);
    vorbis_dsp_state dsp{};
    if (vorbis_analysis_init(&dsp, &info) != 0)
    {
        vorbis_comment_clear(&comment);
        vorbis_info_clear(&info);
        return false;
    }
    vorbis_block block{};
    vorbis_block_init(&dsp, &block);
    ogg_stream_state stream{};
    ogg_stream_init(&stream, 1);

    ogg_packet header{};
    ogg_packet comments{};
    ogg_packet codebooks{};
    bool success = vorbis_analysis_headerout(&dsp, &comment, &header, &comments,
                                              &codebooks) == 0;
    if (success)
    {
        success = ogg_stream_packetin(&stream, &header) == 0 &&
                  ogg_stream_packetin(&stream, &comments) == 0 &&
                  ogg_stream_packetin(&stream, &codebooks) == 0;
    }

    ogg_page page{};
    while (success && ogg_stream_flush(&stream, &page) != 0)
        success = writeOggPage(file, page);

    constexpr int frameCount = 256;
    if (success)
    {
        float** buffer = vorbis_analysis_buffer(&dsp, frameCount);
        success = buffer != nullptr;
        if (success)
        {
            for (int frame = 0; frame < frameCount; ++frame)
                buffer[0][frame] = static_cast<float>((frame % 32) - 16) / 16.0f;
            success = vorbis_analysis_wrote(&dsp, frameCount) == 0 &&
                      vorbis_analysis_wrote(&dsp, 0) == 0;
        }
    }

    bool endOfStream = false;
    while (success && !endOfStream)
    {
        const int blockResult = vorbis_analysis_blockout(&dsp, &block);
        if (blockResult != 1)
        {
            success = false;
            break;
        }
        success = vorbis_analysis(&block, nullptr) == 0 &&
                  vorbis_bitrate_addblock(&block) == 0;
        ogg_packet packet{};
        while (success && vorbis_bitrate_flushpacket(&dsp, &packet) == 1)
        {
            success = ogg_stream_packetin(&stream, &packet) == 0;
            while (success)
            {
                const int pageResult = ogg_stream_pageout(&stream, &page);
                if (pageResult == 0)
                    break;
                success = writeOggPage(file, page);
                endOfStream = ogg_page_eos(&page) != 0;
            }
        }
    }

    ogg_stream_clear(&stream);
    vorbis_block_clear(&block);
    vorbis_dsp_clear(&dsp);
    vorbis_comment_clear(&comment);
    vorbis_info_clear(&info);
    return success && endOfStream;
}

QByteArray mp3Fixture()
{
    return QByteArray::fromBase64(QByteArrayLiteral(
        "SUQzBAAAAAAAI1RTU0UAAAAPAAADTGF2ZjYyLjEyLjEwMgAAAAAAAAAAAAAA/+M4wAAAAAAAAAAAAEluZm8AAAAPAAAABQAAAkAAgICAgICAgICAgICAgICAgICAgKCgoKCgoKCgoKCgoKCgoKCgoKCgwMDAwMDAwMDAwMDAwMDAwMDAwMDg4ODg4ODg4ODg4ODg4ODg4ODg4P//////////////////////////AAAAAExhdmYAAAAAAAAAAAAAAAAAAAAAACQCwAAAAAAAAAJArzIjVAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/+MYxAAMQIrduUMAAqoFLbbQHd3d3P0RERC93d3/93AwMDD7igIXFwfD+UDEp4f4EdoD/AjtAf4Edz/R7+iAAYYQAAAAyLxN/+MYxAoPcPbE+ZMoABj/l0mTIi3+LOIaAPgLmqAB+iQmYHaJEwiBQ6Yxv/xEOh0weDxTv8qIgqCoiPf9YKiI8Gn//v27wMAq/+MYxAcOOMo8AZcoAAbgDMZU/YDB4gRaBgGQQGBjAy1CBgfoIvAwI0ISAxLRbWAwGUD0gb4L8H//9Hq9f//S///ff1KZqVxr/+MYxAkNyN68AZk4AP//nc8ojEIy6XxE0dZbYhg4oHzbmlUj83f9GPPdgdAtFBEbD/o6QkAmnf+os0ZVw+fPo13qtUMaVhVr/+MYxAwQGNpkAc8YAdoxIchzLIrkOQ6c/RbQ1KFKU0VDF8E5TRjn6aJ0xYSuVz7UJ8+3mr2uIKwFHBR2IL0V+X5KTEFNRaqq"));
}

QByteArray flacFixture()
{
    return QByteArray::fromBase64(QByteArrayLiteral(
        "ZkxhQwAAACICQAJAAAEEAAE/AfQA8AAABkDjfj5UtjnVIXS+vr57KA6ChAAALg0AAABMYXZmNjIuMTIuMTAyAQAAABUAAABlbmNvZGVyPUxhdmY2Mi4xMi4xMDL/+CQIAMpOAAAP4B3hKFguCy5MKRMe/+WZwrpycsdUmQQOlMG7PswAMZGWCYFhQIYQ7NPURCQoCMT98ZSCwLCKJ+6MTCwiEhSTmyDEwsCYKBGE5wxBomCgkFhZDEGRoYIwSIKEUvc0GExEQiQWoyDI9YiISQTEZHuaoSELAmIyDENDCMSEgTArENHGRlgmBYUCGEOzT1EQkKAjE/fGUgsCwiifujEwsIhIUk5sgxMLAmCgRhOcMQaJgoJBYWQxBkaGCMEiChFL3NBhMREIkFqMgyPWIiEkExGR7mqEhCwJiMgxDQwjEhIEwKxDRxkZYJgWFAhhDs09REJCgIxP3xlILAsIon7oxMLCISFJObIMTCwJgoEYTnDEGiYKCQWFkMQZGhgjBIgoRS9zQYTERCJBajIMj1iIhJBMRke5oAxZ//gkCAHNTtWX0VbSmtk85HPy7QLxEp3lmie9aFBXzFAon6RAun9WADCQTC4yOvCMELAslZDmRiyQJgki1tPllAWIorR2ZGLIUJIqQyaclBIgorEGnGIyQWEkRGENkGXIsEhYTLhiHWUQoRKLQZPWigkQWEy2nWlAsUJUjs1omBQkEwuMjrwjBCwLJWQ5kYskCYJItbT5ZQFiKK0dmRiyFCSKkMmnJQSIKKxBpxiMkFhJERhDZBlyLBIWEy4Yh1lEKESi0GT1ooJEFhMtp1pQLFCVI7NaJgUJBMLjI68IwQsCyVkOZGLJAmCSLW0+WUBYiitHZkYshQkipDJpyUEiCisQacYjJBYSREYQ2QZciwSFhMuGIdZRChEotBk9aKCRBYDVIf/4dAgCAb9iTiQeF9wIyPir6Wvc19Rr0Sblmfy5PnPrWkDvbrtBiv7pgEzEZDdHz9au2IhiQTo+Pix25EIg3ElQsfrlSCQaGhnZ6rz1IzGQkEVF/X1U2NBoRLl/X12poZiISSLXXFamg0Exuy1adi5KJjMSEuLj585uZCGzJd+9XFJSMxGQlkfHy1IRmIyG6Pn61dsRDEgnR8fFjtyIRBuJKhY/XKkEg0NDOz1XnqRmMhIIqL+vqpsaDQiXL+vrtTQzEQkkWuuK1NBoJjdlq07FyUTGYkJcXHz5zcyENmS796uKSkZiMhLI+PlqQjMRkN0fP1q7YiGJBOj4+LHbkQiDAKfY"));
}

struct AudioCase
{
    QString fileName;
    QString format;
};

}

TEST(AudioFormatDecodeTest, DecodesAndBuildsWaveformsForAllSupportedExtensions)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    ASSERT_TRUE(writeWav(dir.filePath(QStringLiteral("tone.wav"))));
    ASSERT_TRUE(writeBytes(dir.filePath(QStringLiteral("tone.mp3")), mp3Fixture()));
    ASSERT_TRUE(writeBytes(dir.filePath(QStringLiteral("tone.flac")), flacFixture()));
    ASSERT_TRUE(writeOgg(dir.filePath(QStringLiteral("tone.ogg"))));
    ASSERT_TRUE(writeBytes(dir.filePath(QStringLiteral("tone.aiff")), makeAiff()));
    ASSERT_TRUE(writeBytes(dir.filePath(QStringLiteral("tone.aif")), makeAiff()));

    const std::array<AudioCase, 6> cases{{
        {QStringLiteral("tone.wav"), QStringLiteral("WAV")},
        {QStringLiteral("tone.mp3"), QStringLiteral("MP3")},
        {QStringLiteral("tone.flac"), QStringLiteral("FLAC")},
        {QStringLiteral("tone.ogg"), QStringLiteral("OGG")},
        {QStringLiteral("tone.aiff"), QStringLiteral("AIFF")},
        {QStringLiteral("tone.aif"), QStringLiteral("AIFF")},
    }};

    VNX::AssetImporter importer;
    for (const AudioCase& testCase : cases)
    {
        SCOPED_TRACE(testCase.fileName.toStdString());
        const QString path = dir.filePath(testCase.fileName);
        EXPECT_TRUE(VNX::AssetImporter::isFormatSupported(path));

        const auto info = HarkenEditor::Bridge::audioFileInfo(path);
        ASSERT_TRUE(info.valid);
        EXPECT_EQ(info.channels, 1);
        EXPECT_EQ(info.sampleRate, 8000);
        EXPECT_GT(info.frameCount, 0);
        EXPECT_GT(info.durationSeconds, 0.0);

        std::vector<float> samples;
        int channels = 0;
        int sampleRate = 0;
        ASSERT_TRUE(HarkenEditor::Bridge::decodeAudioFileInterleaved(
            path, samples, channels, sampleRate));
        EXPECT_EQ(channels, 1);
        EXPECT_EQ(sampleRate, 8000);
        EXPECT_FALSE(samples.empty());

        const VNX::AudioMetadata metadata = importer.analyzeFile(path);
        EXPECT_EQ(metadata.originalFormat, testCase.format);
        EXPECT_EQ(metadata.channels, 1);
        EXPECT_EQ(metadata.sampleRate, 8000u);
        EXPECT_GT(metadata.totalSamples, 0u);

        ASSERT_TRUE(VNX::WaveformCache::generate(path, 8));
        VNX::WaveformPeakData waveform;
        ASSERT_TRUE(VNX::WaveformCache::load(path, waveform));
        EXPECT_TRUE(waveform.isValid());
        EXPECT_TRUE(waveform.hasBandData());
        EXPECT_EQ(waveform.numChannels, 1u);
        EXPECT_EQ(waveform.sampleRate, 8000u);
    }
}

TEST(AudioFormatDecodeTest, RejectsCorruptAndUnsupportedInputs)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString validOgg = dir.filePath(QStringLiteral("valid.ogg"));
    ASSERT_TRUE(writeOgg(validOgg));
    QFile oggFile(validOgg);
    ASSERT_TRUE(oggFile.open(QIODevice::ReadOnly));
    const QByteArray oggBytes = oggFile.readAll();
    ASSERT_GT(oggBytes.size(), 64);
    ASSERT_TRUE(writeBytes(dir.filePath(QStringLiteral("truncated.ogg")),
                           oggBytes.first(oggBytes.size() - 16)));

    const QByteArray aiff = makeAiff();
    ASSERT_TRUE(writeBytes(dir.filePath(QStringLiteral("truncated.aiff")),
                           aiff.first(aiff.size() - 1)));
    ASSERT_TRUE(writeBytes(dir.filePath(QStringLiteral("oversized.aif")),
                           makeAiff(0xFFFFFFFFu)));
    ASSERT_TRUE(writeWav(dir.filePath(QStringLiteral("unsupported.wma"))));
    ASSERT_TRUE(writeWav(dir.filePath(QStringLiteral("unsupported.m4a"))));

    VNX::AssetImporter importer;
    for (const QString& name : {QStringLiteral("truncated.ogg"),
                                QStringLiteral("truncated.aiff"),
                                QStringLiteral("oversized.aif")})
    {
        SCOPED_TRACE(name.toStdString());
        const QString path = dir.filePath(name);
        EXPECT_FALSE(HarkenEditor::Bridge::audioFileInfo(path).valid);
        std::vector<float> samples;
        int channels = 0;
        int sampleRate = 0;
        EXPECT_FALSE(HarkenEditor::Bridge::decodeAudioFileInterleaved(
            path, samples, channels, sampleRate));
        EXPECT_TRUE(samples.empty());
        EXPECT_EQ(importer.analyzeFile(path).sampleRate, 0u);
        EXPECT_FALSE(VNX::WaveformCache::generate(path, 8));
    }

    for (const QString& name : {QStringLiteral("unsupported.wma"),
                                QStringLiteral("unsupported.m4a")})
    {
        const QString path = dir.filePath(name);
        EXPECT_FALSE(VNX::AssetImporter::isFormatSupported(path));
        QString error;
        EXPECT_FALSE(importer.validateFile(path, error));
        EXPECT_TRUE(error.contains(QStringLiteral("Unsupported audio format")));
    }
}
