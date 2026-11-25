#include "Versions/StudioInterface.h"

#include "Platform/PlatformPaths.h"
#include "Versions/VersionScanner.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QVersionNumber>

namespace harken::hub::StudioInterface {

namespace {

constexpr int kListTemplatesTimeoutMs = 30000;
constexpr int kIdentifyTimeoutMs = 10000;

int compare(const QString& studioVersion, const char* reference)
{
    return QVersionNumber::compare(QVersionNumber::fromString(studioVersion),
                                   QVersionNumber::fromString(QLatin1String(reference)));
}

QString text(const char* source)
{
    return QCoreApplication::translate("HarkenHub", source);
}

bool isValidLegacySelfTest(const QJsonObject& report)
{
    if (!report.value(QStringLiteral("ok")).toBool()
        || !report.value(QStringLiteral("schemaArchiveOk")).toBool()
        || report.value(QStringLiteral("buildType")).toString().trimmed().isEmpty())
        return false;

    const int protocol = report.value(QStringLiteral("capabilitiesVersion")).toInt();
    if (protocol == 1)
        return report.value(QStringLiteral("projectSchema")).toObject()
                   .value(QStringLiteral("current")).toInt() > 0;

    static const QRegularExpression commitHash(QStringLiteral("\\A[0-9a-f]{40}\\z"));
    static const QRegularExpression snapshotHash(QStringLiteral("\\A[0-9a-f]{64}\\z"));
    return protocol == 2
        && report.value(QStringLiteral("editorSchemaVersion")).toInt() > 0
        && report.value(QStringLiteral("packSchemaVersion")).toInt() > 0
        && report.value(QStringLiteral("gitDirty")).isBool()
        && commitHash.match(report.value(QStringLiteral("gitCommit")).toString()).hasMatch()
        && snapshotHash.match(report.value(QStringLiteral("schemaSnapshotHash")).toString()).hasMatch();
}

}

bool canCreateProjects(const QString& studioVersion)
{
    return compare(studioVersion, "0.7.0") >= 0;
}

bool canListTemplates(const QString& studioVersion)
{
    return compare(studioVersion, "0.9.4") > 0;
}

QString executable(const InstalledVersion& version)
{
    return version.executablePath.isEmpty()
        ? QDir(version.installPath).filePath(studioBinaryRelativePath())
        : version.executablePath;
}

std::optional<StudioIdentity> identify(const QString& executable, QString* error)
{
    QProcess process;
    process.start(executable, {QStringLiteral("--version"), QStringLiteral("--json"), QStringLiteral("--self-test")});
    if (!process.waitForFinished(kIdentifyTimeoutMs)) {
        process.kill();
        process.waitForFinished();
        *error = QStringLiteral("HarkenStudio did not report its version: %1").arg(executable);
        return std::nullopt;
    }
    const QJsonObject response = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
    QJsonObject result;
    if (response.contains(QStringLiteral("status"))) {
        if (response.value(QStringLiteral("status")).toString() == QStringLiteral("ok"))
            result = response.value(QStringLiteral("result")).toObject();
    } else if (isValidLegacySelfTest(response)) {
        result = response;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0
        || result.value(QStringLiteral("product")).toString() != QStringLiteral("HarkenStudio")) {
        *error = QStringLiteral("The selected executable is not a HarkenStudio that reports its version: %1").arg(executable);
        return std::nullopt;
    }
    StudioIdentity identity{result.value(QStringLiteral("version")).toString().trimmed(),
                            result.value(QStringLiteral("buildId")).toString().trimmed(),
                            result.value(QStringLiteral("channel")).toString().trimmed()};
    if (identity.version.isEmpty() || identity.buildId.isEmpty() || identity.channel.isEmpty()) {
        *error = QStringLiteral("HarkenStudio did not report a complete build identity: %1").arg(executable);
        return std::nullopt;
    }
    return identity;
}

std::optional<std::vector<StudioTemplate>> listTemplates(const InstalledVersion& version, QString* error)
{
    if (!canListTemplates(version.semanticVersion))
        return std::vector<StudioTemplate>{};
    QProcess process;
    process.start(executable(version), {QStringLiteral("--list-templates"), QStringLiteral("--json")});
    if (!process.waitForFinished(kListTemplatesTimeoutMs)) {
        process.kill();
        process.waitForFinished();
        *error = text(QT_TRANSLATE_NOOP("HarkenHub", "Studio did not list its project templates."));
        return std::nullopt;
    }
    const QJsonValue templates = QJsonDocument::fromJson(process.readAllStandardOutput())
                                     .object().value(QStringLiteral("templates"));
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0 || !templates.isArray()) {
        *error = text(QT_TRANSLATE_NOOP("HarkenHub", "Studio did not list its project templates."));
        return std::nullopt;
    }
    std::vector<StudioTemplate> result;
    for (const QJsonValue& value : templates.toArray()) {
        const QJsonObject object = value.toObject();
        const QString id = object.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;
        result.push_back({id, object.value(QStringLiteral("displayName")).toString(),
                          object.value(QStringLiteral("description")).toString()});
    }
    return result;
}

}
