#include "ProjectPreflight.h"

#include "Core/MigrationManager.h"
#include "Core/ProjectContract.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

namespace VNX {

namespace {

QString formatSchemaMismatchError(int projectSchema, int studioSchema)
{
    return (projectSchema < studioSchema
                ? QStringLiteral(
                    "This project needs an update before it can open. "
                    "Open it in Harken Studio to review and confirm the update. "
                    "(Project Schema %1, this Harken Studio %2.)")
                : QStringLiteral(
                    "This project uses a newer project format. "
                    "Open it with a newer Harken Studio version. "
                    "(Project Schema %1, this Harken Studio %2.)"))
        .arg(projectSchema)
        .arg(studioSchema);
}

}

QJsonObject PreflightReport::toJson() const
{
    QJsonObject root;
    root[QStringLiteral("currentSchema")] = currentSchema;
    root[QStringLiteral("targetSchema")] = targetSchema;
    root[QStringLiteral("warnings")] = QJsonArray::fromStringList(warnings);
    root[QStringLiteral("blockingErrors")] =
        QJsonArray::fromStringList(blockingErrors);
    return root;
}

QString ProjectPreflight::schemaMismatchError(int projectSchema,
                                               int studioSchema)
{
    return formatSchemaMismatchError(projectSchema, studioSchema);
}

PreflightReport ProjectPreflight::run(const QString& projectPath,
                                      int targetSchema,
                                      ProjectContractReadContext* context)
{
    ProjectContractReadContext localContext;
    if (!context) context = &localContext;
    PreflightReport report;
    report.targetSchema = targetSchema > 0
                              ? targetSchema
                              : MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION;

    const QFileInfo info(projectPath);
    if (!info.exists() || !info.isFile()) {
        report.blockingErrors.append(
            QStringLiteral("Project file does not exist: %1").arg(projectPath));
        return report;
    }

    QFile file(projectPath);
    if (!file.open(QIODevice::ReadOnly)) {
        report.blockingErrors.append(
            QStringLiteral("Project file cannot be read"));
        return report;
    }
    const QByteArray content = file.readAll();
    file.close();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(content, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        report.blockingErrors.append(
            QStringLiteral("Project file is not valid JSON: %1")
                .arg(parseError.errorString()));
        return report;
    }

    const QJsonObject root = doc.object();
    report.currentSchema = MigrationManager::instance().detectVersion(root);

    const ProjectContractProbe probe =
        ProjectContractRegistry::instance().probe(projectPath, context);
    if (!probe.blockingErrors.isEmpty()) {
        report.blockingErrors.append(probe.blockingErrors);
        return report;
    }
    report.warnings.append(probe.warnings);

    if (report.targetSchema != MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION) {
        report.blockingErrors.append(
            QStringLiteral("Requested HarkenStudio Schema %1 does not match "
                           "this executable's Schema %2")
                .arg(report.targetSchema)
                .arg(MigrationManager::CURRENT_PROJECT_SCHEMA_VERSION));
        return report;
    }
    if (report.currentSchema != report.targetSchema) {
        report.blockingErrors.append(
            formatSchemaMismatchError(report.currentSchema, report.targetSchema));
        return report;
    }

    const QStringList governanceErrors =
        ProjectContractRegistry::instance().validateTree(
            info.absolutePath(), root, true, {}, context);
    if (!governanceErrors.isEmpty()) {
        report.blockingErrors.append(governanceErrors);
        return report;
    }

    return report;
}

}
