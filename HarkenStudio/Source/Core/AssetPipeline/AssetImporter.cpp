
#include "AssetImporter.h"
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

#define DR_WAV_IMPLEMENTATION
#include <dr_wav/dr_wav.h>

#include "RuntimeBridge/PackagingBridge.h"

namespace VNX {


AssetImporter::AssetImporter(QObject* parent)
    : QObject(parent)
{
}

AssetImporter::~AssetImporter() = default;

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

AudioMetadata AssetImporter::analyzeFile(const QString& path)
{
    QString format = detectFormat(path);

    if (format == "WAV") {
        return readWavMetadata(path);
    } else if (format == "MP3") {
        return readMp3Metadata(path);
    } else if (format == "FLAC") {
        return readFlacMetadata(path);
    } else if (format == "OGG") {
        return readOggMetadata(path);
    } else if (format == "AIFF") {
        return readAiffMetadata(path);
    }

    return AudioMetadata();
}

static drwav_uint64 smplChunkCallback(void* pChunkUserData, drwav_read_proc onRead,
                                       drwav_seek_proc , void* pReadSeekUserData,
                                       const drwav_chunk_header* pChunkHeader,
                                       drwav_container , const drwav_fmt* )
{
    if (pChunkHeader->id.fourcc[0] == 's' && pChunkHeader->id.fourcc[1] == 'm' &&
        pChunkHeader->id.fourcc[2] == 'p' && pChunkHeader->id.fourcc[3] == 'l')
    {
        auto* loopInfo = static_cast<LoopInfo*>(pChunkUserData);
        drwav_uint8 header[36];
        size_t bytesRead = onRead(pReadSeekUserData, header, 36);
        if (bytesRead < 36) return 0;

        uint32_t numLoops = 0;
        memcpy(&numLoops, header + 28, 4);

        if (numLoops > 0)
        {
            drwav_uint8 loopData[24];
            bytesRead = onRead(pReadSeekUserData, loopData, 24);
            if (bytesRead >= 24)
            {
                uint32_t loopStart = 0, loopEnd = 0;
                memcpy(&loopStart, loopData + 8, 4);
                memcpy(&loopEnd, loopData + 12, 4);

                loopInfo->enabled = true;
                loopInfo->loopStart = loopStart;
                loopInfo->loopEnd = loopEnd;
                loopInfo->loopCount = 0;
            }
        }
    }
    return 0;
}

LoopInfo AssetImporter::readWavLoopInfo(const QString& path)
{
    LoopInfo info;
    drwav wav;

#ifdef _WIN32
    if (!drwav_init_file_ex_w(&wav, reinterpret_cast<const wchar_t*>(path.utf16()),
                               smplChunkCallback, &info, 0, nullptr)) {
        return info;
    }
#else
    if (!drwav_init_file_ex(&wav, path.toUtf8().constData(),
                             smplChunkCallback, &info, 0, nullptr)) {
        return info;
    }
#endif

    drwav_uninit(&wav);
    return info;
}

ValidationFlags AssetImporter::validateAudio(const QString& path)
{
    ValidationFlags flags;

    drwav wav;
#ifdef _WIN32
    if (!drwav_init_file_w(&wav, reinterpret_cast<const wchar_t*>(path.utf16()), nullptr)) {
        return flags;
    }
#else
    if (!drwav_init_file(&wav, path.toUtf8().constData(), nullptr)) {
        return flags;
    }
#endif

    constexpr size_t ANALYSIS_SAMPLES = 44100 * 10;
    size_t samplesToRead = std::min(static_cast<size_t>(wav.totalPCMFrameCount * wav.channels),
                                    ANALYSIS_SAMPLES);

    std::vector<float> samples(samplesToRead);
    size_t framesRead = drwav_read_pcm_frames_f32(&wav, samplesToRead / wav.channels, samples.data());

    if (framesRead == 0) {
        drwav_uninit(&wav);
        return flags;
    }

    float peak = 0.0f;
    double sumSquares = 0.0;
    double dcSum = 0.0;
    size_t actualSamples = framesRead * wav.channels;

    for (size_t i = 0; i < actualSamples; ++i) {
        float sample = samples[i];
        float absSample = std::abs(sample);

        if (absSample > peak) {
            peak = absSample;
        }

        sumSquares += sample * sample;
        dcSum += sample;

        if (absSample >= 1.0f) {
            flags.hasClipping = true;
        }
    }

    flags.peakDb = (peak > 0.0f) ? 20.0f * std::log10(peak) : -144.0f;

    double rms = std::sqrt(sumSquares / actualSamples);
    flags.rmsDb = (rms > 0.0) ? static_cast<float>(20.0 * std::log10(rms)) : -144.0f;

    flags.truePeakDb = flags.peakDb + 0.5f;

    double dcOffset = dcSum / actualSamples;
    flags.hasDCOffset = std::abs(dcOffset) > 0.01;

    {
        uint32_t channels = wav.channels;
        uint64_t totalFrames = framesRead;
        double sr = static_cast<double>(wav.sampleRate);

        double hsB0 = 1.53512485958697, hsB1 = -2.69169618940638, hsB2 = 1.19839281085285;
        double hsA1 = -1.69065929318241, hsA2 = 0.73248077421585;
        double hpB0 = 1.0, hpB1 = -2.0, hpB2 = 1.0;
        double hpA1 = -1.99004745483398, hpA2 = 0.99007225036621;

        if (std::abs(sr - 48000.0) > 1.0) {
            flags.integratedLUFS = flags.rmsDb - 0.691f;
        } else {
            double kWeightedEnergy = 0.0;
            uint64_t kSampleCount = 0;

            for (uint32_t ch = 0; ch < channels && ch < 2; ++ch) {
                double hs_x1 = 0, hs_x2 = 0, hs_y1 = 0, hs_y2 = 0;
                double hp_x1 = 0, hp_x2 = 0, hp_y1 = 0, hp_y2 = 0;

                for (uint64_t f = 0; f < totalFrames; ++f) {
                    double x = static_cast<double>(samples[f * channels + ch]);

                    double hsOut = hsB0 * x + hsB1 * hs_x1 + hsB2 * hs_x2
                                 - hsA1 * hs_y1 - hsA2 * hs_y2;
                    hs_x2 = hs_x1; hs_x1 = x;
                    hs_y2 = hs_y1; hs_y1 = hsOut;

                    double hpOut = hpB0 * hsOut + hpB1 * hp_x1 + hpB2 * hp_x2
                                 - hpA1 * hp_y1 - hpA2 * hp_y2;
                    hp_x2 = hp_x1; hp_x1 = hsOut;
                    hp_y2 = hp_y1; hp_y1 = hpOut;

                    kWeightedEnergy += hpOut * hpOut;
                    ++kSampleCount;
                }
            }

            if (kSampleCount > 0) {
                double meanSquare = kWeightedEnergy / static_cast<double>(kSampleCount);
                if (meanSquare > 1e-20) {
                    flags.integratedLUFS = static_cast<float>(
                        -0.691 + 10.0 * std::log10(meanSquare));
                }
            }
        }
    }

    drwav_uninit(&wav);
    return flags;
}


bool AssetImporter::validateFile(const QString& path, QString& errorMsg)
{
    QFileInfo fileInfo(path);

    if (!fileInfo.exists()) {
        errorMsg = tr("File does not exist");
        return false;
    }

    if (!fileInfo.isFile()) {
        errorMsg = tr("Path is not a file");
        return false;
    }

    if (fileInfo.size() == 0) {
        errorMsg = tr("File is empty");
        return false;
    }

    if (!isFormatSupported(path)) {
        errorMsg = tr("Unsupported audio format: %1").arg(fileInfo.suffix());
        return false;
    }

    if (PathUtils::hasUnsafeCharacters(fileInfo.fileName())) {
        errorMsg = tr("Filename contains invalid characters");
        return false;
    }

    return true;
}

QStringList AssetImporter::supportedFormats()
{
    return AudioFormatCatalog::extensions();
}

bool AssetImporter::isFormatSupported(const QString& path)
{
    return PathUtils::isSupportedAudioFormat(path);
}


QString AssetImporter::computeContentHash(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "[AssetImporter] computeContentHash: cannot open" << path;
        return QString();
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        qWarning() << "[AssetImporter] computeContentHash: read failed" << path;
        return QString();
    }

    return QString::fromLatin1(hash.result().toHex());
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

QString AssetImporter::detectFormat(const QString& path)
{
    QString ext = PathUtils::getExtension(path);

    if (ext == "wav") return "WAV";
    if (ext == "mp3") return "MP3";
    if (ext == "flac") return "FLAC";
    if (ext == "ogg") return "OGG";
    if (ext == "aiff" || ext == "aif") return "AIFF";

    return QString();
}

AudioMetadata AssetImporter::readWavMetadata(const QString& path)
{
    AudioMetadata meta;

    drwav wav;
#ifdef _WIN32
    if (!drwav_init_file_w(&wav, reinterpret_cast<const wchar_t*>(path.utf16()), nullptr)) {
        qWarning() << "Failed to open WAV file:" << path;
        return meta;
    }
#else
    if (!drwav_init_file(&wav, path.toUtf8().constData(), nullptr)) {
        qWarning() << "Failed to open WAV file:" << path;
        return meta;
    }
#endif

    meta.sampleRate = wav.sampleRate;
    meta.channels = static_cast<uint16_t>(wav.channels);
    meta.bitDepth = static_cast<uint16_t>(wav.bitsPerSample);
    meta.totalSamples = wav.totalPCMFrameCount;
    meta.durationSec = static_cast<double>(wav.totalPCMFrameCount) / wav.sampleRate;
    meta.fileSize = QFileInfo(path).size();
    meta.originalFormat = "WAV";

    meta.loopInfo = readWavLoopInfo(path);

    meta.validation = validateAudio(path);

    drwav_uninit(&wav);
    return meta;
}

AudioMetadata AssetImporter::readMp3Metadata(const QString& path)
{
    AudioMetadata meta;
    meta.originalFormat = "MP3";
    meta.fileSize = QFileInfo(path).size();

    const auto info = HarkenEditor::Bridge::audioFileInfo(path);
    if (info.valid)
    {
        meta.sampleRate = static_cast<uint32_t>(info.sampleRate);
        meta.channels = static_cast<uint16_t>(info.channels);
        meta.bitDepth = static_cast<uint16_t>(info.bitDepth);
        meta.totalSamples = static_cast<uint64_t>(info.frameCount);
        meta.durationSec = info.durationSeconds;
    }
    else
    {
        qWarning() << "[AssetImporter] Failed to read MP3 metadata:" << path;
    }

    return meta;
}

AudioMetadata AssetImporter::readFlacMetadata(const QString& path)
{
    AudioMetadata meta;
    meta.originalFormat = "FLAC";
    meta.fileSize = QFileInfo(path).size();

    const auto info = HarkenEditor::Bridge::audioFileInfo(path);
    if (info.valid)
    {
        meta.sampleRate = static_cast<uint32_t>(info.sampleRate);
        meta.channels = static_cast<uint16_t>(info.channels);
        meta.bitDepth = static_cast<uint16_t>(info.bitDepth);
        meta.totalSamples = static_cast<uint64_t>(info.frameCount);
        meta.durationSec = info.durationSeconds;
    }
    else
    {
        qWarning() << "[AssetImporter] Failed to read FLAC metadata:" << path;
    }

    return meta;
}

AudioMetadata AssetImporter::readOggMetadata(const QString& path)
{
    AudioMetadata meta;
    meta.originalFormat = "OGG";
    meta.fileSize = QFileInfo(path).size();

    const auto info = HarkenEditor::Bridge::audioFileInfo(path);
    if (info.valid)
    {
        meta.sampleRate = static_cast<uint32_t>(info.sampleRate);
        meta.channels = static_cast<uint16_t>(info.channels);
        meta.bitDepth = static_cast<uint16_t>(info.bitDepth);
        meta.totalSamples = static_cast<uint64_t>(info.frameCount);
        meta.durationSec = info.durationSeconds;
    }
    else
    {
        qWarning() << "[AssetImporter] Failed to read OGG metadata:" << path;
    }

    return meta;
}

AudioMetadata AssetImporter::readAiffMetadata(const QString& path)
{
    AudioMetadata meta;
    meta.originalFormat = "AIFF";
    meta.fileSize = QFileInfo(path).size();

    const auto info = HarkenEditor::Bridge::audioFileInfo(path);
    if (info.valid)
    {
        meta.sampleRate = static_cast<uint32_t>(info.sampleRate);
        meta.channels = static_cast<uint16_t>(info.channels);
        meta.bitDepth = static_cast<uint16_t>(info.bitDepth);
        meta.totalSamples = static_cast<uint64_t>(info.frameCount);
        meta.durationSec = info.durationSeconds;
    }
    else
    {
        qWarning() << "[AssetImporter] Failed to read AIFF metadata:" << path;
    }

    return meta;
}

}
