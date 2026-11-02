#pragma once

#include "Tooling/Harkenizer/Core/ProjectPackExporter.h"
#include "Tooling/Harkenizer/Core/ProjectBuildSnapshot.h"

namespace VNX {

class PackBuildState
{
public:
    static std::shared_ptr<PackBuildState> prepare(
        std::shared_ptr<const ProjectBuildSnapshot> snapshot,
        const ProjectPackExporter::ExportOptions& options,
        const QString& outputDirectory, const QByteArray& buildIdentity,
        std::optional<ProjectBuildIdentity>& sharedIdentity);

    bool restore(ProjectPackExporter::ExportResult* result,
                 std::vector<ProjectPackExporter::PackBuildRecord>* records) const;
    bool save(const ProjectPackExporter::ExportResult& result,
              const std::vector<ProjectPackExporter::PackBuildRecord>& records,
              const QMap<uint32_t, QString>& sourceDependencies);
    QString resourceFingerprint(const QString& relativePackPath, const QString& contentHash) const;
    void setResourceFingerprint(const QString& relativePackPath, const QString& contentHash,
                                const QString& fingerprint);
    void setMediaArtifacts(const QString& relativePackPath, QStringList artifacts);
    static void prune(const ProjectBuildSnapshot& snapshot);

private:
    std::shared_ptr<const ProjectBuildSnapshot> snapshot_;
    std::function<bool()> canceled_;
    QString outputDirectory_;
    ProjectPlatform platform_;
    QString cachePath_;
    QString fingerprint_;
    QMap<uint32_t, QString> objectGuids_;
    std::weak_ptr<QLockFile> platformLock_;
    QJsonObject saved_;
    QJsonObject resources_;
};

}
