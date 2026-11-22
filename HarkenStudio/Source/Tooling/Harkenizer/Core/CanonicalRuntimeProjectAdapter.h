#pragma once

#include "Core/PlatformContext.h"

#include <QByteArray>
#include <QString>
#include <QUuid>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <harken_bus_compiler/BusCompiler.h>

namespace harken::studio {
struct ProjectData;
}

namespace harken::studio::project_schema {
struct CompileTargetProfileV1;
class SegmentProgram;
class RuntimeIdentityIndexV1;
}

namespace harken::studio::bridge {

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
    harken::studio::types::ObjectId soundStableId = {};
    uint32_t sourceId = 0;
    QString absolutePath;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint64_t frameCount = 0;
    bool seekable = false;
};

struct LegacyPackBusDisplayRowV1
{
    harken::studio::types::ObjectId busId = {};
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

    friend ProjectInputCompileResult compileStudioProjectInputForContext(
        const harken::studio::ProjectData& project, harken::studio::PlatformScope scope, const QString& language,
        std::optional<harken::studio::types::ObjectId> previewTrackStableId,
        uint64_t projectGeneration);
    friend const void* compiledProjectSnapshotImpl(
        const CompiledProjectSnapshot& snapshot);
    friend const harken::studio::project_schema::SegmentProgram* compiledSegmentProgram(
        const CompiledProjectSnapshot& snapshot);
};

struct ProjectInputCompileResult
{
    std::vector<ProjectInputDiagnostic> diagnostics;
    QByteArray canonicalProject;
    harken::studio::types::ObjectId previewRootStableId = {};
    std::shared_ptr<CompiledProjectSnapshot> snapshot;
    std::vector<PreviewResourceInput> resources;
    std::shared_ptr<const harken::studio::project_schema::RuntimeIdentityIndexV1> identityIndex;
    std::shared_ptr<const harken::studio::project_schema::CompileTargetProfileV1>
        compileTargetProfile;
    std::shared_ptr<const harken::studio::bus_compiler::CompiledBusResultV1> busResult;
    std::vector<LegacyPackBusDisplayRowV1> legacyPackBusDisplayRows;

    bool ok() const
    {
        return diagnostics.empty() && snapshot && snapshot->valid();
    }
};

const harken::studio::bus_compiler::CompiledBusResultV1* compiledBusResult(
    const ProjectInputCompileResult& result);

std::vector<ProjectInputDiagnostic> validateBusPublicationSupport(
    const harken::studio::ProjectData& project, QUuid targetPlatform);

ProjectInputCompileResult compileStudioProjectInputForContext(
    const harken::studio::ProjectData& project, harken::studio::PlatformScope scope, const QString& language,
    std::optional<harken::studio::types::ObjectId> previewTrackStableId,
    uint64_t projectGeneration);

ProjectInputCompileResult compileStudioProjectInput(
    const harken::studio::ProjectData& project,
    std::optional<harken::studio::types::ObjectId> previewTrackStableId = std::nullopt,
    uint64_t projectGeneration = 0);
harken::studio::bus_compiler::PublishedBusProjectionV1 compileStudioBusProjectionForPlatform(
    const harken::studio::ProjectData& project, QUuid targetPlatform,
    std::vector<ProjectInputDiagnostic>* diagnostics);
const void* compiledProjectSnapshotImpl(const CompiledProjectSnapshot& snapshot);

std::shared_ptr<const harken::studio::project_schema::RuntimeIdentityIndexV1>
canonicalRuntimeIdentityIndexForProject(harken::studio::ProjectData& project);

}

namespace harken::studio {

std::shared_ptr<const harken::studio::project_schema::RuntimeIdentityIndexV1>
canonicalRuntimeIdentityIndex(ProjectData& project);

// CanonicalRuntimeProjectAdapter is the single production adapter owner. The
// Bridge aliases remain available to Preview while the implementation lives
// in Core, where ProjectData and its mutation lifecycle are defined.
using CanonicalRuntimeProjectInputV1 =
    harken::studio::bridge::ProjectInputCompileResult;

class CanonicalRuntimeProjectAdapter final
{
public:
    static CanonicalRuntimeProjectInputV1 compile(
        const ProjectData& project,
        std::optional<harken::studio::types::ObjectId> previewTrackStableId = std::nullopt,
        uint64_t projectGeneration = 0);
};

}
