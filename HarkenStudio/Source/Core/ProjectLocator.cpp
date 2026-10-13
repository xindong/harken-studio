#include "ProjectLocator.h"

#include <QDir>
#include <QFileInfo>

namespace VNX {

ProjectLocation ProjectLocator::locate(const QString& inputPath)
{
    ProjectLocation result;
    const QFileInfo inputInfo(inputPath);
    if (inputInfo.isFile()) {
        if (inputInfo.suffix().compare(QStringLiteral("hsp"), Qt::CaseInsensitive) != 0) {
            result.issue = ProjectIssue::ProjectNotFound;
            return result;
        }
        result.projectFilePath = inputInfo.canonicalFilePath();
        if (result.projectFilePath.isEmpty())
            result.projectFilePath = inputInfo.absoluteFilePath();
        return result;
    }
    if (!inputInfo.isDir()) {
        result.issue = ProjectIssue::ProjectNotFound;
        return result;
    }

    const QDir directory(inputInfo.absoluteFilePath());
    const QStringList names = directory.entryList(
        {QStringLiteral("*.hsp")}, QDir::Files | QDir::NoSymLinks, QDir::Name);
    for (const QString& name : names) {
        QFileInfo candidate(directory.filePath(name));
        QString path = candidate.canonicalFilePath();
        if (path.isEmpty())
            path = candidate.absoluteFilePath();
        result.candidates.append(path);
    }
    if (result.candidates.isEmpty()) {
        result.issue = ProjectIssue::ProjectNotFound;
        return result;
    }
    if (result.candidates.size() != 1) {
        result.issue = ProjectIssue::ProjectAmbiguous;
        return result;
    }
    result.projectFilePath = result.candidates.constFirst();
    return result;
}

}
