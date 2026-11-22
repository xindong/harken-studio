#pragma once

#include "Tooling/Harkenizer/Core/ProjectPackExporter.h"

#include <harken_studio/ObjectId.h>

#include <QString>
#include <set>
#include <vector>
#include <utility>

namespace harken::studio {

struct PackBuildPlan;

struct CanonicalPackSetExportRequest
{
    const ProjectData* project = nullptr;
    QString outputDir;
    ProjectPackExporter::ExportOptions options;
    std::set<harken::studio::types::ObjectId> includedActionIds;
    std::set<std::pair<harken::studio::types::ObjectId, QString>> failedActionVariants;
    const PackBuildPlan* plan = nullptr;
    harken::studio::types::ObjectId snapshotBus = harken::studio::types::kMasterBusObjectId;
    harken::studio::types::ObjectId snapshotParent = {};
    harken::studio::types::ObjectId snapshotSendSource = {};
    harken::studio::types::ObjectId snapshotSendTarget = {};
    harken::studio::types::ObjectId snapshotDownmix = harken::studio::types::kMasterBusObjectId;
    harken::studio::types::ObjectId snapshotMaster = harken::studio::types::kMasterBusObjectId;
};

ProjectPackExporter::ExportResult ExportCanonicalPackSet(
    ProjectPackExporter& exporter, const CanonicalPackSetExportRequest& request,
    std::vector<ProjectPackExporter::PackBuildRecord>* outRecords);

bool ReadPackPublication(const QString& directory, const QString& projectId,
                         const QString& platformId, QJsonArray* packs,
                         QStringList* managedFiles, QString* error,
                         bool requireComplete = true,
                         const QStringList& expectedPackPaths = {},
                         bool allowPlatformReplacement = false,
                         const std::function<bool()>& cancelCheck = {});

inline bool ValidateSnapshotBusIds(harken::studio::types::ObjectId bus, harken::studio::types::ObjectId parent,
                                   harken::studio::types::ObjectId sendSource, harken::studio::types::ObjectId sendTarget,
                                   harken::studio::types::ObjectId downmix, harken::studio::types::ObjectId master)
{
    const harken::studio::types::ObjectId ids[] = {bus, parent, sendSource, sendTarget, downmix, master};
    for (const harken::studio::types::ObjectId id : ids) {
        if (id.high != 0)
            return false;
    }
    return true;
}

} // namespace harken::studio
