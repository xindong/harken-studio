#include "CurrentWorldArtifactWriter.h"
#include "CurrentWorldArtifactWriterInternal.h"
#include "CurrentWorldProjectAdapter.h"
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

bool WriteCurrentWorldArtifactV1(
    ProjectData& project,
    PlatformScope scope,
    const QString& targetPath,
    QString* error)
{
    auto request = buildCurrentWorldSyncRequest(project, scope, error);
    if (!request)
        return false;
    auto compiled = HarkenEditor::Bridge::compileCurrentWorldRequest(*request);
    if (!compiled.ok()) {
        setError(error, QStringLiteral(
            "Current World project compilation failed (%1, stable id %2)")
                .arg(static_cast<std::uint32_t>(compiled.status))
                .arg(VNX::objectIdString(compiled.relatedStableId)));
        return false;
    }
    return WriteCurrentWorldArtifactV1(targetPath, compiled.world->View(),
                                       error);
}

bool WriteCurrentWorldArtifactV1(
    ProjectData& project,
    PlatformScope scope,
    const QString& targetPath,
    const std::unordered_map<harkenstudio::ObjectId, std::uint64_t>& soundRevisions,
    std::uint64_t* semanticFingerprint,
    QString* error)
{
    auto request = buildCurrentWorldSyncRequest(project, scope, error);
    if (!request)
        return false;
    request->soundContentRevisions = soundRevisions;
    auto compiled = HarkenEditor::Bridge::compileCurrentWorldRequest(*request);
    if (!compiled.ok()) {
        setError(error, QStringLiteral(
            "Current World project compilation failed (%1, stable id %2)")
                .arg(static_cast<std::uint32_t>(compiled.status))
                .arg(VNX::objectIdString(compiled.relatedStableId)));
        return false;
    }
    const auto view = compiled.world->View();
    if (semanticFingerprint != nullptr)
        *semanticFingerprint = view.identity.runtimeSemanticFingerprint;
    return WriteCurrentWorldArtifactV1(targetPath, view, error);
}

}
