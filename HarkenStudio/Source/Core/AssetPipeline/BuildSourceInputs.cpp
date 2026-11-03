#include "BuildSourceInputs.h"

#include "AssetImporter.h"
#include "AssetRegistry.h"
#include "SourceFileState.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QTemporaryDir>
#include <mutex>
#include <shared_mutex>

namespace VNX {

namespace {

QString sourcePath(const AssetInfo& asset)
{
    return asset.absolutePath.isEmpty() ? QString() : QFileInfo(asset.absolutePath).absoluteFilePath();
}

QString sourceKey(const QString& path)
{
    const auto canonical = QFileInfo(path).canonicalFilePath();
    QString key = QDir::cleanPath(canonical.isEmpty() ? path : canonical);
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
}

void fail(QString* error, const QString& message)
{
    if (error) *error = message;
}

}

struct BuildSourceInputs::Impl
{
    struct Entry
    {
        QString key;
        QMutex mutex;
        AssetInfo asset;
        QString error;
        std::shared_ptr<const BuildSourceInput> input;
    };

    QString projectRoot;
    mutable std::shared_mutex operations;
    mutable QMutex mutex;
    QHash<QString, std::shared_ptr<Entry>> entries;
    QHash<QString, QString> aliases;
    QHash<uint32_t, AssetInfo> assets;
    QHash<uint32_t, AssetInfo> changed;
    std::shared_ptr<QTemporaryDir> storage;

    std::shared_ptr<Entry> find(const AssetInfo& asset, QString* error)
    {
        const auto path = sourcePath(asset);
        if (path.isEmpty()) {
            fail(error, QStringLiteral("Audio source has no file path: %1").arg(asset.sourceID));
            return {};
        }
        QMutexLocker lock(&mutex);
        auto key = aliases.value(path);
        if (key.isEmpty()) {
            lock.unlock();
            const auto canonical = sourceKey(path);
            lock.relock();
            key = aliases.value(path);
            if (key.isEmpty()) {
                key = canonical;
                aliases.insert(path, key);
            }
        }
        auto found = entries.find(key);
        if (found != entries.end()) return *found;
        auto entry = std::make_shared<Entry>();
        entry->key = key;
        entry->asset = assets.value(asset.sourceID, asset);
        entries.insert(key, entry);
        return entry;
    }

    bool capture(Entry& entry, QFile& source, const QString& path, const QByteArray& before,
                 bool reusable, bool identify, QString* error)
    {
        {
            QMutexLocker lock(&mutex);
            if (!storage) storage = std::make_shared<QTemporaryDir>();
        }
        if (!storage->isValid()) {
            fail(error, QStringLiteral("Cannot create audio source snapshot directory"));
            return false;
        }
        const QString capturedPath = storage->filePath(QString::fromLatin1(
            QCryptographicHash::hash(entry.key.toUtf8(), QCryptographicHash::Sha256).toHex())
            + QLatin1Char('.') + QFileInfo(path).suffix());
        QFile captured(capturedPath);
        if (!captured.open(QIODevice::WriteOnly)) {
            fail(error, QStringLiteral("Cannot capture audio source: %1").arg(path));
            return false;
        }
        const auto modified = QFileInfo(path).lastModified();
        QCryptographicHash hash(QCryptographicHash::Sha256);
        QByteArray buffer(1024 * 1024, Qt::Uninitialized);
        for (;;) {
            const auto count = source.read(buffer.data(), buffer.size());
            if (count < 0 || (count > 0 && captured.write(buffer.constData(), count) != count)) {
                fail(error, QStringLiteral("Cannot capture audio source: %1").arg(path));
                return false;
            }
            if (count == 0) break;
            if (identify) hash.addData(QByteArrayView(buffer.constData(), count));
        }
#ifndef Q_OS_WIN
        if (before != sourceFileState(source)) {
            fail(error, QStringLiteral("Source file changed while reading: %1").arg(path));
            return false;
        }
#endif
        if (!captured.flush() || !captured.setFileTime(modified, QFileDevice::FileModificationTime)) {
            fail(error, QStringLiteral("Cannot finish audio source snapshot: %1").arg(path));
            return false;
        }
        captured.close();
        auto input = std::make_shared<BuildSourceInput>();
        input->originalPath = path;
        input->inputPath = capturedPath;
        input->contentHash = identify ? QString::fromLatin1(hash.result().toHex()) : entry.asset.contentHash;
        const bool metadataUnchanged = input->contentHash == entry.asset.contentHash
            && entry.asset.metadata.sampleRate != 0 && entry.asset.metadata.channels != 0
            && entry.asset.metadata.totalSamples != 0;
        input->metadata = metadataUnchanged ? entry.asset.metadata : AssetImporter().readMetadata(capturedPath);
        input->storage_ = storage;
        if (input->metadata.sampleRate == 0 || input->metadata.channels == 0
            || input->metadata.totalSamples == 0) {
            fail(error, QStringLiteral("Cannot analyze audio source: %1").arg(path));
            return false;
        }
        if (identify) {
            if (entry.asset.contentHash != input->contentHash) entry.asset.waveformState.clear();
            entry.asset.contentHash = input->contentHash;
            entry.asset.metadata = input->metadata;
            entry.asset.sourceState = reusable ? before : QByteArray();
            entry.asset.lastModified = modified.toSecsSinceEpoch();
        }
        entry.input = std::move(input);
        return true;
    }

    bool identify(Entry& entry, const AssetInfo& asset, QString* error)
    {
        if (!entry.error.isEmpty()) {
            fail(error, entry.error);
            return false;
        }
        if (!entry.input) {
            const auto path = sourcePath(asset);
            QFile source(path);
            if (!openSourceFile(source)) {
                entry.error = QStringLiteral("Cannot read audio source: %1").arg(path);
                fail(error, entry.error);
                return false;
            }
            bool reusable = false;
            const auto before = sourceFileState(source, &reusable);
            if (reusable && before == entry.asset.sourceState && !entry.asset.contentHash.isEmpty()
                && entry.asset.metadata.sampleRate != 0 && entry.asset.metadata.channels != 0
                && entry.asset.metadata.totalSamples != 0) {
                auto input = std::make_shared<BuildSourceInput>();
                input->originalPath = path;
                input->inputPath = path;
                input->contentHash = entry.asset.contentHash;
                input->metadata = entry.asset.metadata;
                entry.input = std::move(input);
            } else if (!capture(entry, source, path, before, reusable, true, &entry.error)) {
                fail(error, entry.error);
                return false;
            }
        }
        QMutexLocker lock(&mutex);
        const auto found = assets.constFind(asset.sourceID);
        if (found != assets.cend() && found->sourceState == entry.asset.sourceState
            && found->contentHash == entry.asset.contentHash) return true;
        auto observed = found == assets.cend() ? asset : *found;
        if (observed.contentHash != entry.asset.contentHash) observed.waveformState.clear();
        observed.sourceState = entry.asset.sourceState;
        observed.contentHash = entry.asset.contentHash;
        observed.metadata = entry.asset.metadata;
        observed.lastModified = entry.asset.lastModified;
        changed.insert(asset.sourceID, observed);
        assets.insert(asset.sourceID, observed);
        return true;
    }
};

BuildSourceInputs::BuildSourceInputs(QString projectRoot, QHash<uint32_t, AssetInfo> assets)
    : impl_(std::make_unique<Impl>())
{
    impl_->projectRoot = std::move(projectRoot);
    impl_->assets = std::move(assets);
}

BuildSourceInputs::~BuildSourceInputs() = default;

void BuildSourceInputs::beginRequest()
{
    std::unique_lock operation(impl_->operations);
    impl_->entries.clear();
    impl_->aliases.clear();
    impl_->storage.reset();
}

QHash<uint32_t, AssetInfo> BuildSourceInputs::assets() const
{
    QMutexLocker lock(&impl_->mutex);
    return impl_->assets;
}

std::shared_ptr<const BuildSourceInput> BuildSourceInputs::inspect(const AssetInfo& asset, QString* error)
{
    std::shared_lock operation(impl_->operations);
    const auto entry = impl_->find(asset, error);
    if (!entry) return {};
    QMutexLocker lock(&entry->mutex);
    return impl_->identify(*entry, asset, error) ? entry->input : nullptr;
}

std::shared_ptr<const BuildSourceInput> BuildSourceInputs::capture(const AssetInfo& asset, QString* error)
{
    std::shared_lock operation(impl_->operations);
    const auto entry = impl_->find(asset, error);
    if (!entry) return {};
    QMutexLocker lock(&entry->mutex);
    if (!impl_->identify(*entry, asset, error)) return {};
    if (entry->input->storage_) return entry->input;
    const auto path = sourcePath(asset);
    QFile source(path);
    bool reusable = false;
    if (!openSourceFile(source)) {
        entry->error = QStringLiteral("Cannot read audio source: %1").arg(path);
        fail(error, entry->error);
        return {};
    }
    const auto before = sourceFileState(source, &reusable);
    if (!reusable || before != entry->asset.sourceState) {
        entry->error = QStringLiteral("Source file changed before reading: %1").arg(path);
        fail(error, entry->error);
        return {};
    }
    if (!impl_->capture(*entry, source, path, before, reusable, false, &entry->error)) {
        fail(error, entry->error);
        return {};
    }
    return entry->input;
}

QByteArray BuildSourceInputs::waveformState(uint32_t sourceID) const
{
    QMutexLocker lock(&impl_->mutex);
    return impl_->assets.value(sourceID).waveformState;
}

void BuildSourceInputs::setWaveformState(uint32_t sourceID, const QByteArray& state)
{
    QMutexLocker lock(&impl_->mutex);
    const auto found = impl_->assets.find(sourceID);
    if (found == impl_->assets.end() || found->waveformState == state) return;
    found->waveformState = state;
    impl_->changed.insert(sourceID, *found);
}

bool BuildSourceInputs::save()
{
    std::unique_lock operation(impl_->operations);
    if (impl_->projectRoot.isEmpty() || impl_->changed.isEmpty()) return true;
    if (!AssetRegistry::saveBuildSources(impl_->projectRoot, impl_->assets, impl_->changed)) return false;
    impl_->changed.clear();
    return true;
}

}
