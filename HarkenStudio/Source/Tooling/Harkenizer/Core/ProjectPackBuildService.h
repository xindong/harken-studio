#pragma once

#include "Tooling/Harkenizer/Core/ProjectPackExporter.h"

#include <QElapsedTimer>

namespace VNX::ProjectPackBuildService {

enum class ArtifactOrder { Requested, PlatformId };
enum class Finalization { None, Complete, Partial };

struct PlatformResult
{
    QString platform;
    ProjectPlatform definition;
    QString outputDir;
    ProjectPackExporter::ExportResult result;
    std::vector<ProjectPackExporter::PackBuildRecord> records;
    QElapsedTimer timer;
};

struct Request
{
    QString outputRoot;
    QStringList platforms;
    ProjectPackExporter::ExportOptions options;
    std::shared_ptr<QLockFile> outputLock;
    bool stopOnCanceledPlatform = true;
    ArtifactOrder artifactOrder = ArtifactOrder::Requested;
    std::function<bool()> finalizationCancelCheck;
    std::function<void()> preflightFinished;
    std::function<void(const PlatformResult&)> platformFinished;
};

struct Result
{
    bool outputLocked = false;
    bool preflightSucceeded = false;
    bool canceled = false;
    bool allPlatformsSucceeded = false;
    QString preflightError;
    QString preflightSummary;
    QStringList succeededPlatforms;
    std::vector<PlatformResult> platforms;
    QVector<PackArtifactMetadata> artifacts;
    Finalization finalization = Finalization::None;
    ProjectPackExporter::ExportResult finalizationResult;
};

Result run(const Request& request);

}
