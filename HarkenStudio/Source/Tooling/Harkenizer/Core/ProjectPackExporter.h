
#pragma once

#include "Tooling/Harkenizer/Core/PackExportSelection.h"

#include "ActionClosureSemantics.h"
#include "AssetPipeline/AssetTypes.h"
#include "PlatformContext.h"
#include "PackArtifactCache.h"
#include "PackGenerationRecord.h"
#include "ProjectTypes.h"
#include "RuntimeBridge/PackagingBridge.h"
#include <QObject>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <functional>
#include <cstdint>
#include <optional>
#include <map>
#include <set>
#include <string>
#include <vector>

class QLockFile;

namespace VNX {
    struct ProjectData;
    struct ProjectItem;
    struct PackBuildPlan;
    struct CanonicalPackSetExportRequest;
    struct PackBuildContext;
    struct WaveformPeakData;
    struct ProjectBuildSnapshot;
    struct MediaBuildSnapshot;
    class PackMediaPreparation;
    class PackBuildState;
}

namespace VNX {

namespace ProjectPackExporterInternal {

    struct PackResourceReuse
    {
        QString path;
        QString fingerprint;
    };

    struct ProjectPackExporterExportOptions
    {
        QString platform;
        PackExportSelection selection;
        std::shared_ptr<const ProjectBuildSnapshot> snapshot;
        std::shared_ptr<PackBuildContext> buildContext;
        std::shared_ptr<PackBuildState> packBuildState;
        std::shared_ptr<const PackBuildPlan> buildPlan;
        PackMediaPreparation* mediaPreparation = nullptr;
        QMap<QString, std::shared_ptr<QLockFile>> platformLocks;
        QString currentWorldOutputPath;
        QString waveformOutputDir;
        QString language;
        bool    debugStrings = true;
        bool    deferBindings = false;
        bool    forceReexport = false;
        bool    selfCleaning = false;
        bool    updateSelectedOnly = false;
        bool    includeUnchecked = false;
        std::optional<PackResourceReuse> resourceReuse;

        QSet<QString> onlyUnits;
        QSet<QString> onlyPackPaths;
        std::set<harkenstudio::ObjectId> onlyActionStableIds;

        // Polled between units; returning true aborts the export with
        // ExportResult::canceled set.
        std::function<bool()> cancelCheck;
        std::function<void(const PackGenerationProgress&)> progressCallback;
        std::function<void(const PackBuildObservation&)> observationCallback;
    };

}

class ProjectPackExporter : public QObject
{
    Q_OBJECT

public:
    explicit ProjectPackExporter(QObject* parent = nullptr);
    ~ProjectPackExporter() override;

    static void beginBackgroundExport();
    static void endBackgroundExport();
    static bool waitForBackgroundExports(int timeoutMs);
    static bool hasBackgroundExportInFlight();

    using ExportOptions = ProjectPackExporterInternal::ProjectPackExporterExportOptions;

    static std::shared_ptr<PackBuildContext> createBuildContext(const ProjectData& project);

    static std::shared_ptr<const PackBuildPlan> createBuildPlan(const ProjectData& project,
                                                               const ExportOptions& options);

    struct ExportResult
    {
        bool    success = false;
        bool    canceled = false;
        QString outputPath;
        qint64  fileSize  = 0;
        int     objectCount = 0;
        int     mediaCount  = 0;
        int     actionCount  = 0;
        QString error;
        QStringList warnings;
        int     mediaCacheHits = 0;
        int     mediaConversions = 0;
        uint64_t currentWorldSemanticFingerprint = 0;
        std::map<harkenstudio::ObjectId, uint64_t> soundContentRevisions;
        QString resourceFingerprint;
        bool reusedResources = false;
        int builtPackCount = 0;
        int skippedPackCount = 0;
        int removedPackCount = 0;
        QVector<PackArtifactMetadata> artifacts;
        QJsonArray outputs;
    };

    struct PackBuildRecord
    {
        enum class Source { Group, Action, Init, Shared };
        QString  packName;
        uint32_t objectID = 0;
        Source   source = Source::Action;
        bool     success = false;
        QString  outputPath;
        QString  error;
        qint64   fileSize = 0;
        QString  platform;
        QStringList warnings;
        qint64   elapsedMs = -1;
        QString  language;
        QString  platformName;
        BasePlatform basePlatform = BasePlatform::Windows;
        QJsonObject platformDefinition;
        bool     skipped = false;
        QString  unitKey;
        bool     omitted = false;
    };

    ExportResult ExportAllPacks(const ProjectData& project,
                                 const QString& outputDir,
                                 const ExportOptions& options,
                                 std::vector<PackBuildRecord>* outRecords = nullptr);

    ExportResult ExportInitPack(const ProjectData& project,
                                 const QString& outputPath,
                                 const ExportOptions& options,
                                 std::vector<HarkenEditor::Bridge::PackBuildSession::CanonicalPackEntry>* resourceEntries = nullptr);

    ExportResult ExportActionPack(const ProjectData& project,
                                  const QString& actionName,
                                  const QString& outputPath,
                                  const ExportOptions& options,
                                  const std::set<uint32_t>* sharedMediaIds = nullptr,
                                  const std::set<uint32_t>* sharedObjectIds = nullptr,
                                  std::vector<HarkenEditor::Bridge::PackBuildSession::CanonicalPackEntry>* resourceEntries = nullptr);

    ExportResult ExportSharedPack(const ProjectData& project,
                                  const QString& outputPath,
                                  const ExportOptions& options,
                                  const std::set<uint32_t>& sharedMedia,
                                  const std::set<uint32_t>& sharedObjects);

    static QString platformOutputDir(const QString& baseDir,
                                     const ProjectPlatform& platform);

    static bool PreparePackOutputDirectory(const QString& baseDir,
                                           const ProjectPlatform& platform,
                                           QString* errorOut = nullptr);

    static ExportResult FinalizePackExport(const QString& baseDir,
                                           const ProjectData& project,
                                           const QStringList& selectedPlatformIds,
                                           bool selfCleaning,
                                           const QMap<QString, QJsonArray>& outputs,
                                           const std::function<bool()>& cancelCheck = {});

    static std::shared_ptr<QLockFile> LockPackOutputRoot(const QString& baseDir,
                                                       QString* errorOut = nullptr);

    static QStringList resolveExportPlatforms(const ProjectData& project,
                                              const QString& explicitPlatform,
                                              const PackExportSelection& selection,
                                              QString* errorOut = nullptr);

    int GenerateHarkenPeakFiles(const ProjectData& project,
                               const QString& outputDir);


    struct ValidationIssue
    {
        enum Severity { Error, Warning, Info };
        Severity severity;
        QString  message;
        uint32_t objectID = 0;
    };

    struct ValidationResult
    {
        bool valid = true;
        std::vector<ValidationIssue> issues;
        int errorCount   = 0;
        int warningCount = 0;
    };

    ValidationResult ValidateBeforeBuild(const ProjectData& project);

    static ValidationResult ValidatePackBuildReadiness(const ProjectData& project,
                                                       const QString& outputDir,
                                                       PackBuildContext* buildContext = nullptr);

    struct DependencyNode
    {
        uint32_t objectID = 0;
        QString  name;
        QString  type;
        std::vector<DependencyNode> children;
    };

    std::vector<DependencyNode> GetDependencyTree(const ProjectData& project);
    QJsonArray GetDependencyTreeJson(const ProjectData& project);

    struct UnusedAsset
    {
        uint32_t sourceID = 0;
        QString  name;
        QString  path;
    };

    std::vector<UnusedAsset> FindUnusedAssets(const ProjectData& project);
    QJsonArray FindUnusedAssetsJson(const ProjectData& project);

signals:
    void progressChanged(int percent, const QString& message);
    void unitProgressChanged(int done, int total, const QString& unitName);

public:
    static std::set<harkenstudio::ObjectId> CollectActionStableIds(const ProjectData& project);

    struct ActionPackUnit
    {
        harkenstudio::ObjectId stableId = {};
        uint32_t objectID = 0;
        QString  name;
    };
    static std::vector<ActionPackUnit> CollectActionPackUnits(const ProjectData& project);

    struct SharedOwnership
    {
        std::set<uint32_t> mediaIds;
        std::set<uint32_t> objectIds;
    };

private:
    friend ExportResult ExportCanonicalPackSet(
        ProjectPackExporter& exporter, const CanonicalPackSetExportRequest& request,
        std::vector<PackBuildRecord>* outRecords);



    struct MediaCollectionStats
    {
        std::map<harkenstudio::ObjectId, uint64_t> soundContentRevisions;
        int cacheHits = 0;
        int conversions = 0;
        QStringList warnings;
    };

    ExportResult exportSingleActionPack(
        const ProjectData& project,
        const ProjectItem& action,
        const QString& outputPath,
        const ExportOptions& options,
        const std::set<uint32_t>* sharedMediaIds = nullptr,
        const std::set<uint32_t>* sharedObjectIds = nullptr,
        std::vector<HarkenEditor::Bridge::PackBuildSession::CanonicalPackEntry>* resourceEntries = nullptr);

    void CollectContentInputsFromItem(const ProjectItem* item,
                                      HarkenEditor::Bridge::PackBuildSession& builder,
                                      std::set<uint32_t>& visitedObjects,
                                      std::set<uint32_t>& requiredMedia,
                                      const ProjectData& project,
                                      ProjectPlatformId platform,
                                      const QString& language,
                                      const std::set<uint32_t>* excludedObjectIds = nullptr);

    SharedOwnership computeSharedOwnership(const ProjectData& project,
                                           const PackBuildPlan& plan,
                                           const std::set<harkenstudio::ObjectId>& includedActionIds) const;

    bool CollectMedia(const ProjectData& project,
                       HarkenEditor::Bridge::PackBuildSession& builder,
                       const std::set<uint32_t>& requiredMedia,
                       const std::set<uint32_t>* includedObjectIds,
                       const ExportOptions& options,
                       MediaCollectionStats* stats = nullptr,
                       QString* errorOut = nullptr,
                       const ActionClosurePlan* closure = nullptr,
                       const std::set<uint32_t>* excludedClosureMediaIds = nullptr);

    bool CollectBuses(const ProjectData& project,
                      HarkenEditor::Bridge::PackBuildSession& builder,
                      PlatformScope scope, QString* errorOut);
    bool CollectSends(const ProjectData& project,
                      HarkenEditor::Bridge::PackBuildSession& builder,
                       PlatformScope scope, QString* errorOut);
    void CollectAttenuations(const ProjectData& project,
                                     HarkenEditor::Bridge::PackBuildSession& builder);
    bool CollectTypedBindings(const ProjectData& project,
                              HarkenEditor::Bridge::PackBuildSession& builder,
                              const std::set<harkenstudio::ObjectId>* allowedRuntimeObjectIds = nullptr,
                              QString* errorOut = nullptr,
                              PackBuildContext* buildContext = nullptr);
    void CollectReferencedMedia(const ProjectData& project,
                                 std::set<uint32_t>& referencedMedia);

    static QString ResolveAudioFilePath(const ProjectData& project,
                                        uint32_t sourceID,
                                        const QString& audioSourcesDir,
                                        const MediaBuildSnapshot* media = nullptr);

    static bool WriteHarkenPeakFile(const QString& outputPath,
                                   const struct WaveformPeakData& data);

    static uint32_t FNVHash(const QString& str);

    static void ApplyMediaCollectionStats(ExportResult& result,
                                          const MediaCollectionStats& stats);
};

}
