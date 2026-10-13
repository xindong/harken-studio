
#pragma once

#include "MediaBuildSnapshot.h"
#include <QObject>
#include <QFuture>
#include <QThreadPool>
#include <QMutex>
#include <QHash>
#include <memory>
#include <atomic>
#include <vector>

namespace VNX {

struct ConversionResult
{
    bool success = false;
    uint32_t sourceID = 0;
    ProjectPlatformId platform;
    bool cacheHit = false;
    CacheEntry cacheEntry;
    QString cachePath;
    QString absolutePath;
    uint64_t outputSize = 0;
    float compressionRatio = 1.0f;
    ConversionSettings::Codec outputCodec = ConversionSettings::Codec::PCM_Float;
    uint32_t outputSampleRate = 0;
    uint16_t outputChannels = 0;
    QString errorMessage;
};

class AssetConverter : public QObject
{
    Q_OBJECT

public:
    explicit AssetConverter(QObject* parent = nullptr);
    ~AssetConverter() override;

    void setProjectRoot(const QString& root);
    void setBuildSnapshot(const MediaBuildSnapshot& snapshot);


    ConversionResult convert(uint32_t sourceID, ProjectPlatformId platform);
    ConversionResult convert(const MediaConversionRequest& request);

    QFuture<ConversionResult> convertAsync(uint32_t sourceID, ProjectPlatformId platform);

    QFuture<QList<ConversionResult>> convertBatchAsync(const QList<uint32_t>& sourceIDs,
                                                        ProjectPlatformId platform);

    bool isJobActive(uint32_t sourceID, ProjectPlatformId platform) const;
    void cancelJob(uint32_t sourceID, ProjectPlatformId platform);
    void cancelAllJobs();
    int activeJobCount() const;

    static bool resample(const std::vector<float>& input,
                         int inputSampleRate, int outputSampleRate,
                         int channels,
                         std::vector<float>& output);


    static void convertToMono(std::vector<float>& data, int channels);

    static bool processSamples(std::vector<float>& samples, int& sampleRate, int& channels,
                               const ConversionSettings& settings, LoopInfo& loop,
                               QString* error = nullptr);

    static bool writeFloatWav(const QString& path,
                              const std::vector<float>& interleaved,
                              uint16_t channels, uint32_t sampleRate);

signals:
    void conversionProgress(uint32_t sourceID, VNX::ProjectPlatformId platform, int percent);
    void conversionComplete(uint32_t sourceID, VNX::ProjectPlatformId platform, const QString& cachePath);
    void conversionFailed(uint32_t sourceID, VNX::ProjectPlatformId platform, const QString& error);

private:
    struct JobState {
        QFuture<ConversionResult> future;
        uint32_t sourceID = 0;
        ProjectPlatformId platform;
        std::atomic<bool> cancelled{false};
    };

    std::optional<MediaConversionRequest> captureRequest(
        uint32_t sourceID, ProjectPlatformId platform, QString* error) const;
    ConversionResult doConversion(const MediaConversionRequest& request,
                                  const std::shared_ptr<JobState>& job = {});

    bool decodeSource(const QString& path, std::vector<float>& samples,
                      int& channels, int& sampleRate);

    bool writeCacheFile(const QString& path, const std::vector<float>& interleaved,
                        uint16_t channels, uint32_t sampleRate);

    static LoopInfo remapLoopInfo(const LoopInfo& sourceLoop,
                                  uint64_t sourceFramesAfterTrim,
                                  uint32_t sourceSampleRate,
                                  uint32_t outputSampleRate,
                                  uint64_t outputFrames);

    QString projectRoot_;
    std::optional<MediaBuildSnapshot> snapshot_;
    QThreadPool threadPool_;
    mutable QMutex jobMutex_;
    QHash<QString, std::shared_ptr<JobState>> activeJobs_;
};

}
