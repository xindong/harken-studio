#include "Projects/ProjectIntakeConsumer.h"

#include "Platform/PlatformPaths.h"
#include "Projects/ProjectProbe.h"
#include "Projects/ProjectStore.h"

#include <QDateTime>
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

    if (!store_->contains(probe.canonicalPath)) {
        ProjectRecord project;
        project.projectId = probe.projectId;
        project.canonicalPath = probe.canonicalPath;
        project.displayName = probe.displayName;
        project.detectedSchema = probe.schemaVersion;
        project.lastKnownGoodStudioBuildId = buildId;
        project.lastOpenedAt = openedAt;
        return store_->addProject(project) ? RecordAction::Applied
                                           : RecordAction::Keep;
    }
    for (const auto& project : store_->listProjects()) {
        if (project.canonicalPath != probe.canonicalPath)
            continue;
        if ((!project.projectId.isEmpty() && !probe.projectId.isEmpty()
             && QUuid(project.projectId) != QUuid(probe.projectId))
            || timestamp <= QDateTime::fromString(project.lastOpenedAt, Qt::ISODateWithMs))
            return RecordAction::Drop;
        break;
    }
    if (!buildId.isEmpty())
        return store_->setLastKnownGood(probe.canonicalPath, buildId,
                                        openedAt)
                   ? RecordAction::Applied
                   : RecordAction::Keep;
    return store_->setLastOpened(probe.canonicalPath, openedAt)
               ? RecordAction::Applied
               : RecordAction::Keep;
}

}
