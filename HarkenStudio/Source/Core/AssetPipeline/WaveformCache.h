
#pragma once

#include <QString>
#include <QFuture>
#include <QObject>
#include <cstdint>
#include <memory>
#include <vector>

namespace VNX {

struct ProjectData;
struct ProjectItem;

namespace VKD {
    constexpr uint32_t MAGIC        = 0x41444B56;
    constexpr uint32_t VERSION      = 2;
    constexpr uint32_t HEADER_SIZE  = 32;

    constexpr uint32_t SECTION_WAVE    = 0x45564157;
    constexpr uint32_t SECTION_BAND    = 0x444E4142;
    constexpr uint32_t SECTION_MIDSIDE = 0x5344494D;
    constexpr uint32_t SECTION_LOUD    = 0x44554F4C;
    constexpr uint32_t SECTION_SPEC    = 0x43455053;
    constexpr uint32_t SECTION_MARK    = 0x4B52414D;

    constexpr uint32_t SECTION_DIR_ENTRY_SIZE = 12;
    constexpr uint32_t WAVE_HEADER_SIZE = 24;
    constexpr uint32_t BAND_HEADER_SIZE = 8;

    constexpr uint32_t DEFAULT_NUM_PEAKS = 2048;
}

struct WaveformPeakData
{
    uint32_t numPeaks       = 0;
    uint32_t samplesPerPeak = 0;
    uint32_t numChannels    = 0;
    uint32_t sampleRate     = 0;
    double   durationSec    = 0.0;

    std::vector<float> peaks;

    std::vector<float> bandData;

    std::vector<float> midSidePeaks;

    const float* getChannelMinPeaks(uint32_t ch) const;

    const float* getChannelMaxPeaks(uint32_t ch) const;

    const float* getChannelBand(uint32_t ch) const;

    std::vector<float> getMonoMinPeaks() const;

    std::vector<float> getMonoMaxPeaks() const;

    bool isValid() const { return numPeaks > 0 && numChannels > 0 && peaks.size() == numPeaks * 2 * numChannels; }

    bool hasBandData() const { return numChannels > 0 && numPeaks > 0 && bandData.size() == numPeaks * 3 * numChannels; }

    bool hasMidSideData() const { return numPeaks > 0 && midSidePeaks.size() == numPeaks * 4; }

    const float* getMidMinPeaks() const { return hasMidSideData() ? midSidePeaks.data() : nullptr; }

    const float* getMidMaxPeaks() const { return hasMidSideData() ? midSidePeaks.data() + 1 : nullptr; }

    const float* getSideMinPeaks() const { return hasMidSideData() ? midSidePeaks.data() + numPeaks * 2 : nullptr; }

    const float* getSideMaxPeaks() const { return hasMidSideData() ? midSidePeaks.data() + numPeaks * 2 + 1 : nullptr; }
};

class WaveformCache : public QObject
{
    Q_OBJECT

public:
    static WaveformCache& instance();

    static bool generate(const QString& audioFilePath, uint32_t targetPeaks = VKD::DEFAULT_NUM_PEAKS);

    static QFuture<bool> generateAsync(const QString& audioFilePath, uint32_t targetPeaks = VKD::DEFAULT_NUM_PEAKS);

    static bool load(const QString& audioFilePath, WaveformPeakData& outData);

    static std::shared_ptr<const WaveformPeakData> loadShared(const QString& audioFilePath);

    static WaveformPeakData readRange(const QString& audioFilePath,
                                      uint64_t startFrame, uint64_t frameCount,
                                      uint32_t targetPeaks);

    static bool isValid(const QString& audioFilePath);

    static QString cachePath(const QString& audioFilePath);

    static bool generateMidSide(const QString& audioFilePath, WaveformPeakData& outData);

    static QFuture<bool> generateMidSideAsync(const QString& audioFilePath, WaveformPeakData& outData);

    static bool remove(const QString& audioFilePath);

    static QFuture<int> rebuildAllAsync(const struct ProjectData* project,
                                         const QString& audioSourcesDir,
                                         int maxConcurrent = 4);

signals:
    void cacheReady(const QString& audioFilePath);

private:
    WaveformCache() = default;

    static bool generateForRevision(const QString& audioFilePath,
                                    uint32_t targetPeaks,
                                    quint64 revision);
    static bool generateMidSideForRevision(const QString& audioFilePath,
                                           WaveformPeakData& outData,
                                           quint64 revision);
    static bool writeVKD(const QString& vkdPath,
                          const QString& audioFilePath,
                          const WaveformPeakData& data,
                          uint64_t sourceFileSize,
                          int64_t sourceMtime,
                          quint64 revision);

    static bool readAndValidateHeader(QDataStream& stream,
                                       uint64_t expectedFileSize,
                                       int64_t expectedMtime,
                                       uint32_t& outSectionCount);

    static bool generateFromDecoded(const QString& audioFilePath, const QString& vkdPath,
                                     uint32_t targetPeaks,
                                     uint64_t sourceFileSize,
                                     int64_t sourceMtime,
                                     quint64 revision);
};

}
