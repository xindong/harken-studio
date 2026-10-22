#include "ProjectUpgradeManager.h"

#include "MigrationManager.h"
#include "ProjectDataPolicy.h"
#include "ProjectLocator.h"
#include "ProjectPreflight/ProjectPreflight.h"
#include "ProjectTransaction.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QUuid>

#include <limits>

namespace VNX {

ProjectUpgradeManager::ProjectUpgradeManager(MigrationManager* migrationManager)
    : migrationManager_(migrationManager ? migrationManager : &MigrationManager::instance())
{
}

namespace {

QString projectFileFor(const QString& input)
{
    const ProjectLocation location = ProjectLocator::locate(input);
    return location.ok() ? location.projectFilePath : QString();
}

bool copyFile(const QString& source, const QString& destination)
{
    if (!QDir().mkpath(QFileInfo(destination).absolutePath()))
        return false;
    QFile::remove(destination);
    return QFile::copy(source, destination);
}

struct FileSetMetrics
{
    int files = 0;
    qint64 bytes = 0;
};

qint64 allocatedBytes(qint64 size)
{
    constexpr qint64 block = 4096;
    return size <= 0 ? block : ((size + block - 1) / block) * block;
}

FileSetMetrics protectedTreeMetrics(const QString& projectDirectory,
                                    const ProjectDataPolicy& dataPolicy)
{
    FileSetMetrics result;
    QDirIterator iterator(projectDirectory, QDir::Files | QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    const QDir root(projectDirectory);
    while (iterator.hasNext()) {
        const QFileInfo entry(iterator.next());
        const QString relative = ProjectDataPolicy::normalizeRelativePath(
            root.relativeFilePath(entry.absoluteFilePath()));
        if (!dataPolicy.protectByDefault(relative))
            continue;
        ++result.files;
        result.bytes += allocatedBytes(entry.size());
    }
    return result;
}

FileSetMetrics writeSetMetrics(const QString& projectDirectory,
                               const QStringList& writeSet)
{
    FileSetMetrics result;
    for (const QString& relative : writeSet) {
        const QFileInfo entry(QDir(projectDirectory).filePath(relative));
        if (!entry.isFile())
            continue;
        ++result.files;
        result.bytes += allocatedBytes(entry.size());
    }
    return result;
}

QStringList nonWritableUpgradeFiles(const ProjectUpgradeAnalysis& analysis)
{
    QStringList paths;
    for (const QString& relative : analysis.migrationPlan.writeSet) {
        const QFileInfo entry(QDir(analysis.projectDirectory).filePath(relative));
        if (!entry.isFile())
            continue;
        QFile file(entry.absoluteFilePath());
        if (!entry.isWritable() || !file.open(QIODevice::ReadWrite | QIODevice::ExistingOnly))
            paths.append(QDir::toNativeSeparators(entry.absoluteFilePath()));
    }
    return paths;
}

bool copyTree(const QString& sourceDirectory, const QString& destinationDirectory,
              const ProjectDataPolicy& dataPolicy,
              const FileSetMetrics& total,
              ProjectUpgradeProgressCallback progress)
{
    if (!QDir().mkpath(destinationDirectory))
        return false;
    int completedFiles = 0;
    qint64 completedBytes = 0;
    QDirIterator iterator(sourceDirectory,
                          QDir::AllEntries | QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    const QDir root(sourceDirectory);
    while (iterator.hasNext()) {
        const QFileInfo entry(iterator.next());
        if (entry.fileName() == QStringLiteral(".")
            || entry.fileName() == QStringLiteral(".."))
            continue;
        const QString relative = ProjectDataPolicy::normalizeRelativePath(
            root.relativeFilePath(entry.absoluteFilePath()));
        if (!dataPolicy.protectByDefault(relative))
            continue;
        if (entry.isSymLink())
            return false;
        const QString destination = QDir(destinationDirectory).filePath(relative);
        if (entry.isDir()) {
            if (!QDir().mkpath(destination))
                return false;
        } else if (!copyFile(entry.absoluteFilePath(), destination)) {
            return false;
        } else {
            ++completedFiles;
            completedBytes += allocatedBytes(entry.size());
            if (progress) {
                progress({ProjectUpgradePhase::CopyingStaging,
                          completedFiles, total.files,
                          completedBytes, total.bytes, relative});
            }
        }
    }
    return true;
}

bool readJson(const QString& path, QJsonObject* object, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("Cannot read %1: %2").arg(path, file.errorString());
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error)
            *error = QStringLiteral("Invalid JSON in %1: %2")
                         .arg(path, parseError.errorString());
        return false;
    }
    *object = document.object();
    return true;
}

bool hasRequiredSpace(const ProjectUpgradeAnalysis& analysis)
{
    const ProjectVolumeInfo project = ProjectVolume::classify(
        analysis.projectFilePath);
    if (project.kind == ProjectVolumeKind::Remote
        || (project.availableBytes >= 0
            && project.availableBytes < analysis.requiredProjectBytes))
        return false;
    if (analysis.requiredRecoveryBytes <= 0)
        return true;
    const QString recoveryRoot = QStandardPaths::writableLocation(
        QStandardPaths::AppLocalDataLocation);
    const ProjectVolumeInfo recovery = ProjectVolume::classify(recoveryRoot);
    return recovery.kind == ProjectVolumeKind::CertifiedInternal
        && (recovery.availableBytes < 0
            || recovery.availableBytes >= analysis.requiredRecoveryBytes);
}

bool hasCommitSpace(const ProjectUpgradeAnalysis& analysis,
                    const QString& stagingDirectory)
{
    const FileSetMetrics stagedWriteSet = writeSetMetrics(
        stagingDirectory, analysis.migrationPlan.writeSet);
    const ProjectUpgradeStorageRequirements required =
        ProjectUpgradeManager::storageRequirements(
            0, analysis.transactionBytes, stagedWriteSet.bytes,
            analysis.stagingFileCount + analysis.transactionFileCount,
            analysis.protection);
    const ProjectVolumeInfo project = ProjectVolume::classify(
        analysis.projectFilePath);
    if (project.availableBytes >= 0
        && project.availableBytes < required.projectBytes)
        return false;
    if (required.recoveryBytes <= 0)
        return true;
    const ProjectVolumeInfo recovery = ProjectVolume::classify(
        analysis.recoveryRoot);
    return recovery.kind == ProjectVolumeKind::CertifiedInternal
        && (recovery.availableBytes < 0
            || recovery.availableBytes >= required.recoveryBytes);
}

QByteArray fileFingerprint(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

bool isIteratorDotEntry(const QString& relativePath)
{
    return relativePath == QStringLiteral(".")
        || relativePath == QStringLiteral("..")
        || relativePath.endsWith(QStringLiteral("/."))
        || relativePath.endsWith(QStringLiteral("/.."));
}

QByteArray treeFingerprint(const QString& directory, const ProjectDataPolicy& dataPolicy)
{
    QStringList entries;
    QDirIterator iterator(directory,
                          QDir::AllEntries | QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    const QDir root(directory);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QString relative = QDir::fromNativeSeparators(
            root.relativeFilePath(path));
        const QFileInfo info(path);
        if (isIteratorDotEntry(relative)
            || !dataPolicy.includeInContentFingerprint(relative, info.isDir()))
            continue;
        if (info.isSymLink()) return {};
        entries.append((info.isDir() ? QStringLiteral("D:") : QStringLiteral("F:"))
                       + relative);
    }
    entries.sort();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QByteArray separator(1, '\0');
    for (const QString& entry : entries) {
        const QString relative = entry.mid(2);
        hash.addData(entry.toUtf8());
        hash.addData(separator);
        if (entry.startsWith(QStringLiteral("F:"))) {
            QFile file(root.filePath(relative));
            if (!file.open(QIODevice::ReadOnly)) return {};
            hash.addData(QCryptographicHash::hash(file.readAll(),
                                                  QCryptographicHash::Sha256));
        }
        hash.addData(separator);
    }
    return hash.result();
}

QByteArray protectedTreeFingerprint(const QString& directory,
                                   const ProjectDataPolicy& dataPolicy)
{
    QStringList entries;
    QDirIterator iterator(directory,
                          QDir::AllEntries | QDir::Hidden | QDir::System,
                          QDirIterator::Subdirectories);
    const QDir root(directory);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        const QString relative = QDir::fromNativeSeparators(
            root.relativeFilePath(path));
        const QFileInfo info(path);
        if (isIteratorDotEntry(relative)
            || !dataPolicy.includeInContentFingerprint(relative, info.isDir()))
            continue;
        if (info.isSymLink())
            return {};
        entries.append((info.isDir() ? QStringLiteral("D:")
                                     : QStringLiteral("F:"))
                       + relative);
    }
    entries.sort();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QByteArray separator(1, '\0');
    for (const QString& entry : entries) {
        hash.addData(entry.toUtf8());
        hash.addData(separator);
        if (entry.startsWith(QStringLiteral("F:"))) {
            QFile file(root.filePath(entry.mid(2)));
            if (!file.open(QIODevice::ReadOnly))
                return {};
            hash.addData(QCryptographicHash::hash(file.readAll(),
                                                  QCryptographicHash::Sha256));
        }
        hash.addData(separator);
    }
    return hash.result();
}

QString receiptPathFor(const QString& projectDirectory)
{
    return projectDirectory + QStringLiteral(".harken-upgrade-receipt.json");
}

}

ProjectUpgradeStorageRequirements ProjectUpgradeManager::storageRequirements(
    qint64 stagingBytes, qint64 transactionSourceBytes,
    qint64 transactionTargetBytes, int fileCount,
    ProjectUpgradeProtection protection)
{
    const auto add = [](qint64 first, qint64 second) {
        if (first > std::numeric_limits<qint64>::max() - second)
            return std::numeric_limits<qint64>::max();
        return first + second;
    };
    qint64 payload = add(stagingBytes,
                         add(transactionSourceBytes, transactionTargetBytes));
    const qint64 percentage = payload / 10;
    const qint64 perFile = qMax<qint64>(0, fileCount) * 8192;
    const qint64 safety = qMax<qint64>(8 * 1024 * 1024,
                                      add(percentage, perFile));
    ProjectUpgradeStorageRequirements result;
    result.safetyBytes = safety;
    if (protection == ProjectUpgradeProtection::AtomicInPlace) {
        result.projectBytes = add(payload, safety);
    } else {
        result.projectBytes = add(add(stagingBytes, transactionTargetBytes), safety);
        result.recoveryBytes = add(
            add(transactionSourceBytes, transactionTargetBytes), safety);
    }
    return result;
}

ProjectUpgradeAnalysis ProjectUpgradeManager::analyze(
    const QString& inputProjectFilePath,
    ProjectUpgradeProgressCallback progress) const
{
    ProjectUpgradeAnalysis analysis;
    analysis.projectFilePath = QDir::cleanPath(
        QFileInfo(inputProjectFilePath).absoluteFilePath());
    if (progress)
        progress({ProjectUpgradePhase::Analyzing, 0, 1, 0, 0, {}});
    const auto finishProgress = qScopeGuard([&progress]() {
        if (progress)
            progress({ProjectUpgradePhase::Analyzing, 1, 1, 0, 0, {}});
    });
    const ProjectLocation location = ProjectLocator::locate(inputProjectFilePath);
    if (!location.ok()) {
        analysis.issue = location.issue;
        analysis.detail = location.candidates.join(QStringLiteral("\n"));
        analysis.blockingErrors.append(projectIssueCode(analysis.issue) + QChar(':')
                                       + analysis.detail);
        return analysis;
    }
    analysis.projectFilePath = location.projectFilePath;
    analysis.projectDirectory = QFileInfo(analysis.projectFilePath).absolutePath();
    analysis.targetSchema = targetSchemaForTesting_ > 0
        ? targetSchemaForTesting_ : MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION;
    analysis.sourceFingerprint = fileFingerprint(analysis.projectFilePath);
    if (analysis.sourceFingerprint.isEmpty()) {
        analysis.issue = ProjectIssue::ProjectFileDamaged;
        analysis.detail = QStringLiteral("Project file cannot be fingerprinted");
        analysis.blockingErrors.append(analysis.detail);
        return analysis;
    }
    analysis.dataPolicy = ProjectDataPolicy::forProjectFile(analysis.projectFilePath);
    analysis.sourceTreeFingerprint = treeFingerprint(analysis.projectDirectory,
                                                     analysis.dataPolicy);
    if (analysis.sourceTreeFingerprint.isEmpty()) {
        analysis.issue = ProjectIssue::ProjectFileDamaged;
        analysis.detail = QStringLiteral("Project tree cannot be fingerprinted");
        analysis.blockingErrors.append(analysis.detail);
        return analysis;
    }
    analysis.probe = ProjectContractRegistry::instance().probe(analysis.projectFilePath);
    analysis.sourceSchema = analysis.probe.schemaVersion;
    analysis.warnings = analysis.probe.warnings;
    analysis.blockingErrors = analysis.probe.blockingErrors;
    if (!analysis.blockingErrors.isEmpty()) {
        analysis.issue = analysis.probe.issue == ProjectIssue::None
            ? ProjectIssue::ProjectFileDamaged : analysis.probe.issue;
        analysis.detail = analysis.probe.detail;
        return analysis;
    }
    if (analysis.sourceSchema < 1 || analysis.sourceSchema > analysis.targetSchema) {
        analysis.issue = analysis.sourceSchema > analysis.targetSchema
            ? ProjectIssue::ProjectRequiresNewerStudio
            : ProjectIssue::ProjectSchemaStateMismatch;
        analysis.detail = ProjectPreflight::schemaMismatchError(
            analysis.sourceSchema, analysis.targetSchema);
        analysis.blockingErrors.append(projectIssueCode(analysis.issue) + QChar(':')
                                       + analysis.detail);
        return analysis;
    }

    QJsonObject root;
    QString rootError;
    if (!readJson(analysis.projectFilePath, &root, &rootError)) {
        analysis.issue = ProjectIssue::ProjectFileDamaged;
        analysis.detail = rootError;
        analysis.blockingErrors.append(rootError);
        return analysis;
    }
    analysis.upgradeRequired = analysis.sourceSchema < analysis.targetSchema;
    if (analysis.sourceSchema < analysis.targetSchema) {
        analysis.migrationPlan = migrationManager_->planProject(
            analysis.projectDirectory, analysis.projectFilePath,
            analysis.sourceSchema, analysis.targetSchema);
        if (!analysis.migrationPlan.ok) {
            analysis.issue = ProjectIssue::MigrationMissing;
            analysis.detail = analysis.migrationPlan.error;
            analysis.blockingErrors.append(projectIssueCode(analysis.issue));
            return analysis;
        }
        const QStringList nonWritableFiles = nonWritableUpgradeFiles(analysis);
        if (!nonWritableFiles.isEmpty()) {
            analysis.issue = ProjectIssue::ProjectUpgradeFilesNotWritable;
            analysis.detail = nonWritableFiles.join(QLatin1Char('\n'));
            analysis.blockingErrors = nonWritableFiles;
            return analysis;
        }
        analysis.projectVolume = ProjectVolume::classify(analysis.projectFilePath);
        if (analysis.projectVolume.kind == ProjectVolumeKind::Remote) {
            analysis.issue = ProjectIssue::ProjectVolumeUnsupported;
            analysis.detail = projectIssueCode(analysis.issue);
            analysis.blockingErrors.append(analysis.detail);
            return analysis;
        }
        const FileSetMetrics staging = protectedTreeMetrics(
            analysis.projectDirectory, analysis.dataPolicy);
        const FileSetMetrics transaction = writeSetMetrics(
            analysis.projectDirectory, analysis.migrationPlan.writeSet);
        analysis.stagingBytes = staging.bytes;
        analysis.stagingFileCount = staging.files;
        analysis.transactionBytes = transaction.bytes;
        analysis.transactionFileCount = transaction.files;
        if (analysis.projectVolume.kind == ProjectVolumeKind::CertifiedInternal) {
            analysis.protection = ProjectUpgradeProtection::AtomicInPlace;
            analysis.recoveryVolume = analysis.projectVolume;
        } else {
            analysis.protection = ProjectUpgradeProtection::RecoverableInPlace;
            analysis.recoveryRoot = QDir(QStandardPaths::writableLocation(
                QStandardPaths::AppLocalDataLocation))
                .filePath(QStringLiteral("ProjectRecovery"));
            analysis.recoveryVolume = ProjectVolume::classify(
                QStandardPaths::writableLocation(
                    QStandardPaths::AppLocalDataLocation));
            if (analysis.recoveryVolume.kind != ProjectVolumeKind::CertifiedInternal) {
                analysis.issue = ProjectIssue::ProjectRecoveryVolumeUnavailable;
                analysis.detail = projectIssueCode(analysis.issue);
                analysis.blockingErrors.append(analysis.detail);
                return analysis;
            }
        }
        const ProjectUpgradeStorageRequirements required = storageRequirements(
            analysis.stagingBytes, analysis.transactionBytes,
            analysis.transactionBytes,
            analysis.stagingFileCount + analysis.transactionFileCount,
            analysis.protection);
        analysis.requiredProjectBytes = required.projectBytes;
        analysis.requiredRecoveryBytes = required.recoveryBytes;
        const bool projectSpaceMissing = analysis.projectVolume.availableBytes >= 0
            && analysis.projectVolume.availableBytes < analysis.requiredProjectBytes;
        const bool recoverySpaceMissing = analysis.requiredRecoveryBytes > 0
            && analysis.recoveryVolume.availableBytes >= 0
            && analysis.recoveryVolume.availableBytes
                   < analysis.requiredRecoveryBytes;
        if (projectSpaceMissing || recoverySpaceMissing) {
            analysis.issue = ProjectIssue::ProjectUpgradeSpaceInsufficient;
            analysis.detail = projectIssueCode(analysis.issue);
            analysis.blockingErrors.append(analysis.detail);
            return analysis;
        }
    }
    if (!analysis.upgradeRequired) {
        analysis.blockingErrors = treeValidatorForTesting_
            ? treeValidatorForTesting_(analysis.projectDirectory, root, true)
            : ProjectContractRegistry::instance().validateTree(
                analysis.projectDirectory, root, true);
    }
    analysis.ok = analysis.blockingErrors.isEmpty();
    if (!analysis.ok) {
        analysis.issue = ProjectIssue::ProjectFileDamaged;
        analysis.detail = analysis.blockingErrors.join(QLatin1Char('\n'));
    }
    return analysis;
}

ProjectUpgradeAnalysis ProjectUpgradeManager::analyzeForProjectOpen(
    const QString& projectFilePath,
    ProjectUpgradeProgressCallback progress) const
{
    if (!ProjectTransaction::recoveryRequired(projectFilePath))
        return analyze(projectFilePath, std::move(progress));

    ProjectUpgradeAnalysis analysis;
    analysis.projectFilePath = QDir::cleanPath(
        QFileInfo(projectFilePath).absoluteFilePath());
    analysis.projectDirectory = QFileInfo(analysis.projectFilePath).absolutePath();
    analysis.targetSchema = targetSchemaForTesting_ > 0
        ? targetSchemaForTesting_ : MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION;
    analysis.issue = ProjectIssue::ProjectUpgradeRecoveryRequired;
    analysis.detail = projectIssueCode(analysis.issue);
    analysis.blockingErrors.append(analysis.detail);
    return analysis;
}

ProjectUpgradeResult ProjectUpgradeManager::execute(
    const ProjectUpgradeAnalysis& analysis,
    StagedProjectValidator validator,
    ProjectUpgradeProgressCallback progress) const
{
    ProjectUpgradeResult result;
    result.projectFilePath = analysis.projectFilePath;
    result.warnings = analysis.warnings;
    if (!analysis.ok) {
        result.issue = analysis.issue == ProjectIssue::None
            ? ProjectIssue::InternalFailure : analysis.issue;
        result.detail = analysis.detail;
        result.error = projectIssueCode(result.issue) + QChar(':') + result.detail;
        return result;
    }
    if (fileFingerprint(analysis.projectFilePath) != analysis.sourceFingerprint
        || treeFingerprint(analysis.projectDirectory, analysis.dataPolicy)
               != analysis.sourceTreeFingerprint) {
        result.issue = ProjectIssue::ProjectChangedDuringUpgrade;
        result.error = projectIssueCode(result.issue);
        return result;
    }
    if (analysis.upgradeRequired && !hasRequiredSpace(analysis)) {
        result.issue = ProjectIssue::ProjectUpgradeSpaceInsufficient;
        result.error = projectIssueCode(result.issue);
        return result;
    }
    if (!analysis.upgradeRequired) {
        result.ok = true;
        if (progress)
            progress({ProjectUpgradePhase::Completed, 1, 1, 0, 0, {}});
        return result;
    }

    const auto rejectNonWritableFiles = [&]() {
        result.detail = nonWritableUpgradeFiles(analysis).join(QLatin1Char('\n'));
        if (result.detail.isEmpty())
            return false;
        result.issue = ProjectIssue::ProjectUpgradeFilesNotWritable;
        result.error = projectIssueCode(result.issue) + QLatin1Char(':') + result.detail;
        return true;
    };
    if (rejectNonWritableFiles())
        return result;

    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString stagingDirectory = analysis.projectDirectory +
                                     QStringLiteral(".harken-upgrade-staging-") + token;
    const FileSetMetrics stagingMetrics{
        analysis.stagingFileCount, analysis.stagingBytes};
    if (progress)
        progress({ProjectUpgradePhase::CopyingStaging, 0,
                  stagingMetrics.files, 0, stagingMetrics.bytes, {}});
    if (!copyTree(analysis.projectDirectory, stagingDirectory,
                  analysis.dataPolicy,
                  stagingMetrics, progress)) {
        result.error = QStringLiteral("Cannot create upgrade staging directory");
        QDir(stagingDirectory).removeRecursively();
        return result;
    }
    if (protectedTreeFingerprint(stagingDirectory, analysis.dataPolicy)
        != protectedTreeFingerprint(analysis.projectDirectory, analysis.dataPolicy)) {
        result.error = QStringLiteral("Upgrade staging copy failed verification");
        QDir(stagingDirectory).removeRecursively();
        return result;
    }

    const QString stagedProjectFile = QDir(stagingDirectory).filePath(
        QDir(analysis.projectDirectory).relativeFilePath(analysis.projectFilePath));
    QJsonObject stagedRoot;
    QString error;
    const auto migrationProgress = [&progress](
        const ProjectMigrationProgress& value) {
        if (!progress)
            return;
        progress({ProjectUpgradePhase::Migrating,
                  value.completedSteps, value.totalSteps,
                  value.completedBytes, value.totalBytes,
                  value.stepId});
    };
    if (!migrationManager_->migrateProject(
            analysis.migrationPlan, analysis.projectDirectory, stagingDirectory,
            analysis.projectFilePath, &result.warnings, &error,
            migrationProgress)
        || !readJson(stagedProjectFile, &stagedRoot, &error)) {
        result.error = error;
        QDir(stagingDirectory).removeRecursively();
        return result;
    }
    if (progress)
        progress({ProjectUpgradePhase::ValidatingStaging, 0, 1,
                  0, analysis.stagingBytes, {}});
    const QStringList stagedErrors = treeValidatorForTesting_
        ? treeValidatorForTesting_(stagingDirectory, stagedRoot, true)
        : ProjectContractRegistry::instance().validateTree(
            stagingDirectory, stagedRoot, true);
    if (!stagedErrors.isEmpty()) {
        result.error = stagedErrors.first();
        QDir(stagingDirectory).removeRecursively();
        return result;
    }
    if (progress)
        progress({ProjectUpgradePhase::ValidatingStaging, 1, 1,
                  analysis.stagingBytes, analysis.stagingBytes, {}});

    if (validator) {
        QString validationError;
        if (!validator(stagedProjectFile, &validationError)) {
            result.error = validationError.isEmpty()
                               ? QStringLiteral("Staged project could not be loaded")
                               : validationError;
            QDir(stagingDirectory).removeRecursively();
            return result;
        }
    }

    if (treeFingerprint(analysis.projectDirectory, analysis.dataPolicy)
        != analysis.sourceTreeFingerprint) {
        result.error = QStringLiteral("Project changed while staging; upgrade was cancelled");
        QDir(stagingDirectory).removeRecursively();
        return result;
    }

    if (!hasCommitSpace(analysis, stagingDirectory)) {
        result.issue = ProjectIssue::ProjectUpgradeSpaceInsufficient;
        result.error = projectIssueCode(result.issue);
        QDir(stagingDirectory).removeRecursively();
        return result;
    }
    if (rejectNonWritableFiles()) {
        QDir(stagingDirectory).removeRecursively();
        return result;
    }

    const auto transactionProgress = [&progress](ProjectUpgradePhase phase) {
        return [&progress, phase](int completedFiles, int totalFiles,
                                  qint64 completedBytes, qint64 totalBytes) {
            if (progress) {
                progress({phase, completedFiles, totalFiles,
                          completedBytes, totalBytes, {}});
            }
        };
    };
    if (progress)
        progress({ProjectUpgradePhase::BackingUp, 0,
                  analysis.transactionFileCount, 0,
                  analysis.transactionBytes, {}});
    std::unique_ptr<ProjectTransaction> transaction = ProjectTransaction::begin(
        analysis.projectFilePath, analysis.migrationPlan.writeSet,
        analysis.recoveryRoot, &error,
        transactionProgress(ProjectUpgradePhase::BackingUp));
    if (!transaction) {
        result.error = error;
        QDir(stagingDirectory).removeRecursively();
        return result;
    }
    const auto failWithRollback = [&](const QString& failure) {
        QString rollbackError;
        if (!transaction->rollback(
                &rollbackError,
                transactionProgress(ProjectUpgradePhase::RollingBack))) {
            result.issue = ProjectIssue::ProjectUpgradeRecoveryRequired;
            result.detail = rollbackError;
            result.error = projectIssueCode(result.issue) + QChar(':')
                         + failure + QStringLiteral("; ") + rollbackError;
        } else {
            result.error = failure;
        }
        QDir(stagingDirectory).removeRecursively();
        return result;
    };
    if (treeFingerprint(analysis.projectDirectory, analysis.dataPolicy)
        != analysis.sourceTreeFingerprint)
        return failWithRollback(QStringLiteral("Project changed before upgrade commit"));
    if (!transaction->replaceFromDirectory(
            stagingDirectory, &error,
            transactionProgress(ProjectUpgradePhase::Replacing))) {
        return failWithRollback(error);
    }
    QJsonObject committedRoot;
    if (!readJson(analysis.projectFilePath, &committedRoot, &error)) {
        return failWithRollback(error);
    }
    if (progress)
        progress({ProjectUpgradePhase::ValidatingProject, 0, 1,
                  0, analysis.stagingBytes, {}});
    const QStringList committedErrors = treeValidatorForTesting_
        ? treeValidatorForTesting_(analysis.projectDirectory, committedRoot, true)
        : ProjectContractRegistry::instance().validateTree(
            analysis.projectDirectory, committedRoot, true);
    if (!committedErrors.isEmpty()) {
        return failWithRollback(committedErrors.first());
    }
    if (progress)
        progress({ProjectUpgradePhase::ValidatingProject, 1, 1,
                  analysis.stagingBytes, analysis.stagingBytes, {}});
    if (progress)
        progress({ProjectUpgradePhase::Committing, 0, 1, 0, 0, {}});
    if (!transaction->commit(true, &error)) {
        return failWithRollback(error);
    }
    if (progress)
        progress({ProjectUpgradePhase::Committing, 1, 1, 0, 0, {}});
    QDir(stagingDirectory).removeRecursively();
    result.ok = true;
    result.changed = true;
    result.backupPath = transaction->backupDirectory();
    result.transactionPath = transaction->transactionPath();
    result.backupFingerprint = transaction->backupFingerprint();
    result.committedFingerprint = transaction->committedFingerprint();
    if (progress)
        progress({ProjectUpgradePhase::Completed, 1, 1, 0, 0, {}});
    return result;
}

bool ProjectUpgradeManager::finalize(ProjectUpgradeResult& result, QString* error) const
{
    if (!result.ok || !result.changed || result.transactionPath.isEmpty())
        return true;
    const QString projectDirectory = QFileInfo(result.projectFilePath).absolutePath();
    const QString receiptPath = receiptPathFor(projectDirectory);
    if (QFileInfo::exists(receiptPath)
        && !ProjectTransaction::discardReceipt(receiptPath, error))
        return false;
    if (!ProjectTransaction::finalizeJournal(result.transactionPath,
                                              receiptPath, error))
        return false;
    result.transactionPath = receiptPath;
    return true;
}

ProjectUpgradeResult ProjectUpgradeManager::readReceipt(
    const QString& inputProjectFilePath, QString* error) const
{
    ProjectUpgradeResult result;
    result.projectFilePath = projectFileFor(inputProjectFilePath);
    if (result.projectFilePath.isEmpty()) {
        if (error) *error = QStringLiteral("No .hsp project file found");
        return result;
    }
    const QString receiptPath = receiptPathFor(
        QFileInfo(result.projectFilePath).absolutePath());
    QFile file(receiptPath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("No upgrade backup is available");
        return result;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("Upgrade receipt is invalid");
        return result;
    }
    const QJsonObject receipt = document.object();
    if (receipt.value(QStringLiteral("state")).toString()
            != QStringLiteral("committed")
        || QDir::cleanPath(receipt.value(QStringLiteral("projectDirectory"))
                               .toString())
               != QFileInfo(result.projectFilePath).absolutePath()) {
        if (error) *error = QStringLiteral("Upgrade receipt targets another project");
        return result;
    }
    result.backupPath = receipt.value(QStringLiteral("backupDirectory")).toString();
    result.backupFingerprint = QByteArray::fromHex(
        receipt.value(QStringLiteral("backupFingerprint")).toString().toLatin1());
    result.committedFingerprint = QByteArray::fromHex(
        receipt.value(QStringLiteral("committedFingerprint")).toString().toLatin1());
    result.transactionPath = receiptPath;
    result.ok = result.changed = !result.backupPath.isEmpty()
                              && !result.backupFingerprint.isEmpty();
    if (!result.ok && error)
        *error = QStringLiteral("Upgrade receipt is incomplete");
    return result;
}

bool ProjectUpgradeManager::discardReceipt(const QString& inputProjectFilePath,
                                            QString* error) const
{
    const QString projectFilePath = projectFileFor(inputProjectFilePath);
    if (projectFilePath.isEmpty()) {
        if (error) *error = QStringLiteral("No .hsp project file found");
        return false;
    }
    return ProjectTransaction::discardReceipt(
        receiptPathFor(QFileInfo(projectFilePath).absolutePath()), error);
}

bool ProjectUpgradeManager::rollbackLastUpgrade(const QString& inputProjectFilePath,
                                                QString* error) const
{
    ProjectUpgradeResult result = readReceipt(inputProjectFilePath, error);
    if (!result.ok)
        return false;
    return rollback(result, error);
}

bool ProjectUpgradeManager::recover(const QString& inputProjectFilePath,
                                    QString* error) const
{
    const QString projectFilePath = projectFileFor(inputProjectFilePath);
    if (projectFilePath.isEmpty()) {
        if (error)
            *error = QStringLiteral("No .hsp project file found");
        return false;
    }
    return ProjectTransaction::recover(projectFilePath, error);
}

bool ProjectUpgradeManager::rollback(const ProjectUpgradeResult& result,
                                     QString* error) const
{
    if (!result.changed || result.transactionPath.isEmpty())
        return true;
    return ProjectTransaction::rollbackJournal(result.transactionPath, error);
}

}
