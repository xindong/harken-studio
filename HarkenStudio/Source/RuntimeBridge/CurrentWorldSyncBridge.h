#pragma once

#include "Utils/ObjectId.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace HarkenEditor::Bridge {

class CurrentWorldSyncRequest;
using CurrentWorldSyncRequestPtr = std::shared_ptr<CurrentWorldSyncRequest>;

enum class CurrentWorldSyncStatus : std::uint32_t {
    Ok = 0,
    RuntimeUnavailable,
    MediaUnavailable,
    InvalidProject,
    CompileFailed,
    InstallFailed,
    Pending,
    Superseded,
    Cancelled,
    IdentityExhausted,
};

enum class PendingCurrentWorldDisposition : std::uint32_t {
    Superseded,
    Cancelled,
};

struct CurrentWorldSyncToken {
    std::uint64_t projectSessionId = 0;
    std::uint64_t authoringRevision = 0;
    std::uint64_t runtimeEpoch = 0;
    std::uint64_t submissionSerial = 0;

    bool operator==(const CurrentWorldSyncToken& other) const noexcept {
        return projectSessionId == other.projectSessionId
            && authoringRevision == other.authoringRevision
            && runtimeEpoch == other.runtimeEpoch
            && submissionSerial == other.submissionSerial;
    }
};

struct CurrentWorldSyncResult {
    CurrentWorldSyncStatus status = CurrentWorldSyncStatus::InvalidProject;
    CurrentWorldSyncToken token;
    harkenstudio::ObjectId relatedStableId = {};
    std::uint64_t worldGeneration = 0;

    bool ok() const noexcept { return status == CurrentWorldSyncStatus::Ok; }
    bool pending() const noexcept {
        return status == CurrentWorldSyncStatus::Pending;
    }
};

CurrentWorldSyncResult syncCurrentWorld(CurrentWorldSyncRequestPtr request,
                                        CurrentWorldSyncToken token);
CurrentWorldSyncResult clearCurrentWorld(CurrentWorldSyncToken token);
std::optional<CurrentWorldSyncResult> discardPendingCurrentWorld(
    PendingCurrentWorldDisposition disposition =
        PendingCurrentWorldDisposition::Cancelled) noexcept;
std::optional<CurrentWorldSyncResult> takeCurrentWorldSyncCompletion() noexcept;

} // namespace HarkenEditor::Bridge
