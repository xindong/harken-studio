#pragma once

#include <QByteArray>
#include <QString>
#include <QUuid>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <harken_bus_compiler/BusCompiler.h>

namespace VNX {
struct ProjectData;
}

namespace harkenprojectschema {
struct CompileTargetProfileV1;
class SegmentProgram;
class RuntimeIdentityIndexV1;
}

namespace HarkenEditor::Bridge {

enum class ProjectInputDiagnosticCode : uint8_t
{
    MissingIdentity = 0,
    DuplicateIdentity,
    UnsupportedObjectType,
    UnsupportedField,
    MissingReference,
    InvalidReferenceType,
    MissingResource,
    AmbiguousMarkerId,
    FrontEndRejected,
    IrRejected,
    ProgramRejected
};

struct ProjectInputDiagnostic
{
    ProjectInputDiagnosticCode code = ProjectInputDiagnosticCode::UnsupportedField;
    QString message;
    QUuid itemGuid;
    QUuid entityGuid;
    QString field;
};

struct PreviewResourceInput
{
    harkenstudio::ObjectId soundStableId = {};
    uint32_t sourceId = 0;
    QString absolutePath;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint64_t frameCount = 0;
    bool seekable = false;
};

struct LegacyPackBusDisplayRowV1
{
    harkenstudio::ObjectId busId = {};
    QString displayName;
};

struct ProjectInputCompileResult;

class CompiledProjectSnapshot
{
public:
    CompiledProjectSnapshot();
    ~CompiledProjectSnapshot();
    CompiledProjectSnapshot(CompiledProjectSnapshot&&) noexcept;
    CompiledProjectSnapshot& operator=(CompiledProjectSnapshot&&) noexcept;

    CompiledProjectSnapshot(const CompiledProjectSnapshot&) = delete;
    CompiledProjectSnapshot& operator=(const CompiledProjectSnapshot&) = delete;

    bool valid() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;

    friend ProjectInputCompileResult compileStudioProjectInput(
        const VNX::ProjectData& project,
        std::optional<harkenstudio::ObjectId> previewTrackStableId,
        uint64_t projectGeneration);
    friend const void* compiledProjectSnapshotImpl(
        const CompiledProjectSnapshot& snapshot);
    friend const harkenprojectschema::SegmentProgram* compiledSegmentProgram(
        const CompiledProjectSnapshot& snapshot);
};

struct ProjectInputCompileResult
{
    std::vector<ProjectInputDiagnostic> diagnostics;
    QByteArray canonicalProject;
    harkenstudio::ObjectId previewRootStableId = {};
    std::shared_ptr<CompiledProjectSnapshot> snapshot;
    std::vector<PreviewResourceInput> resources;
    std::shared_ptr<const harkenprojectschema::RuntimeIdentityIndexV1> identityIndex;
    std::shared_ptr<const harkenprojectschema::CompileTargetProfileV1>
        compileTargetProfile;
    std::shared_ptr<const harkenbuscompiler::CompiledBusResultV1> busResult;
    std::vector<LegacyPackBusDisplayRowV1> legacyPackBusDisplayRows;

    bool ok() const
    {
        return diagnostics.empty() && snapshot && snapshot->valid();
    }
};

const harkenbuscompiler::CompiledBusResultV1* compiledBusResult(
    const ProjectInputCompileResult& result);

std::vector<ProjectInputDiagnostic> validateBusPublicationSupport(
    const VNX::ProjectData& project, QUuid targetPlatform);

ProjectInputCompileResult compileStudioProjectInput(
    const VNX::ProjectData& project,
    std::optional<harkenstudio::ObjectId> previewTrackStableId = std::nullopt,
    uint64_t projectGeneration = 0);
harkenbuscompiler::PublishedBusProjectionV1 compileStudioBusProjectionForPlatform(
    const VNX::ProjectData& project, QUuid targetPlatform,
    std::vector<ProjectInputDiagnostic>* diagnostics);
const void* compiledProjectSnapshotImpl(const CompiledProjectSnapshot& snapshot);

std::shared_ptr<const harkenprojectschema::RuntimeIdentityIndexV1>
canonicalRuntimeIdentityIndexForProject(VNX::ProjectData& project);

}

namespace VNX {

std::shared_ptr<const harkenprojectschema::RuntimeIdentityIndexV1>
canonicalRuntimeIdentityIndex(ProjectData& project);

// CanonicalRuntimeProjectAdapter is the single production adapter owner. The
// Bridge aliases remain available to Preview while the implementation lives
// in Core, where ProjectData and its mutation lifecycle are defined.
using CanonicalRuntimeProjectInputV1 =
    HarkenEditor::Bridge::ProjectInputCompileResult;

class CanonicalRuntimeProjectAdapter final
{
public:
    static CanonicalRuntimeProjectInputV1 compile(
        const ProjectData& project,
        std::optional<harkenstudio::ObjectId> previewTrackStableId = std::nullopt,
        uint64_t projectGeneration = 0);
};

}
