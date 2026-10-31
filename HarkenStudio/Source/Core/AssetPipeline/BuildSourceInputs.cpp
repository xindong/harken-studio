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
        QString contentHash;
        QSet<QString> paths;
        std::shared_ptr<const BuildSourceInput> input;
    };

    mutable QMutex mutex;
    QHash<QString, Entry> entries;
    QHash<QString, QString> aliases;
    QMap<uint32_t, QString> fingerprints;
    std::shared_ptr<QTemporaryDir> storage;

    Entry* identify(const AssetInfo& asset, QString* error)
    {
        const auto path = sourcePath(asset);
        if (path.isEmpty()) {
            fail(error, QStringLiteral("Audio source has no file path: %1").arg(asset.sourceID));
            return nullptr;
        }
        auto key = aliases.value(path);
        if (key.isEmpty()) {
            key = sourceKey(path);
            aliases.insert(path, key);
        }
        auto found = entries.find(key);
        if (found != entries.end()) {
            found->paths.insert(path);
            fingerprints.insert(asset.sourceID, found->contentHash);
            return &*found;
        }
        const auto hash = AssetImporter::computeContentHash(path);
        if (hash.isEmpty()) {
            fail(error, QStringLiteral("Cannot read audio source: %1").arg(path));
            return nullptr;
        }
        fingerprints.insert(asset.sourceID, hash);
        return &*entries.insert(key, {hash, {path}, {}});
    }
};

BuildSourceInputs::BuildSourceInputs() : impl_(std::make_unique<Impl>()) {}
BuildSourceInputs::~BuildSourceInputs() = default;

QString BuildSourceInputs::sourceFingerprint(const AssetInfo& asset, QString* error)
{
    QMutexLocker lock(&impl_->mutex);
    const auto* entry = impl_->identify(asset, error);
    return entry ? entry->contentHash : QString();
}

void BuildSourceInputs::beginDependencyScope()
{
    QMutexLocker lock(&impl_->mutex);
    impl_->fingerprints.clear();
}

bool BuildSourceInputs::adopt(BuildSourceInputs& probes, QString* error)
{
    if (&probes == this) return true;
    std::scoped_lock lock(impl_->mutex, probes.impl_->mutex);
    for (auto it = probes.impl_->entries.cbegin(); it != probes.impl_->entries.cend(); ++it) {
        const auto previous = impl_->entries.constFind(it.key());
        if (previous != impl_->entries.cend() && previous->contentHash != it->contentHash) {
            fail(error, QStringLiteral("Source file changed during the build: %1").arg(it.key()));
            return false;
        }
        for (const auto& path : it->paths) {
            const auto alias = impl_->aliases.constFind(path);
            if (alias == impl_->aliases.cend()) continue;
            const auto existing = impl_->entries.constFind(*alias);
            if (existing != impl_->entries.cend() && existing->contentHash != it->contentHash) {
                fail(error, QStringLiteral("Source file changed during the build: %1").arg(path));
                return false;
            }
        }
    }
    for (auto it = probes.impl_->entries.cbegin(); it != probes.impl_->entries.cend(); ++it) {
        auto existing = impl_->entries.find(it.key());
        if (existing == impl_->entries.end()) impl_->entries.insert(it.key(), *it);
        else {
            existing->paths.unite(it->paths);
            if (!existing->input) existing->input = it->input;
        }
    }
    impl_->aliases.insert(probes.impl_->aliases);
    impl_->fingerprints.insert(probes.impl_->fingerprints);
    return true;
}

QMap<uint32_t, QString> BuildSourceInputs::fingerprints() const
{
    QMutexLocker lock(&impl_->mutex);
    return impl_->fingerprints;
}

std::shared_ptr<const BuildSourceInput> BuildSourceInputs::capture(const AssetInfo& asset, QString* error)
{
    QMutexLocker lock(&impl_->mutex);
    auto* entry = impl_->identify(asset, error);
    if (!entry) return {};
    if (entry->input) return entry->input;
    if (!impl_->storage) impl_->storage = std::make_shared<QTemporaryDir>();
    if (!impl_->storage->isValid()) {
        fail(error, QStringLiteral("Cannot create audio source snapshot directory"));
        return {};
    }
    const auto path = sourcePath(asset);
    const auto modified = QFileInfo(path).lastModified();
    const QString fileName = QString::fromLatin1(QCryptographicHash::hash(
        impl_->aliases.value(path).toUtf8(), QCryptographicHash::Sha256).toHex())
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
    QMutexLocker lock(&impl_->mutex);
    for (const auto& entry : impl_->entries) {
        for (const auto& path : entry.paths) {
            if (canceled && canceled()) {
                fail(error, QStringLiteral("Export canceled"));
                return false;
            }
            if (AssetImporter::computeContentHash(path) != entry.contentHash) {
                fail(error, QStringLiteral("Source file changed during the build: %1").arg(path));
                return false;
            }
        }
    }
    return true;
}

}
