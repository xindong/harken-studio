#pragma once

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <vector>

namespace VNX {

class ProjectMigrationWorkspace
{
public:
    ProjectMigrationWorkspace(QString sourceDirectory,
                              QString stagingDirectory,
                              QStringList writeSet,
                              QString projectFile = {});

    bool readObject(const QString& relativePath,
                    QJsonObject* object,
                    QString* error = nullptr);
    bool writeObject(const QString& relativePath,
                     const QJsonObject& object,
                     QString* error = nullptr);
    bool flush(QString* error = nullptr);

    int parseCount(const QString& relativePath) const;
    int writeCount(const QString& relativePath) const;
    QStringList dirtyFiles() const;
    QString projectFile() const;
    QString sourceDirectory() const;
    QString stagingDirectory() const;
    QStringList writeSet() const;
    const QHash<QString, QJsonObject>& loadedObjects() const;

private:
    QString normalize(const QString& relativePath) const;

    QString sourceDirectory_;
    QString stagingDirectory_;
    QString projectFile_;
    QSet<QString> writeSet_;
    QHash<QString, QJsonObject> objects_;
    QHash<QString, int> parseCounts_;
    QHash<QString, int> writeCounts_;
    QSet<QString> dirty_;
};

using ProjectMigrationTransform = std::function<bool(
    ProjectMigrationWorkspace&, QStringList*, QString*)>;

struct ProjectMigrationStep
{
    QString id;
    int fromSchema = 0;
    int toSchema = 0;
    QStringList dependencies;
    QStringList writeSet;
    ProjectMigrationTransform transform;
};

struct ProjectMigrationPlan
{
    bool ok = false;
    int sourceSchema = 0;
    int targetSchema = 0;
    std::vector<ProjectMigrationStep> steps;
    QStringList writeSet;
    int fileCount = 0;
    int stepCount = 0;
    qint64 recoveryBytes = 0;
    QString error;
};

struct ProjectMigrationProgress
{
    int completedSteps = 0;
    int totalSteps = 0;
    int completedFiles = 0;
    int totalFiles = 0;
    qint64 completedBytes = 0;
    qint64 totalBytes = 0;
    QString stepId;
    QString relativePath;
};

using ProjectMigrationProgressCallback =
    std::function<void(const ProjectMigrationProgress&)>;
using ProjectMigrationVersionValidator = std::function<bool(
    ProjectMigrationWorkspace&, int, QString*)>;

class ProjectMigrationRegistry
{
public:
    bool registerStep(ProjectMigrationStep step, QString* error = nullptr);
    ProjectMigrationPlan plan(int sourceSchema,
                              int targetSchema,
                              const QString& projectDirectory = {}) const;
    bool execute(const ProjectMigrationPlan& plan,
                 ProjectMigrationWorkspace& workspace,
                 QStringList* warnings = nullptr,
                 QString* error = nullptr,
                 ProjectMigrationProgressCallback progress = {},
                 ProjectMigrationVersionValidator validator = {}) const;

private:
    std::vector<ProjectMigrationStep> steps_;
};

}
