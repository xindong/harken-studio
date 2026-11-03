#include "Tooling/Harkenizer/Core/ProjectPackExporter.h"
#include "Tooling/Harkenizer/Core/CanonicalPackSetExport.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>

#include <filesystem>

namespace VNX {
namespace {

bool preserveDirectorySpelling(const QString& baseDir, const ProjectPlatform& platform)
{
    QDir root(baseDir);
    const std::filesystem::path requested(root.filePath(platform.name).toStdU16String());
    for (const auto& entry : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (entry.isSymLink() || entry.fileName() == platform.name) continue;
        if (entry.fileName().normalized(QString::NormalizationForm_C).toCaseFolded() != platform.name.normalized(QString::NormalizationForm_C).toCaseFolded()) continue;
        std::error_code error;
        if (!std::filesystem::equivalent(std::filesystem::path(entry.absoluteFilePath().toStdU16String()), requested, error)) continue;
        const auto temporary = QStringLiteral(".harken-pack-rename-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (!root.rename(entry.fileName(), temporary)) return false;
        if (root.rename(temporary, platform.name)) return true;
        root.rename(temporary, entry.fileName());
        return false;
    }
    return true;
}

}

bool ProjectPackExporter::PreparePackOutputDirectory(const QString& baseDir,
                                                     const ProjectPlatform& platform,
                                                     QString* error)
{
    const QDir directory(platformOutputDir(baseDir, platform));
    if (!directory.mkpath(QStringLiteral("."))) {
        if (error)
            *error = QStringLiteral("Cannot create platform output directory: %1").arg(directory.path());
        return false;
    }
    if (!preserveDirectorySpelling(baseDir, platform)) {
        if (error)
            *error = QStringLiteral("Cannot update platform output directory name: %1").arg(directory.path());
        return false;
    }
    return true;
}

std::shared_ptr<QLockFile> ProjectPackExporter::LockPackOutputRoot(const QString& baseDir, QString* error)
{
    if (baseDir.isEmpty() || !QDir().mkpath(baseDir)) {
        if (error) *error = QStringLiteral("Cannot create Pack output root: %1").arg(baseDir);
        return {};
    }
    auto lock = std::make_shared<QLockFile>(QDir(baseDir).filePath(QStringLiteral(".harken-pack-build.lock")));
    lock->setStaleLockTime(0);
    if (!lock->tryLock()) {
        if (error) *error = QStringLiteral("Pack output is being built by another request: %1").arg(baseDir);
        return {};
    }
    return lock;
}

}
