#include "Projects/ProjectProbe.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace VNH {

ProjectProbeResult ProjectProbe::probe(const QString& inputPath)
{
    ProjectProbeResult result;
    const auto fail = [&result](const char* code, const char* message) {
        result.errorCode = QString::fromLatin1(code);
        result.error = QCoreApplication::translate("HarkenHub", message);
        return result;
    };
    QFileInfo info(inputPath);
    if (inputPath.trimmed().isEmpty())
        return fail("PROJECT_NOT_FOUND", "The project file could not be found.");
    if (info.isDir()) {
        const QDir directory(info.absoluteFilePath());
        const QFileInfoList candidates = directory.entryInfoList(
            {QStringLiteral("*.hsp")}, QDir::Files | QDir::NoSymLinks, QDir::Name);
        for (const QFileInfo& candidate : candidates)
            result.candidates.append(candidate.canonicalFilePath());
        if (candidates.size() > 1)
            return fail("PROJECT_AMBIGUOUS", "This folder contains more than one project. Select a .hsp file.");
        if (candidates.isEmpty())
            return fail("PROJECT_NOT_FOUND", "The project file could not be found.");
        info = candidates.first();
    }
    if (!info.isFile() || info.suffix().compare(QStringLiteral("hsp"), Qt::CaseInsensitive) != 0)
        return fail("PROJECT_NOT_FOUND", "The project file could not be found.");
    QFile file(info.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly))
        return fail("PROJECT_UNREADABLE", "The project file cannot be read.");

    result.ok = true;
    result.canonicalPath = info.canonicalFilePath();
    result.displayName = info.completeBaseName();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    const QJsonObject root = document.object();
    const QJsonObject header = root.value(QStringLiteral("harkenDocument")).toObject();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || header.value(QStringLiteral("type")).toString() != QStringLiteral("project.root")) {
        result.metadataWarning = QCoreApplication::translate(
            "HarkenHub", "Project details are unavailable. The selected Studio will check the project.");
        return result;
    }
    const QUuid id(header.value(QStringLiteral("id")).toString());
    const QUuid projectId(header.value(QStringLiteral("projectId")).toString());
    if (!id.isNull() && id == projectId)
        result.projectId = id.toString(QUuid::WithoutBraces);
    result.savedStudioVersion = header.value(QStringLiteral("savedStudioVersion")).toString().trimmed();
    result.schemaVersion = header.value(QStringLiteral("schemaVersion")).toInt(0);
    const QString name = root.value(QStringLiteral("content")).toObject()
                             .value(QStringLiteral("metadata")).toObject()
                             .value(QStringLiteral("name")).toString();
    if (!name.isEmpty())
        result.displayName = name;
    return result;
}

}
