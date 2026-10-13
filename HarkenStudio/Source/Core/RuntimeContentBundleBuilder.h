#pragma once

#include "ProjectPlatform.h"

#include <QString>
#include <QStringList>

#include <cstdint>
#include <vector>

namespace VNX {

struct ProjectData;

struct RuntimeContentPack
{
    QString path;
    qint64 size = 0;
    QString sha256;
};

struct RuntimeContentAction
{
    QString name;
    harkenstudio::ObjectId stableId = {};
};

struct RuntimeContentPublication
{
    QString path;
    QString identitySha256;
};

struct RuntimeContentBundleResult
{
    bool success = false;
    QString error;
    QString manifestPath;
    QString bundleDirectory;
    QString bundleId;
    QString platform;
    QString platformName;
    BasePlatform basePlatform = BasePlatform::Windows;
    // Preview envelope path. Games load orderedPacks; this file may be
    // omitted, deleted, or rebuilt without changing the pack contract.
    QString currentWorldPath;
    std::uint64_t worldSemanticFingerprint = 0;
    std::vector<RuntimeContentPack> orderedPacks;
    std::vector<RuntimeContentPublication> requiredPublications;
    std::vector<RuntimeContentAction> actions;
    QStringList warnings;
};

class RuntimeContentBundleBuilder
{
public:
    static RuntimeContentBundleResult build(
        ProjectData& project,
        const QString& publishRoot,
        const QString& platform);
    static bool deploy(
        const RuntimeContentBundleResult& bundle,
        const QString& targetRoot,
        QString* error = nullptr);
};

}
