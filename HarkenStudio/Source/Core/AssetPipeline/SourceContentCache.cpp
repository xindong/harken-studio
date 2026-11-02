#include "SourceContentCache.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>
#include <QSet>
#include <atomic>

#if defined(Q_OS_WIN)
#include <QtCore/qt_windows.h>
#include <fcntl.h>
#include <io.h>
#elif defined(Q_OS_MACOS)
#include <sys/mount.h>
#include <sys/stat.h>
#elif defined(Q_OS_LINUX)
#include <sys/stat.h>
#include <sys/vfs.h>
#endif

namespace VNX {

namespace {

struct FileState
{
    QByteArray identity;
    bool reusable = false;
};

bool openReadOnly(QFile& file, bool attributesOnly = false)
{
#if defined(Q_OS_WIN)
    const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(file.fileName().utf16()),
        attributesOnly ? FILE_READ_ATTRIBUTES : GENERIC_READ,
        FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const auto descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
    if (descriptor == -1) {
        CloseHandle(handle);
        return false;
    }
    if (file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) return true;
    _close(descriptor);
    return false;
#else
    Q_UNUSED(attributesOnly);
    return file.open(QIODevice::ReadOnly);
#endif
}

FileState fileState(const QFile& file)
{
    FileState result;
    QDataStream stream(&result.identity, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
#if defined(Q_OS_WIN)
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(file.handle()));
    FILE_BASIC_INFO basic{};
    FILE_STANDARD_INFO standard{};
    FILE_ID_INFO id{};
    if (handle == INVALID_HANDLE_VALUE || GetFileType(handle) != FILE_TYPE_DISK
        || !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))
        || !GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard))
        || !GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id))
        || standard.Directory)
        return {};
    stream << quint64(id.VolumeSerialNumber)
           << QByteArray(reinterpret_cast<const char*>(id.FileId.Identifier), sizeof(id.FileId.Identifier))
           << qint64(standard.EndOfFile.QuadPart) << qint64(basic.LastWriteTime.QuadPart)
           << qint64(basic.ChangeTime.QuadPart);
    wchar_t fileSystem[32]{};
    wchar_t resolved[512]{};
    const auto length = GetFinalPathNameByHandleW(handle, resolved, 512, VOLUME_NAME_DOS);
    const auto local = length > 0 && length < 512
        && QString::fromWCharArray(resolved, length).startsWith(QStringLiteral("\\\\?\\"))
        && !QString::fromWCharArray(resolved, length).startsWith(QStringLiteral("\\\\?\\UNC\\"), Qt::CaseInsensitive);
    if (local && GetVolumeInformationByHandleW(handle, nullptr, 0, nullptr, nullptr, nullptr,
                                              fileSystem, 32)) {
        const auto type = QString::fromWCharArray(fileSystem);
        result.reusable = basic.ChangeTime.QuadPart != 0
            && (type == QStringLiteral("NTFS") || type == QStringLiteral("ReFS"));
    }
#elif defined(Q_OS_MACOS) || defined(Q_OS_LINUX)
    struct stat info{};
    if (fstat(file.handle(), &info) != 0 || !S_ISREG(info.st_mode)) return {};
#if defined(Q_OS_MACOS)
    const auto modified = info.st_mtimespec;
    const auto changed = info.st_ctimespec;
#else
    const auto modified = info.st_mtim;
    const auto changed = info.st_ctim;
#endif
    stream << quint64(info.st_dev) << quint64(info.st_ino) << qint64(info.st_size)
           << qint64(modified.tv_sec) << qint64(modified.tv_nsec)
           << qint64(changed.tv_sec) << qint64(changed.tv_nsec);
    struct statfs volume{};
    if (fstatfs(file.handle(), &volume) == 0) {
#if defined(Q_OS_MACOS)
        result.reusable = (volume.f_flags & MNT_LOCAL) != 0
            && QByteArray(volume.f_fstypename) == QByteArrayLiteral("apfs");
#else
        const auto type = quint64(volume.f_type);
        result.reusable = changed.tv_nsec != 0
            && (type == 0xEF53 || type == 0x58465342 || type == 0x9123683E
                || type == 0x01021994 || type == 0x2FC12FC1);
#endif
    }
#endif
    return result;
}

bool validDigest(const QString& digest)
{
    const auto bytes = digest.toLatin1();
    return bytes.size() == 64 && QByteArray::fromHex(bytes).toHex() == bytes;
}

QString fileKey(const QString& path)
{
    auto key = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    key = key.toCaseFolded();
#endif
    return key;
}

}

QByteArray packFileState(const QString& path)
{
    QFile file(path);
    if (!openReadOnly(file, true)) return {};
    const auto state = fileState(file);
    return state.reusable ? state.identity : QByteArray();
}

struct SourceContentCache::Impl
{
    struct Entry
    {
        QMutex mutex;
        QByteArray identity;
        QString digest;
    };

    QString cachePath;
    QMutex mutex;
    QMutex saveMutex;
    QMap<QString, std::shared_ptr<Entry>> entries;
    std::atomic<quint64> revision{0};
    quint64 savedRevision = 0;

    void load()
    {
        if (cachePath.isEmpty()) return;
        QFile file(cachePath);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 64 * 1024 * 1024) return;
        const auto document = QJsonDocument::fromJson(file.readAll());
        const auto root = document.object();
        if (root.value(QStringLiteral("version")).toInt() != 1
            || !root.value(QStringLiteral("sources")).isObject())
            return;
        const auto files = root.value(QStringLiteral("sources")).toObject();
        QMap<QString, std::shared_ptr<Entry>> loaded;
        for (auto it = files.begin(); it != files.end(); ++it) {
            const auto record = it.value().toObject();
            const auto identity = record.value(QStringLiteral("state")).toString().toLatin1();
            const auto digest = record.value(QStringLiteral("digest")).toString();
            if (it.key().isEmpty() || identity.isEmpty()
                || QByteArray::fromHex(identity).toHex() != identity || !validDigest(digest))
                return;
            auto entry = std::make_shared<Entry>();
            entry->identity = QByteArray::fromHex(identity);
            entry->digest = digest;
            loaded.insert(it.key(), std::move(entry));
        }
        entries = std::move(loaded);
    }
};

SourceContentCache::SourceContentCache(QString cachePath) : impl_(std::make_unique<Impl>())
{
    impl_->cachePath = std::move(cachePath);
    impl_->load();
}

SourceContentCache::~SourceContentCache() = default;

QString SourceContentCache::digest(const QString& path, const std::function<bool()>& canceled,
                                   QByteArray* state)
{
    if (state) state->clear();
    if (path.isEmpty() || (canceled && canceled())) return {};
    const auto key = fileKey(path);
    std::shared_ptr<Impl::Entry> entry;
    {
        QMutexLocker lock(&impl_->mutex);
        auto& slot = impl_->entries[key];
        if (!slot) slot = std::make_shared<Impl::Entry>();
        entry = slot;
    }
    QMutexLocker lock(&entry->mutex);
    QFile file(path);
    if (!openReadOnly(file)) return {};
    const auto before = fileState(file);
    if (before.reusable && before.identity == entry->identity && !entry->digest.isEmpty()) {
        if (canceled && canceled()) return {};
        if (state) *state = before.identity;
        return entry->digest;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(1024 * 1024, Qt::Uninitialized);
    for (;;) {
        if (canceled && canceled()) return {};
        const auto count = file.read(buffer.data(), buffer.size());
        if (count < 0) return {};
        if (count == 0) break;
        hash.addData(QByteArrayView(buffer.constData(), count));
    }
    const auto after = fileState(file);
    if (before.identity != after.identity || (canceled && canceled())) return {};
    const auto digest = QString::fromLatin1(hash.result().toHex());
    const auto identity = before.reusable && after.reusable ? after.identity : QByteArray();
    if (entry->identity != identity || entry->digest != digest) {
        entry->identity = identity;
        entry->digest = digest;
        ++impl_->revision;
    }
    if (state) *state = identity;
    return digest;
}

void SourceContentCache::retainSources(const QStringList& paths)
{
    QSet<QString> retained;
    for (const auto& path : paths) retained.insert(fileKey(path));
    QMutexLocker lock(&impl_->mutex);
    for (auto it = impl_->entries.begin(); it != impl_->entries.end();) {
        if (retained.contains(it.key())) {
            ++it;
        } else {
            it = impl_->entries.erase(it);
            ++impl_->revision;
        }
    }
}

bool SourceContentCache::save()
{
    if (impl_->cachePath.isEmpty()) return true;
    QMutexLocker saveLock(&impl_->saveMutex);
    const auto revision = impl_->revision.load();
    if (revision == impl_->savedRevision) return true;
    QMap<QString, std::shared_ptr<Impl::Entry>> entries;
    {
        QMutexLocker lock(&impl_->mutex);
        entries = impl_->entries;
    }
    QJsonObject files;
    for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
        QMutexLocker lock(&it.value()->mutex);
        const auto& entry = *it.value();
        if (!entry.identity.isEmpty() && !entry.digest.isEmpty()) {
            files.insert(it.key(), QJsonObject{
                {QStringLiteral("state"), QString::fromLatin1(entry.identity.toHex())},
                {QStringLiteral("digest"), entry.digest}});
        }
    }
    const auto bytes = QJsonDocument(QJsonObject{
        {QStringLiteral("version"), 1}, {QStringLiteral("sources"), files}}).toJson(QJsonDocument::Compact);
    if (!QDir().mkpath(QFileInfo(impl_->cachePath).absolutePath())) return false;
    QSaveFile file(impl_->cachePath);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return false;
    impl_->savedRevision = revision;
    return true;
}

}
