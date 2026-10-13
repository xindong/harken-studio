#include "ProjectMigration.h"

#include "ProjectDataPolicy.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

#include <algorithm>

namespace VNX {

ProjectMigrationWorkspace::ProjectMigrationWorkspace(
    QString sourceDirectory, QString stagingDirectory, QStringList writeSet,
    QString projectFile)
    : sourceDirectory_(QDir::cleanPath(std::move(sourceDirectory)))
    , stagingDirectory_(QDir::cleanPath(std::move(stagingDirectory)))
    , projectFile_(ProjectDataPolicy::normalizeRelativePath(projectFile))
{
    for (const QString& path : writeSet) {
        const QString normalized = normalize(path);
        if (!normalized.isEmpty())
            writeSet_.insert(normalized);
    }
}

QString ProjectMigrationWorkspace::normalize(const QString& relativePath) const
{
    const QString normalized = ProjectDataPolicy::normalizeRelativePath(relativePath);
    return ProjectDataPolicy::isSafeRelativePath(normalized) ? normalized : QString();
}

bool ProjectMigrationWorkspace::readObject(const QString& relativePath,
                                           QJsonObject* object,
                                           QString* error)
{
    const QString normalized = normalize(relativePath);
    if (normalized.isEmpty() || !object) {
        if (error) *error = QStringLiteral("PROJECT_MIGRATION_PATH_INVALID");
        return false;
    }
    if (objects_.contains(normalized)) {
        *object = objects_.value(normalized);
        return true;
    }
    const QString staged = QDir(stagingDirectory_).filePath(normalized);
    const QString source = QFileInfo::exists(staged)
        ? staged : QDir(sourceDirectory_).filePath(normalized);
    QFile file(source);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("PROJECT_MIGRATION_FILE_UNREADABLE");
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) *error = QStringLiteral("PROJECT_MIGRATION_FILE_INVALID");
        return false;
    }
    objects_.insert(normalized, document.object());
    parseCounts_[normalized] += 1;
    *object = document.object();
    return true;
}

bool ProjectMigrationWorkspace::writeObject(const QString& relativePath,
                                            const QJsonObject& object,
                                            QString* error)
{
    const QString normalized = normalize(relativePath);
    if (normalized.isEmpty() || !writeSet_.contains(normalized)) {
        if (error) *error = QStringLiteral("PROJECT_MIGRATION_WRITE_OUTSIDE_PLAN");
        return false;
    }
    objects_.insert(normalized, object);
    dirty_.insert(normalized);
    return true;
}

bool ProjectMigrationWorkspace::flush(QString* error)
{
    QStringList files = dirty_.values();
    files.sort();
    for (const QString& relative : files) {
        const QString path = QDir(stagingDirectory_).filePath(relative);
        if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
            if (error) *error = QStringLiteral("PROJECT_MIGRATION_STAGE_UNWRITABLE");
            return false;
        }
        QSaveFile file(path);
        const QByteArray bytes = QJsonDocument(objects_.value(relative)).toJson(
            QJsonDocument::Indented);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()
            || !file.commit()) {
            if (error) *error = QStringLiteral("PROJECT_MIGRATION_STAGE_UNWRITABLE");
            return false;
        }
        writeCounts_[relative] += 1;
    }
    dirty_.clear();
    return true;
}

int ProjectMigrationWorkspace::parseCount(const QString& relativePath) const
{
    return parseCounts_.value(ProjectDataPolicy::normalizeRelativePath(relativePath));
}

int ProjectMigrationWorkspace::writeCount(const QString& relativePath) const
{
    return writeCounts_.value(ProjectDataPolicy::normalizeRelativePath(relativePath));
}

QStringList ProjectMigrationWorkspace::dirtyFiles() const
{
    QStringList result = dirty_.values();
    result.sort();
    return result;
}

QString ProjectMigrationWorkspace::projectFile() const
{
    return projectFile_;
}

QString ProjectMigrationWorkspace::stagingDirectory() const
{
    return stagingDirectory_;
}

QString ProjectMigrationWorkspace::sourceDirectory() const
{
    return sourceDirectory_;
}

QStringList ProjectMigrationWorkspace::writeSet() const
{
    QStringList result = writeSet_.values();
    result.sort();
    return result;
}

const QHash<QString, QJsonObject>& ProjectMigrationWorkspace::loadedObjects() const
{
    return objects_;
}

bool ProjectMigrationRegistry::registerStep(ProjectMigrationStep step,
                                            QString* error)
{
    if (step.id.trimmed().isEmpty() || step.fromSchema < 1
        || step.toSchema != step.fromSchema + 1 || !step.transform) {
        if (error) *error = QStringLiteral("PROJECT_MIGRATION_STEP_INVALID");
        return false;
    }
    QStringList normalizedWriteSet;
    for (const QString& path : step.writeSet) {
        const QString normalized = ProjectDataPolicy::normalizeRelativePath(path);
        if (normalized != QStringLiteral("<project-file>")
            && normalized != QStringLiteral("<all-documents>")
            && !ProjectDataPolicy::isSafeRelativePath(normalized)) {
            if (error) *error = QStringLiteral("PROJECT_MIGRATION_WRITE_SET_INVALID");
            return false;
        }
        normalizedWriteSet.append(normalized);
    }
    normalizedWriteSet.sort();
    normalizedWriteSet.removeDuplicates();
    step.writeSet = normalizedWriteSet;
    for (const ProjectMigrationStep& registered : steps_) {
        if (registered.fromSchema == step.fromSchema
            && registered.toSchema == step.toSchema
            && registered.id == step.id) {
            if (error) *error = QStringLiteral("PROJECT_MIGRATION_STEP_DUPLICATE");
            return false;
        }
    }
    steps_.push_back(std::move(step));
    return true;
}

ProjectMigrationPlan ProjectMigrationRegistry::plan(
    int sourceSchema, int targetSchema, const QString& projectDirectory) const
{
    ProjectMigrationPlan result;
    result.sourceSchema = sourceSchema;
    result.targetSchema = targetSchema;
    if (sourceSchema < 1 || targetSchema < sourceSchema) {
        result.error = QStringLiteral("PROJECT_MIGRATION_RANGE_INVALID");
        return result;
    }
    QSet<QString> writeSet;
    for (int version = sourceSchema; version < targetSchema; ++version) {
        std::vector<ProjectMigrationStep> pending;
        for (const ProjectMigrationStep& step : steps_)
            if (step.fromSchema == version && step.toSchema == version + 1)
                pending.push_back(step);
        if (pending.empty()) {
            result.error = QStringLiteral("PROJECT_MIGRATION_MISSING");
            return result;
        }
        QSet<QString> available;
        for (const ProjectMigrationStep& step : pending)
            available.insert(step.id);
        for (const ProjectMigrationStep& step : pending) {
            for (const QString& dependency : step.dependencies) {
                if (!available.contains(dependency)) {
                    result.error = QStringLiteral("PROJECT_MIGRATION_DEPENDENCY_MISSING");
                    return result;
                }
            }
        }
        QSet<QString> completed;
        while (!pending.empty()) {
            auto selected = std::find_if(
                pending.begin(), pending.end(), [&completed](const auto& step) {
                    for (const QString& dependency : step.dependencies)
                        if (!completed.contains(dependency))
                            return false;
                    return true;
                });
            if (selected == pending.end()) {
                result.error = QStringLiteral("PROJECT_MIGRATION_DEPENDENCY_INVALID");
                return result;
            }
            for (const QString& path : selected->writeSet)
                writeSet.insert(path);
            completed.insert(selected->id);
            result.steps.push_back(*selected);
            pending.erase(selected);
        }
    }
    result.writeSet = writeSet.values();
    result.writeSet.sort();
    result.fileCount = result.writeSet.size();
    result.stepCount = static_cast<int>(result.steps.size());
    if (!projectDirectory.isEmpty()) {
        for (const QString& relative : result.writeSet) {
            const QFileInfo info(QDir(projectDirectory).filePath(relative));
            if (info.exists() && info.isFile())
                result.recoveryBytes += info.size();
        }
    }
    result.ok = true;
    return result;
}

bool ProjectMigrationRegistry::execute(const ProjectMigrationPlan& plan,
                                       ProjectMigrationWorkspace& workspace,
                                       QStringList* warnings,
                                       QString* error,
                                       ProjectMigrationProgressCallback progress,
                                       ProjectMigrationVersionValidator validator) const
{
    if (!plan.ok) {
        if (error) *error = plan.error;
        return false;
    }
    ProjectMigrationProgress state;
    state.totalSteps = plan.stepCount;
    state.totalFiles = plan.fileCount;
    state.totalBytes = plan.recoveryBytes;
    if (progress)
        progress(state);
    for (size_t index = 0; index < plan.steps.size();) {
        const int sourceVersion = plan.steps[index].fromSchema;
        const int targetVersion = plan.steps[index].toSchema;
        if (validator && !validator(workspace, sourceVersion, error))
            return false;
        while (index < plan.steps.size()
               && plan.steps[index].fromSchema == sourceVersion
               && plan.steps[index].toSchema == targetVersion) {
            const ProjectMigrationStep& step = plan.steps[index];
            if (!step.transform(workspace, warnings, error))
                return false;
            state.completedSteps += 1;
            state.stepId = step.id;
            if (progress)
                progress(state);
            ++index;
        }
        if (validator && !validator(workspace, targetVersion, error))
            return false;
    }
    if (!workspace.flush(error))
        return false;
    state.completedFiles = state.totalFiles;
    state.completedBytes = state.totalBytes;
    state.relativePath.clear();
    if (progress)
        progress(state);
    return true;
}

}
