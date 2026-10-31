#include "BuildSourceInputs.h"

#include "AssetImporter.h"
#include "Utils/AudioFileReader.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
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
        QString contentHash;
        QSet<QString> paths;
        std::shared_ptr<const BuildSourceInput> input;
    };

    mutable std::shared_mutex operations;
    mutable QMutex mutex;
    QHash<QString, std::shared_ptr<Entry>> entries;
    QHash<QString, QString> aliases;
    QMap<uint32_t, QString> fingerprints;
    std::once_flag storageInitialization;
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
        entries.insert(key, entry);
        return entry;
    }

    bool identify(Entry& entry, const AssetInfo& asset, QString* error)
    {
        const auto path = sourcePath(asset);
        if (entry.contentHash.isEmpty()) {
            const auto hash = AssetImporter::computeContentHash(path);
            if (hash.isEmpty()) {
                fail(error, QStringLiteral("Cannot read audio source: %1").arg(path));
                return false;
            }
            entry.contentHash = hash;
        }
        entry.paths.insert(path);
        QMutexLocker lock(&mutex);
        fingerprints.insert(asset.sourceID, entry.contentHash);
        return true;
    }
};

BuildSourceInputs::BuildSourceInputs() : impl_(std::make_unique<Impl>()) {}
BuildSourceInputs::~BuildSourceInputs() = default;

QString BuildSourceInputs::sourceFingerprint(const AssetInfo& asset, QString* error)
{
    std::shared_lock operation(impl_->operations);
    const auto entry = impl_->find(asset, error);
    if (!entry) return {};
    QMutexLocker lock(&entry->mutex);
    return impl_->identify(*entry, asset, error) ? entry->contentHash : QString();
}

void BuildSourceInputs::beginDependencyScope()
{
    std::unique_lock operation(impl_->operations);
    impl_->fingerprints.clear();
}

bool BuildSourceInputs::adopt(BuildSourceInputs& probes, QString* error)
{
    if (&probes == this) return true;
    std::scoped_lock lock(impl_->operations, probes.impl_->operations);
    for (auto it = probes.impl_->entries.cbegin(); it != probes.impl_->entries.cend(); ++it) {
        const auto& probe = **it;
        if (probe.contentHash.isEmpty()) continue;
        const auto previous = impl_->entries.constFind(it.key());
        if (previous != impl_->entries.cend() && !(*previous)->contentHash.isEmpty()
            && (*previous)->contentHash != probe.contentHash) {
            fail(error, QStringLiteral("Source file changed during the build: %1").arg(it.key()));
            return false;
        }
        for (const auto& path : probe.paths) {
            const auto alias = impl_->aliases.constFind(path);
            if (alias == impl_->aliases.cend()) continue;
            const auto existing = impl_->entries.constFind(*alias);
            if (existing != impl_->entries.cend() && !(*existing)->contentHash.isEmpty()
                && (*existing)->contentHash != probe.contentHash) {
                fail(error, QStringLiteral("Source file changed during the build: %1").arg(path));
                return false;
            }
        }
    }
    for (auto it = probes.impl_->entries.cbegin(); it != probes.impl_->entries.cend(); ++it) {
        const auto& probe = **it;
        if (probe.contentHash.isEmpty()) continue;
        auto existing = impl_->entries.find(it.key());
        if (existing == impl_->entries.end()) {
            auto entry = std::make_shared<Impl::Entry>();
            entry->key = probe.key;
            existing = impl_->entries.insert(it.key(), std::move(entry));
        }
        auto& entry = **existing;
        entry.contentHash = probe.contentHash;
        entry.paths.unite(probe.paths);
        if (!entry.input) entry.input = probe.input;
    }
    impl_->aliases.insert(probes.impl_->aliases);
    impl_->fingerprints.insert(probes.impl_->fingerprints);
    return true;
}

QMap<uint32_t, QString> BuildSourceInputs::fingerprints() const
{
    std::unique_lock operation(impl_->operations);
    return impl_->fingerprints;
}

std::shared_ptr<const BuildSourceInput> BuildSourceInputs::capture(const AssetInfo& asset, QString* error)
{
    std::shared_lock operation(impl_->operations);
    const auto entry = impl_->find(asset, error);
    if (!entry) return {};
    QMutexLocker lock(&entry->mutex);
    if (!impl_->identify(*entry, asset, error)) return {};
    if (entry->input) return entry->input;
    std::call_once(impl_->storageInitialization, [&] {
        impl_->storage = std::make_shared<QTemporaryDir>();
    });
    if (!impl_->storage->isValid()) {
        fail(error, QStringLiteral("Cannot create audio source snapshot directory"));
        return {};
    }
    const auto path = sourcePath(asset);
    const auto modified = QFileInfo(path).lastModified();
    const QString fileName = QString::fromLatin1(QCryptographicHash::hash(
        entry->key.toUtf8(), QCryptographicHash::Sha256).toHex())
        + QLatin1Char('.') + QFileInfo(path).suffix();
    const auto capturedPath = impl_->storage->filePath(fileName);
    AudioFileReader source(path);
    QFile captured(capturedPath);
    if (!source.openReadOnly() || !captured.open(QIODevice::WriteOnly)) {
        fail(error, QStringLiteral("Cannot capture audio source: %1").arg(path));
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (;;) {
        const auto count = source.read(buffer.data(), buffer.size());
        if (count < 0) {
            fail(error, QStringLiteral("Cannot read audio source: %1").arg(path));
            return {};
        }
        if (count == 0) break;
        hash.addData(QByteArrayView(buffer.constData(), count));
        if (captured.write(buffer.constData(), count) != count) {
            fail(error, QStringLiteral("Cannot capture audio source: %1").arg(path));
            return {};
        }
    }
    if (QString::fromLatin1(hash.result().toHex()) != entry->contentHash) {
        fail(error, QStringLiteral("Source file changed during the build: %1").arg(path));
        return {};
    }
    if (!captured.flush()) {
        fail(error, QStringLiteral("Cannot capture audio source: %1").arg(path));
        return {};
    }
    if (!captured.setFileTime(modified, QFileDevice::FileModificationTime)) {
        fail(error, QStringLiteral("Cannot preserve audio source metadata: %1").arg(path));
        return {};
    }
    captured.close();
    AssetImporter importer;
    auto input = std::make_shared<BuildSourceInput>();
    input->originalPath = path;
    input->inputPath = capturedPath;
    input->contentHash = entry->contentHash;
    input->metadata = importer.readMetadata(capturedPath);
    input->storage_ = impl_->storage;
    if (input->metadata.sampleRate == 0 || input->metadata.channels == 0
        || input->metadata.totalSamples == 0) {
        fail(error, QStringLiteral("Cannot analyze audio source: %1").arg(path));
        return {};
    }
    entry->input = input;
    return input;
}

bool BuildSourceInputs::verifyUnchanged(QString* error, const std::function<bool()>& canceled) const
{
    std::unique_lock operation(impl_->operations);
    for (const auto& entry : impl_->entries) {
        if (entry->contentHash.isEmpty()) continue;
        for (const auto& path : entry->paths) {
            if (canceled && canceled()) {
                fail(error, QStringLiteral("Export canceled"));
                return false;
            }
            if (AssetImporter::computeContentHash(path) != entry->contentHash) {
                fail(error, QStringLiteral("Source file changed during the build: %1").arg(path));
                return false;
            }
        }
    }
    return true;
}

}
