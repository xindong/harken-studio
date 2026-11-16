#include "MediaRegistry.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>

namespace HarkenEditor::Bridge {
namespace {

struct LocalMediaEntry
{
    QString path;
    uint64_t fileSize = 0;
};

QHash<uint32_t, LocalMediaEntry>& localMediaRegistry()
{
    static QHash<uint32_t, LocalMediaEntry> registry;
    return registry;
}

QMutex& localMediaRegistryMutex()
{
    static QMutex mutex;
    return mutex;
}

QString normalizedMediaPath(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

}

QString mediaRegistryFindPath(uint32_t sourceId)
{
    QMutexLocker lock(&localMediaRegistryMutex());
    const auto it = localMediaRegistry().constFind(sourceId);
    return it == localMediaRegistry().constEnd() ? QString() : it->path;
}

void mediaRegistryRegister(uint32_t sourceId, const QString& absolutePath, uint64_t fileSize)
{
    if (sourceId == 0 || absolutePath.isEmpty())
        return;
    QMutexLocker lock(&localMediaRegistryMutex());
    localMediaRegistry().insert(sourceId, LocalMediaEntry{normalizedMediaPath(absolutePath), fileSize});
}

void mediaRegistryUnregister(uint32_t sourceId)
{
    QMutexLocker lock(&localMediaRegistryMutex());
    localMediaRegistry().remove(sourceId);
}

uint32_t mediaRegistryCount()
{
    QMutexLocker lock(&localMediaRegistryMutex());
    return static_cast<uint32_t>(localMediaRegistry().size());
}

void mediaRegistryClear()
{
    QMutexLocker lock(&localMediaRegistryMutex());
    localMediaRegistry().clear();
}

uint32_t mediaRegistryFindSourceIdByPath(const QString& absolutePath)
{
    const QString needle = normalizedMediaPath(absolutePath);
    QMutexLocker lock(&localMediaRegistryMutex());
    for (auto it = localMediaRegistry().constBegin(); it != localMediaRegistry().constEnd(); ++it)
    {
        if (it->path == needle)
            return it.key();
    }
    return 0;
}

}
