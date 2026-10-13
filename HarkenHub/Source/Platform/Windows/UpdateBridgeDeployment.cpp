#include "Platform/UpdateBridgeDeployment.h"

#include "Platform/AclHardening.h"
#include "Platform/ProcessScan.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <windows.h>

namespace VNH {

namespace {

bool copyDirectory(const QString& sourceDir, const QString& targetDir,
                   QString* error)
{
    if (!QDir().mkpath(targetDir)) {
        *error = QStringLiteral("Cannot create the bridge directory: %1")
                     .arg(targetDir);
        return false;
    }
    const QDir source(sourceDir);
    QDirIterator it(sourceDir, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QFileInfo info = it.fileInfo();
        if (info.isSymbolicLink() || info.isJunction()) {
            *error = QStringLiteral("Bridge payload holds a reparse point: %1")
                         .arg(info.absoluteFilePath());
            return false;
        }
        const QString relative =
            source.relativeFilePath(info.absoluteFilePath());
        const QString destination =
            targetDir + QLatin1Char('/') + relative;
        if (info.isDir()) {
            if (!QDir().mkpath(destination)) {
                *error = QStringLiteral(
                             "Cannot create the bridge subdirectory: %1")
                             .arg(destination);
                return false;
            }
            continue;
        }
        if (!QFile::copy(info.absoluteFilePath(), destination)) {
            *error = QStringLiteral("Cannot copy the bridge file: %1")
                         .arg(relative);
            return false;
        }
    }
    return true;
}

}

QString updateBridgeBinaryName()
{
    return QStringLiteral("HarkenHubUpdateBridge.exe");
}

QString deployUpdateBridgePayload(const QString& sourceDir,
                                  const QString& targetRoot,
                                  const QString& buildId,
                                  const QString& rootSddl, QString* error)
{
    static const QRegularExpression safeBuildId(
        QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$"));
    if (!safeBuildId.match(buildId).hasMatch()) {
        *error = QStringLiteral("Build id is not a safe directory name: %1")
                     .arg(buildId);
        return QString();
    }
    const QString sourceExe =
        sourceDir + QLatin1Char('/') + updateBridgeBinaryName();
    if (!QFileInfo::exists(sourceExe)) {
        *error = QStringLiteral("Bridge payload is missing: %1").arg(sourceExe);
        return QString();
    }
    if (!QDir().mkpath(targetRoot)) {
        *error = QStringLiteral("Cannot create the updater root: %1")
                     .arg(targetRoot);
        return QString();
    }

    QTemporaryDir deployment(targetRoot + QLatin1Char('/') + buildId +
                             QStringLiteral("-XXXXXX"));
    if (!deployment.isValid()) {
        *error = QStringLiteral("Cannot create the bridge deployment: %1")
                     .arg(deployment.errorString());
        return QString();
    }
    const QString targetDir = deployment.path();
    if (!copyDirectory(sourceDir, targetDir, error))
        return QString();

    const QString deployedExe =
        targetDir + QLatin1Char('/') + updateBridgeBinaryName();
    if (!QFileInfo::exists(deployedExe)) {
        *error = QStringLiteral("Bridge deployment is incomplete: %1")
                     .arg(deployedExe);
        return QString();
    }

    if (!applyDirectoryAcl(targetRoot, rootSddl, error) ||
        !applyDirectoryAcl(targetDir, rootSddl, error))
        return QString();
    deployment.setAutoRemove(false);
    return deployedExe;
}

void pruneUpdateBridgePayloads(const QString& targetRoot,
                              const QString& deployedExe)
{
    const QString currentDir = QFileInfo(deployedExe).absolutePath();
    const auto siblings = QDir(targetRoot).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    for (const QFileInfo& sibling : siblings) {
        if (sibling.isJunction() || sibling.absoluteFilePath() == currentDir ||
            !runningExecutablesUnder(sibling.absoluteFilePath()).isEmpty())
            continue;
        const QString oldExe = QDir(sibling.absoluteFilePath()).filePath(
            updateBridgeBinaryName());
        if (!DeleteFileW(reinterpret_cast<LPCWSTR>(oldExe.utf16())))
            continue;
        QDir(sibling.absoluteFilePath()).removeRecursively();
    }
}

}
