#pragma once

#include "AssetTypes.h"

#include <QMap>
#include <functional>
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
    BuildSourceInputs();
    ~BuildSourceInputs();

    QString sourceFingerprint(const AssetInfo& asset, QString* error = nullptr);
    void beginDependencyScope();
    bool adopt(BuildSourceInputs& probes, QString* error = nullptr);
    QMap<uint32_t, QString> fingerprints() const;
    std::shared_ptr<const BuildSourceInput> capture(const AssetInfo& asset, QString* error = nullptr);
    bool verifyUnchanged(QString* error = nullptr,
                         const std::function<bool()>& canceled = {}) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
