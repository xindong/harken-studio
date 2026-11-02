#pragma once

#include "Tooling/Harkenizer/Core/ProjectPackExporter.h"
#include "Tooling/Harkenizer/Core/ProjectBuildSnapshot.h"

namespace VNX {

class PackBuildState
{
public:
    PackBuildState(std::shared_ptr<const ProjectBuildSnapshot> snapshot, const QByteArray& compiler);
    void select(const ProjectPackExporter::ExportOptions& options, const QString& outputDirectory);
    bool restore(ProjectPackExporter::ExportResult* result,
                 std::vector<ProjectPackExporter::PackBuildRecord>* records) const;
    void applyCounts(ProjectPackExporter::ExportResult& result) const;
    bool isCurrent(const QString& relativePackPath) const;
    bool isWaveformCurrent(const QString& relativePackPath, const QString& relativeWaveformPath) const;
    void recordWaveformOutput(const QString& absolutePath);
    void save(const ProjectPackExporter::ExportResult& result,
              const std::vector<ProjectPackExporter::PackBuildRecord>& records);
    bool flush();
    void retainOutputs(const QString& outputRoot, const QMap<QString, QJsonArray>& outputs);
    QStringList outputPaths() const;
    QString resourceFingerprint(const QString& relativePackPath) const;
    void setResourceFingerprint(const QString& relativePackPath, const QString& fingerprint);
    void prune() const;

private:
    void deriveInputs(const QSet<QString>* successfulPaths = nullptr);
    bool savedPackMatches(const QString& relativePackPath) const;
    bool savedCompanionMatches(const QString& relativePackPath) const;

    std::shared_ptr<const ProjectBuildSnapshot> snapshot_;
    QString compiler_;
    QString cachePath_;
    QString outputDirectory_;
    ProjectPackExporter::ExportOptions options_;
    QJsonObject targets_;
    QJsonObject persisted_;
    QJsonObject waveforms_;
    QJsonObject persistedWaveforms_;
    QJsonObject packs_;
    QMap<QString, QString> inputs_;
    QMap<QString, QString> resources_;
    mutable QHash<QString, bool> packMatches_;
    mutable QHash<QString, bool> companionMatches_;
    mutable QHash<QString, bool> current_;
    mutable QHash<QString, QString> waveformStates_;
    std::vector<ProjectPackExporter::PackBuildRecord> records_;
    QHash<const ProjectItem*, QString> objectDigests_;
    int objectCount_ = 0;
    int mediaCount_ = 0;
    int actionCount_ = 0;
    int emptyActionCount_ = 0;
    bool reusable_ = false;
    bool inputsReady_ = false;
    bool legacyCleanup_ = true;
};

}
