#pragma once

#include "CurrentWorldSyncBridge.h"

#include <cstdint>

namespace harken::studio::bridge {

struct CurrentWorldTestInstallResult {
    enum class Status : std::uint8_t { Ok, Backpressure, Failed };
    Status status = Status::Ok;
    std::uint64_t resultGeneration = 0;
};

using CurrentWorldInstallBackend = CurrentWorldTestInstallResult (*)() noexcept;
using CurrentWorldDiscardObserver = void (*)(CurrentWorldSyncToken,
                                             bool runtimeActive) noexcept;

enum class ProjectGenerationTestPrepareStatus : std::uint8_t {
    Ok = 0,
    InvalidView = 1,
    Backpressure = 2,
    Capacity = 3,
    CapabilityUnavailable = 4,
    DeviceError = 5,
    InvalidWorld = 6,
};

enum class ProjectGenerationTestCommitStatus : std::uint8_t {
    Ok = 0,
    Backpressure = 1,
    InvalidPrepared = 2,
    DeviceError = 3,
};

struct ProjectGenerationTestBackend {
    ProjectGenerationTestPrepareStatus prepareStatus =
        ProjectGenerationTestPrepareStatus::Ok;
    ProjectGenerationTestCommitStatus commitStatus =
        ProjectGenerationTestCommitStatus::Ok;
    std::uint64_t installedWorldGeneration = 1;
    bool openImmediatelyOnCommitOk = true;
    bool cancelCalled = false;
    std::uint32_t prepareCalls = 0;
    std::uint32_t commitCalls = 0;
};

void setCurrentWorldInstallBackendForTesting(
    CurrentWorldInstallBackend backend) noexcept;
void setProjectGenerationTestBackendForTesting(
    ProjectGenerationTestBackend* backend) noexcept;
void setCurrentWorldDiscardObserverForTesting(
    CurrentWorldDiscardObserver observer) noexcept;
void resetCurrentWorldSyncStateForTesting() noexcept;
void servicePendingCurrentWorld() noexcept;
bool queryRuntimeLiveOwnerRevision(std::uint64_t ownerId,
                                   std::uint64_t* outRevision) noexcept;

}
