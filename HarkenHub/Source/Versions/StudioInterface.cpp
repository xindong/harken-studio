#include "Versions/StudioInterface.h"

#include "Platform/PlatformPaths.h"
#include "Versions/VersionScanner.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QVersionNumber>

namespace VNH::StudioInterface {

namespace {

constexpr int kListTemplatesTimeoutMs = 30000;

int compare(const QString& studioVersion, const char* reference)
{
    return QVersionNumber::compare(QVersionNumber::fromString(studioVersion),
                                   QVersionNumber::fromString(QLatin1String(reference)));
}

QString text(const char* source)
{
    return QCoreApplication::translate("HarkenHub", source);
}

}

bool canCreateProjects(const QString& studioVersion)
{
    return compare(studioVersion, "0.7.0") >= 0;
}

bool canSelfTest(const QString& studioVersion)
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
