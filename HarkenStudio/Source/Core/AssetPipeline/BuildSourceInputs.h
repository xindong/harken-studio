#pragma once

#include "AssetTypes.h"

#include <memory>

class QTemporaryDir;

namespace VNX {

class BuildSourceInput
{
public:
    QString originalPath;
    QString inputPath;
    QString contentHash;
    AudioMetadata metadata;

private:
    friend class BuildSourceInputs;
    std::shared_ptr<QTemporaryDir> storage_;
};

class BuildSourceInputs
{
public:
    explicit BuildSourceInputs(QString projectRoot = {}, QHash<uint32_t, AssetInfo> assets = {});
    ~BuildSourceInputs();

    void beginRequest();
    QHash<uint32_t, AssetInfo> assets() const;
    std::shared_ptr<const BuildSourceInput> inspect(const AssetInfo& asset, QString* error = nullptr);
    std::shared_ptr<const BuildSourceInput> capture(const AssetInfo& asset, QString* error = nullptr);
    QByteArray waveformState(uint32_t sourceID) const;
    void setWaveformState(uint32_t sourceID, const QByteArray& state);
    bool save();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
