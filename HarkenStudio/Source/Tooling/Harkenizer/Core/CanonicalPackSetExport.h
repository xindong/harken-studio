#pragma once

#include "Tooling/Harkenizer/Core/ProjectPackExporter.h"

#include <harken_studio/ObjectId.h>

#include <QString>
#include <set>
#include <vector>

namespace VNX {

struct PackBuildPlan;

struct CanonicalPackSetExportRequest
{
    const ProjectData* project = nullptr;
    QString outputDir;
    ProjectPackExporter::ExportOptions options;
    std::set<harkenstudio::ObjectId> includedActionIds;
    const PackBuildPlan* plan = nullptr;
    harkenstudio::ObjectId snapshotBus = harkenstudio::kMasterBusObjectId;
    harkenstudio::ObjectId snapshotParent = {};
    harkenstudio::ObjectId snapshotSendSource = {};
    harkenstudio::ObjectId snapshotSendTarget = {};
    harkenstudio::ObjectId snapshotDownmix = harkenstudio::kMasterBusObjectId;
    harkenstudio::ObjectId snapshotMaster = harkenstudio::kMasterBusObjectId;
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

bool RecoverPackOutput(const QString& outputDir, QString* error);

bool ReadValidatedPackPublication(const QString& directory, const QString& projectId,
                                 const QString& platformId, QJsonArray* packs,
                                 QStringList* managedFiles, QString* error,
                                 const QStringList& expectedPackPaths,
                                 const std::function<bool()>& cancelCheck,
                                 const PackPublicationProof* publication);

inline bool ValidateSnapshotBusIds(harkenstudio::ObjectId bus, harkenstudio::ObjectId parent,
                                   harkenstudio::ObjectId sendSource, harkenstudio::ObjectId sendTarget,
                                   harkenstudio::ObjectId downmix, harkenstudio::ObjectId master)
{
    const harkenstudio::ObjectId ids[] = {bus, parent, sendSource, sendTarget, downmix, master};
    for (const harkenstudio::ObjectId id : ids) {
        if (id.high != 0)
            return false;
    }
    return true;
}

} // namespace VNX
