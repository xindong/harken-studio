#include "Tooling/Harkenizer/Core/CurrentWorldArtifactWriter.h"
#include "Tooling/Harkenizer/Core/CurrentWorldArtifactWriterInternal.h"
#include "Tooling/Harkenizer/Core/CurrentWorldProjectAdapter.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"

#include <harken_studio_build/WorldArtifact.h>

#include <QSaveFile>

#include <limits>
#include <memory>
#include <vector>

namespace VNX {
namespace {

class QSaveCurrentWorldArtifactFile final : public CurrentWorldArtifactFile
{
public:
    explicit QSaveCurrentWorldArtifactFile(const QString& targetPath)
        : file_(targetPath)
    {
    }

    void setDirectWriteFallback(bool enabled) override
    {
        file_.setDirectWriteFallback(enabled);
    }

    bool open() override
    {
        return file_.open(QIODevice::WriteOnly);
    }

    qint64 write(const char* data, qint64 byteCount) override
    {
        return file_.write(data, byteCount);
    }

    bool commit() override
    {
        return file_.commit();
    }

private:
    QSaveFile file_;
};

CurrentWorldArtifactFileFactory fileFactory;

std::unique_ptr<CurrentWorldArtifactFile> makeFile(const QString& targetPath)
{
    if (fileFactory != nullptr)
        return fileFactory(targetPath);
    return std::make_unique<QSaveCurrentWorldArtifactFile>(targetPath);
}

void setError(QString* error, const QString& value)
{
    if (error != nullptr)
        *error = value;
}

} // namespace

bool WriteCurrentWorldArtifactV1(
    const QString& targetPath,
    const harkenstudiobuild::ContainerWorldInstallViewV1& world,
    QString* error)
{
    if (targetPath.isEmpty()) {
        setError(error, QStringLiteral("Current World artifact path is empty"));
        return false;
    }
    std::vector<std::uint8_t> bytes;
    if (!harkenstudiobuild::EncodeCurrentWorldArtifactV1(world, bytes)
        || bytes.size() > static_cast<std::size_t>(
               std::numeric_limits<qint64>::max())) {
        setError(error, QStringLiteral("Current World artifact encoding failed"));
        return false;
    }
    std::unique_ptr<CurrentWorldArtifactFile> file = makeFile(targetPath);
    if (!file) {
        setError(error, QStringLiteral("Current World artifact file is unavailable"));
        return false;
    }
    file->setDirectWriteFallback(false);
    if (!file->open()) {
        setError(error, QStringLiteral("Current World artifact open failed"));
        return false;
    }
    const qint64 byteCount = static_cast<qint64>(bytes.size());
    if (file->write(reinterpret_cast<const char*>(bytes.data()), byteCount)
        != byteCount) {
        setError(error, QStringLiteral("Current World artifact write failed"));
        return false;
    }
    if (!file->commit()) {
        setError(error, QStringLiteral("Current World artifact commit failed"));
        return false;
    }
    return true;
}

void setCurrentWorldArtifactFileFactoryForTesting(
    CurrentWorldArtifactFileFactory factory) noexcept
{
    fileFactory = factory;
}

}
