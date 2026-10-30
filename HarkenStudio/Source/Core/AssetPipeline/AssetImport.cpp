
#include "AssetImporter.h"
#include "Utils/AudioFileReader.h"
#include "AssetRegistry.h"
#include "MediaPool.h"
#include "PathUtils.h"
#include "SourceFileSystem.h"
#include "AudioFormatCatalog.h"
#include "WaveformCache.h"
#include "../ProjectTypes.h"
#include "../UndoCommands.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QCryptographicHash>
#include <QDebug>
#include <cmath>

#include <dr_wav/dr_wav.h>

#include "RuntimeBridge/PackagingBridge.h"

namespace VNX {

void AssetImporter::setProjectRoot(const QString& root)
{
    projectRoot_ = root;
    qDebug() << "[AssetImporter] setProjectRoot:" << root;
}

ImportResult AssetImporter::importFile(const QString& externalPath,
                                        ProjectFileBinding* targetBinding,
                                        const QString& sourceFolder,
                                        bool allowRename,
                                        uint32_t restoreSourceID)
{
    ImportResult result;
    if (!SourceFileSystem::recoverDiscard(&result.errorMessage)) {
        result.error = AssetError::E_CONVERSION_FAILED;
        return result;
    }

    if (!sourceFolder.isEmpty() && restoreSourceID == 0) {
        const QString directory = SourceFileSystem::absolutePath(sourceFolder);
        if (directory.isEmpty() || !QFileInfo(directory).isDir()) {
            result.error = AssetError::E_CONVERSION_FAILED;
            result.errorMessage = tr("The destination source folder no longer exists.");
            return result;
        }
    }

    qDebug() << "[AssetImporter] importFile called:";
    qDebug() << "  externalPath:" << externalPath;
    qDebug() << "  projectRoot_:" << projectRoot_;

    QString errorMsg;
    if (!validateFile(externalPath, errorMsg)) {
        result.error = AssetError::E_UNSUPPORTED_FORMAT;
        result.errorMessage = errorMsg;
        qWarning() << "[AssetImporter] Validation failed:" << errorMsg;
        emit importError(externalPath, errorMsg);
        return result;
    }

    const QString destination = sourceFolder.isEmpty()
        ? PathUtils::calculateImportDestination(externalPath, projectRoot_)
        : QDir(SourceFileSystem::absolutePath(sourceFolder)).filePath(QFileInfo(externalPath).fileName());
    QString contentHash = computeContentHash(externalPath);
    if (contentHash.isEmpty()) {
        result.error = AssetError::E_CONVERSION_FAILED;
        result.errorMessage = tr("Could not read '%1'.").arg(externalPath);
        emit importError(externalPath, result.errorMessage);
        return result;
    }
    const auto& registry = AssetRegistry::instance();
    const AssetInfo* existing = restoreSourceID != 0 ? registry.getAsset(restoreSourceID)
        : allowRename ? registry.findAssetByPath(externalPath) : nullptr;
    if (!existing && restoreSourceID == 0)
        existing = registry.findAssetByPath(destination);
    if (restoreSourceID != 0 && (!existing || registry.sourceFileExists(restoreSourceID))) {
        result.error = AssetError::E_CONVERSION_FAILED;
        result.errorMessage = tr("The missing source changed before it could be restored. Import it again.");
        return result;
    }
    if (existing && !registry.sourceFileExists(existing->sourceID)) {
        const uint32_t sourceID = existing->sourceID;
        const QString relativePath = existing->relativePath;
        result.fileChange = SourceFileSystem::replaceFiles(
            {{relativePath, externalPath}}, &result.errorMessage);
        if (!result.fileChange) {
            result.error = AssetError::E_CONVERSION_FAILED;
            return result;
        }
        existing = registry.getAsset(sourceID);
    }
    if (existing && (result.fileChange
        || computeContentHash(existing->absolutePath) == contentHash)) {
        const uint32_t sourceID = existing->sourceID;
        if (!result.fileChange
            && !MediaPool::instance().refreshSourceFile(sourceID, existing->absolutePath)) {
            result.error = AssetError::E_CORRUPTED_FILE;
            result.errorMessage = tr("Failed to read audio file metadata");
            emit importError(externalPath, result.errorMessage);
            return result;
        }
        existing = registry.getAsset(sourceID);
        result.success = true;
        result.deduplicated = true;
        result.sourceID = existing->sourceID;
        result.relativePath = existing->relativePath;
        result.absolutePath = existing->absolutePath;
        result.metadata = existing->metadata;
        qDebug() << (result.fileChange ? "[AssetImporter] Restored:" : "[AssetImporter] Reused:")
                 << QFileInfo(externalPath).fileName()
                 << "-> existing SourceID:" << QString::number(existing->sourceID, 16);
        return result;
    }

    const bool alreadyStored = !allowRename && QFileInfo::exists(destination)
        && computeContentHash(destination) == contentHash;
    if (!allowRename && QFileInfo::exists(destination) && !alreadyStored) {
        result.error = AssetError::E_CONVERSION_FAILED;
        result.errorMessage = tr("'%1' has changed since import was confirmed. Import it again to choose whether to replace it.")
            .arg(QFileInfo(destination).fileName());
        return result;
    }
    QString destPath = alreadyStored ? destination : copyToAudioSources(externalPath, sourceFolder);
    if (destPath.isEmpty()) {
        result.error = AssetError::E_CONVERSION_FAILED;
        result.errorMessage = tr("Failed to copy file to AudioSources directory");
        qWarning() << "[AssetImporter] Copy to AudioSources failed";
        emit importError(externalPath, result.errorMessage);
        return result;
    }

    QString relativePath = PathUtils::toRelativePath(destPath, PathUtils::getAudioSourcesDir(projectRoot_));
    uint32_t sourceID = AssetRegistry::instance().allocateStableID();

    AudioMetadata metadata = analyzeFile(destPath);
    if (metadata.sampleRate == 0 || metadata.totalSamples == 0) {
        if (!alreadyStored)
            QFile::remove(destPath);
        result.error = AssetError::E_CORRUPTED_FILE;
        result.errorMessage = tr("Failed to read audio file metadata");
        emit importError(externalPath, result.errorMessage);
        return result;
    }

    AssetInfo info;
    info.sourceID = sourceID;
    info.relativePath = relativePath;
    info.absolutePath = destPath;
    info.contentHash = contentHash;
    info.ownerObjectGuid = (targetBinding && targetBinding->rootItem)
        ? targetBinding->rootItem->guid.toString(QUuid::WithoutBraces)
        : QString();
    info.metadata = metadata;
    info.lastModified = QFileInfo(destPath).lastModified().toSecsSinceEpoch();

    result.fileChange = SourceFileSystem::registerImport(info, !alreadyStored, &result.errorMessage);
    if (!result.fileChange) {
        if (!alreadyStored)
            QFile::remove(destPath);
        result.error = AssetError::E_ID_COLLISION;
        emit importError(externalPath, result.errorMessage);
        return result;
    }

    result.success = true;
    result.copiedFile = !alreadyStored;
    result.sourceID = sourceID;
    result.relativePath = relativePath;
    result.absolutePath = destPath;
    result.metadata = metadata;

    MediaPool::instance().refreshWaveform(sourceID);

    qDebug() << "Imported:" << QFileInfo(externalPath).fileName()
             << "-> SourceID:" << QString::number(sourceID, 16);

    return result;
}

QString AssetImporter::copyToAudioSources(const QString& externalPath, const QString& sourceFolder)
{
    qDebug() << "[AssetImporter] copyToAudioSources:";
    qDebug() << "  externalPath:" << externalPath;
    qDebug() << "  projectRoot_:" << projectRoot_;

    QString destPath = PathUtils::calculateImportDestination(externalPath, projectRoot_);
    if (sourceFolder.isEmpty() && !PathUtils::ensureDirectory(PathUtils::getAudioSourcesDir(projectRoot_)))
        return {};
    if (!sourceFolder.isEmpty()) {
        const QString directory = SourceFileSystem::absolutePath(sourceFolder);
        if (directory.isEmpty() || !QFileInfo(directory).isDir())
            return {};
        destPath = QDir(directory).filePath(QFileInfo(externalPath).fileName());
    }
    qDebug() << "  destPath:" << destPath;

    if (QFile::exists(destPath) || AssetRegistry::instance().findAssetByPath(destPath)) {
        QFileInfo info(destPath);
        QString baseName = info.completeBaseName();
        QString ext = info.suffix();
        QDir dir = info.absoluteDir();

        int counter = 1;
        while (QFile::exists(destPath) || AssetRegistry::instance().findAssetByPath(destPath)) {
            destPath = dir.absoluteFilePath(QString("%1_%2.%3").arg(baseName).arg(counter).arg(ext));
            ++counter;
        }
    }

    if (!QFile::copy(externalPath, destPath)) {
        qWarning() << "Failed to copy file:" << externalPath << "->" << destPath;
        return QString();
    }

    return destPath;
}

}
