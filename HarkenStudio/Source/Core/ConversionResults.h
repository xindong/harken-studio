#pragma once

#include "ConversionResolution.h"
#include "Components/VSound.h"

#include <QJsonArray>
#include <QStringList>
#include <QVector>
#include <functional>
#include <memory>
#include <optional>

namespace harken::studio::ConversionResults {

struct FileProperties
{
    QString path;
    QString format;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint64_t frames = 0;
    uint64_t bytes = 0;

    static FileProperties fromSource(const AssetInfo& asset);
    QJsonObject toJson() const;
};

struct SourceResult
{
    QUuid soundGuid;
    QUuid presetGuid;
    SoundSourceDef source;
    QString soundName;
    QString sourcePath;
    QString language;
    FileProperties original;
    std::optional<FileProperties> converted;

    QJsonObject toJson() const;
};

std::optional<SourceResult> inspectSource(const ProjectData& project,
    const ProjectItem& sound, const SoundSourceDef& source, const AssetInfo& asset,
    ProjectPlatformId platform, const QString& language = {},
    double scheduleOffsetSec = 0.0, QString* error = nullptr);
bool hasCachedSource(const ProjectData& project, const ResolvedConversion& conversion,
    const SoundSourceDef& source, const AssetInfo& asset, double scheduleOffsetSec = 0.0);
QVector<SourceResult> query(const ProjectData& project, const QUuid& presetGuid,
    ProjectPlatformId platform, QString* error = nullptr);
QJsonArray toJson(const QVector<SourceResult>& results);

struct BatchResult
{
    bool canceled = false;
    int converted = 0;
    int cached = 0;
    QStringList errors;

    QJsonObject toJson() const;
};

class Batch
{
public:
    static std::shared_ptr<Batch> create(const ProjectData& project,
        const QUuid& presetGuid, ProjectPlatformId platform, QString* error = nullptr);
    ~Batch();
    BatchResult run(const std::function<bool()>& cancelCheck = {});
    void cancel();

private:
    struct Impl;
    Batch();
    std::unique_ptr<Impl> impl_;
};

}
