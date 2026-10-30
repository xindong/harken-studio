#pragma once

#include "Tooling/Harkenizer/Core/ProjectPackExporter.h"

#include <QByteArray>
#include <QMap>

namespace VNX {

class PackBuildReceipt
{
public:
    static std::optional<PackBuildReceipt> prepare(
        std::shared_ptr<const ProjectBuildSnapshot> snapshot,
        const ProjectPackExporter::ExportOptions& options,
        const QString& outputDirectory, const QByteArray& buildIdentity);

    bool restore(ProjectPackExporter::ExportResult* result,
                 std::vector<ProjectPackExporter::PackBuildRecord>* records) const;
    bool save(const ProjectPackExporter::ExportResult& result,
              const std::vector<ProjectPackExporter::PackBuildRecord>& records,
              bool cacheable, const QMap<uint32_t, QString>& sourceDependencies) const;

private:
    std::shared_ptr<const ProjectBuildSnapshot> snapshot_;
    std::function<bool()> canceled_;
    QString outputDirectory_;
    QString platform_;
    QString cachePath_;
    QString scope_;
    QString fingerprint_;
    QMap<uint32_t, QString> objectGuids_;
    std::weak_ptr<QLockFile> platformLock_;
};

}
