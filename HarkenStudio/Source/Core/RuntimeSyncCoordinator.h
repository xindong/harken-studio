#pragma once

#include "RuntimeBridge/CurrentWorldSyncBridge.h"

#include <QObject>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace VNX {

class RuntimeSyncCoordinator final : public QObject
{
    Q_OBJECT

public:
    using ProjectSessionId = std::uint64_t;
    using AuthoringRevision = std::uint64_t;
    using RuntimeEpoch = std::uint64_t;
    using SubmissionSerial = std::uint64_t;
    using SubmitCallback = std::function<
        HarkenEditor::Bridge::CurrentWorldSyncResult(
            HarkenEditor::Bridge::CurrentWorldSyncToken)>;

    enum class AllocationState
    {
        Allocated,
        IdentityExhausted,
    };

    struct AllocationResult
    {
        AllocationState state;
        AuthoringRevision revision;
    };

    struct PropertyMutation
    {
        harkenstudio::ObjectId stableId{};
        std::uint8_t propertyId = 0;
    };

    struct MutationSet
    {
        bool full = false;
        std::vector<PropertyMutation> properties;
    };

    struct InitialIdentityForTesting
    {
        ProjectSessionId projectSessionId = 0;
        AuthoringRevision authoringRevision = 0;
        RuntimeEpoch runtimeEpoch = 0;
        SubmissionSerial submissionSerial = 0;
    };

    enum class RevisionBarrierState
    {
        Ready,
        Pending,
        Failed,
        Superseded,
        RuntimeUnavailable,
    };

    struct InstalledWorldIdentity
    {
        ProjectSessionId projectSessionId = 0;
        AuthoringRevision authoringRevision = 0;
        RuntimeEpoch runtimeEpoch = 0;
        std::uint64_t worldGeneration = 0;
        SubmissionSerial submissionSerial = 0;
    };

    struct RevisionBarrierSnapshot
    {
        RevisionBarrierState state = RevisionBarrierState::Pending;
        ProjectSessionId projectSessionId = 0;
        AuthoringRevision requiredRevision = 0;
        AuthoringRevision installedRevision = 0;
        RuntimeEpoch runtimeEpoch = 0;
        std::uint64_t worldGeneration = 0;
        SubmissionSerial submissionSerial = 0;
        HarkenEditor::Bridge::CurrentWorldSyncStatus failure =
            HarkenEditor::Bridge::CurrentWorldSyncStatus::Ok;
    };

    explicit RuntimeSyncCoordinator(SubmitCallback submit, QObject* parent = nullptr);
    RuntimeSyncCoordinator(SubmitCallback submit,
                           AuthoringRevision initialAuthoringRevision,
                           QObject* parent = nullptr);
    RuntimeSyncCoordinator(SubmitCallback submit,
                           InitialIdentityForTesting initialIdentity,
                           QObject* parent = nullptr);

    [[nodiscard]] AuthoringRevision currentAuthoringRevision() const noexcept;
    AllocationState beginProjectSession();
    AllocationState runtimeStarted();
    void runtimeStopping();
    void beginSubmissionBatch();
    void endSubmissionBatch();
    AllocationResult markCurrentWorldMutation();
    AllocationResult markCurrentWorldPropertyMutation(
        harkenstudio::ObjectId stableId, std::uint8_t propertyId);
    [[nodiscard]] const MutationSet& submittedMutations() const noexcept;
    [[nodiscard]] bool isCurrentSubmission(
        HarkenEditor::Bridge::CurrentWorldSyncToken token) const noexcept;
    void flushPendingMutationNow();
    [[nodiscard]] bool hasOutstandingSubmission() const noexcept;
    [[nodiscard]] RevisionBarrierSnapshot barrier(
        AuthoringRevision requiredRevision) const noexcept;
    [[nodiscard]] std::optional<InstalledWorldIdentity> installedWorld() const noexcept;
    [[nodiscard]] bool hasInstalledWorld() const noexcept;
    void acceptCurrentWorldSyncResult(
        const HarkenEditor::Bridge::CurrentWorldSyncResult& result);

signals:
    void barrierChanged();

private:
    AllocationResult allocateMutation();
    void flushCurrentWorldMutation();
    void failIdentityExhausted();

    QTimer m_coalescingTimer;
    SubmitCallback m_submit;
    ProjectSessionId m_projectSessionId = 0;
    AuthoringRevision m_latestAuthoringRevision = 0;
    RuntimeEpoch m_runtimeEpoch = 0;
    SubmissionSerial m_submissionSerial = 0;
    RevisionBarrierState m_barrierState = RevisionBarrierState::Pending;
    HarkenEditor::Bridge::CurrentWorldSyncStatus m_failure =
        HarkenEditor::Bridge::CurrentWorldSyncStatus::Ok;
    std::optional<HarkenEditor::Bridge::CurrentWorldSyncToken> m_outstandingToken;
    std::optional<InstalledWorldIdentity> m_installedIdentity;
    std::uint32_t m_submissionBatchDepth = 0;
    bool m_flushRequested = false;
    bool m_runtimeActive = false;
    MutationSet m_pendingMutations;
    MutationSet m_submittedMutations;
};

}
