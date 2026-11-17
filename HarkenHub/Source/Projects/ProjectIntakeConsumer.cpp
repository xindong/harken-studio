#include "Projects/ProjectIntakeConsumer.h"

#include "Platform/PlatformPaths.h"
#include "Projects/ProjectProbe.h"
#include "Projects/ProjectStore.h"

#include <QDateTime>
#include <QCryptographicHash>
#include <QSaveFile>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

namespace VNH {

namespace {

constexpr qint64 kMaxRecordBytes = 16 * 1024;

QString creationBuildPath(const QString& projectPath)
{
    const QString absolute = QFileInfo(projectPath).absoluteFilePath();
    QString ancestor = QFileInfo(absolute).absolutePath();
    while (!QDir(ancestor).exists()) {
        const auto next = QFileInfo(ancestor).absolutePath();
        if (next == ancestor) break;
        ancestor = next;
    }
    const QDir parent(ancestor);
    QString normalized = QDir(parent.canonicalPath()).absoluteFilePath(parent.relativeFilePath(absolute));
#ifdef Q_OS_WIN
    normalized = normalized.toCaseFolded();
#endif
    return userDataDir() + QStringLiteral("/project-creation/")
        + QString::fromLatin1(QCryptographicHash::hash(normalized.toUtf8(), QCryptographicHash::Sha256).toHex())
        + QStringLiteral(".json");
}

bool isSafeBuildId(const QString& buildId)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
    return pattern.match(buildId).hasMatch();
}

}

QString ProjectIntakeConsumer::intakeDir()
{
    return userDataDir() + QStringLiteral("/project-intake");
}

bool ProjectIntakeConsumer::rememberCreationBuild(const QString& path, const QString& buildId, QString* error)
{
    const QString destination = creationBuildPath(path);
    if (!QDir().mkpath(QFileInfo(destination).absolutePath())) {
        *error = QStringLiteral("Could not save the selected project build");
        return false;
    }
    QSaveFile file(destination);
    const auto bytes = QJsonDocument(QJsonObject{{"buildId", buildId},
        {"requestedAt", QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)}}).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}

void ProjectIntakeConsumer::forgetCreationBuild(const QString& path)
{
    QFile::remove(creationBuildPath(path));
}

ProjectIntakeConsumer::ProjectIntakeConsumer(ProjectStore* store,
                                             QObject* parent)
    : QObject(parent)
    , store_(store)
{
}

void ProjectIntakeConsumer::start()
{
    const QString directory = intakeDir();
    QDir().mkpath(directory);

    watcher_ = new QFileSystemWatcher(this);
    watcher_->addPath(directory);
    debounceTimer_ = new QTimer(this);
    debounceTimer_->setSingleShot(true);
    debounceTimer_->setInterval(300);
    connect(watcher_, &QFileSystemWatcher::directoryChanged, debounceTimer_,
            qOverload<>(&QTimer::start));
    connect(debounceTimer_, &QTimer::timeout, this, [this]() {
        if (consumeAll())
            emit projectsUpdated();
    });

    if (consumeAll())
        emit projectsUpdated();
}

bool ProjectIntakeConsumer::consumeAll()
{
    bool changed = false;
    const QDir directory(intakeDir());
    const QFileInfoList entries = directory.entryInfoList(
        {QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QFileInfo& entry : entries) {
        QJsonObject record;
        if (entry.size() <= kMaxRecordBytes) {
            QFile file(entry.absoluteFilePath());
            if (file.open(QIODevice::ReadOnly)) {
                const QJsonDocument document =
                    QJsonDocument::fromJson(file.readAll());
                if (document.isObject())
                    record = document.object();
            }
        }
        const RecordAction action =
            record.isEmpty() ? RecordAction::Drop : applyRecord(record);
        if (action == RecordAction::Keep)
            continue;
        QFile::remove(entry.absoluteFilePath());
        if (action == RecordAction::Applied)
            changed = true;
    }
    return changed;
}

ProjectIntakeConsumer::RecordAction
ProjectIntakeConsumer::applyRecord(const QJsonObject& record)
{
    const ProjectProbeResult probe = ProjectProbe::probe(
        record.value(QStringLiteral("projectPath")).toString());
    if (!probe.ok)
        return RecordAction::Drop;

    QString buildId = record.value(QStringLiteral("buildId")).toString();
    if (!buildId.isEmpty() && !isSafeBuildId(buildId))
        buildId.clear();
    const QString openedAt = record.value(QStringLiteral("openedAt")).toString();
    const auto timestamp = QDateTime::fromString(openedAt, Qt::ISODateWithMs);
    if (!timestamp.isValid())
        return RecordAction::Drop;

    QString preferredBuild;
    QFile intent(creationBuildPath(probe.canonicalPath));
    if (intent.open(QIODevice::ReadOnly) && intent.size() <= kMaxRecordBytes) {
        const auto selection = QJsonDocument::fromJson(intent.readAll()).object();
        const auto requested = QDateTime::fromString(selection["requestedAt"].toString(), Qt::ISODateWithMs);
        if (requested.isValid() && timestamp >= requested && isSafeBuildId(selection["buildId"].toString()))
            preferredBuild = selection["buildId"].toString();
    }
    intent.close();
    const auto completed = [&](bool applied) {
        if (applied && !preferredBuild.isEmpty()) forgetCreationBuild(probe.canonicalPath);
        return applied ? RecordAction::Applied : RecordAction::Keep;
    };
    if (!store_->contains(probe.canonicalPath)) {
        ProjectRecord project;
        project.projectId = probe.projectId;
        project.canonicalPath = probe.canonicalPath;
        project.displayName = probe.displayName;
        project.detectedSchema = probe.schemaVersion;
        project.lastKnownGoodStudioBuildId = buildId;
        project.preferredStudioBuildId = preferredBuild;
        project.lastOpenedAt = openedAt;
        return completed(store_->addProject(project));
    }
    for (const auto& project : store_->listProjects()) {
        if (project.canonicalPath != probe.canonicalPath)
            continue;
        if ((!project.projectId.isEmpty() && !probe.projectId.isEmpty()
             && QUuid(project.projectId) != QUuid(probe.projectId))
            || timestamp <= QDateTime::fromString(project.lastOpenedAt, Qt::ISODateWithMs))
            return RecordAction::Drop;
        if (project.preferredStudioBuildId.isEmpty() && !preferredBuild.isEmpty()
            && !store_->setPreferredBuild(probe.canonicalPath, preferredBuild)) return RecordAction::Keep;
        return completed(store_->recordSuccessfulOpen(
                   probe.canonicalPath,
                   probe.projectId.isEmpty() ? project.projectId : probe.projectId,
                   probe.metadataWarning.isEmpty() ? probe.displayName : project.displayName,
                   probe.schemaVersion > 0 ? probe.schemaVersion : project.detectedSchema,
                   buildId.isEmpty() ? project.lastKnownGoodStudioBuildId : buildId,
                   openedAt));
    }
    return RecordAction::Keep;
}

}
