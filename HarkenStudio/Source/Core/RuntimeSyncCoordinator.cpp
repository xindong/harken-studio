#include "RuntimeSyncCoordinator.h"
#include <harken_playable_properties.g.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace VNX {

RuntimeSyncCoordinator::RuntimeSyncCoordinator(SubmitCallback submit, QObject* parent)
    : RuntimeSyncCoordinator(std::move(submit), InitialIdentityForTesting{}, parent)
{
}

RuntimeSyncCoordinator::RuntimeSyncCoordinator(
    SubmitCallback submit,
    AuthoringRevision initialAuthoringRevision,
    QObject* parent)
    : RuntimeSyncCoordinator(
          std::move(submit),
          InitialIdentityForTesting{0, initialAuthoringRevision, 0, 0},
          parent)
{
}

RuntimeSyncCoordinator::RuntimeSyncCoordinator(
    SubmitCallback submit,
    InitialIdentityForTesting initialIdentity,
    QObject* parent)
    : QObject(parent),
      m_submit(std::move(submit)),
      m_projectSessionId(initialIdentity.projectSessionId),
      m_latestAuthoringRevision(initialIdentity.authoringRevision),
      m_runtimeEpoch(initialIdentity.runtimeEpoch),
      m_submissionSerial(initialIdentity.submissionSerial),
      m_runtimeActive(initialIdentity.runtimeEpoch != 0)
{
    m_coalescingTimer.setSingleShot(true);
    m_coalescingTimer.setInterval(0);
    connect(&m_coalescingTimer, &QTimer::timeout, this,
            &RuntimeSyncCoordinator::flushCurrentWorldMutation);
}

RuntimeSyncCoordinator::AuthoringRevision
RuntimeSyncCoordinator::currentAuthoringRevision() const noexcept
{
    return m_latestAuthoringRevision;
}

RuntimeSyncCoordinator::AllocationResult
RuntimeSyncCoordinator::markCurrentWorldMutation()
{
    m_pendingMutations.full = true;
    m_pendingMutations.properties.clear();
    return allocateMutation();
}

RuntimeSyncCoordinator::AllocationResult
RuntimeSyncCoordinator::markCurrentWorldPropertyMutation(
    harkenstudio::ObjectId stableId, std::uint8_t propertyId)
{
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    const auto property = static_cast<Property>(propertyId);
    if (harkenstudio::IsNull(stableId)
        || (property != Property::Volume && property != Property::Pitch
            && property != Property::MakeUpGain))
        return markCurrentWorldMutation();
    if (!m_pendingMutations.full
        && std::none_of(m_pendingMutations.properties.begin(),
                        m_pendingMutations.properties.end(),
                        [&](const auto& row) {
                            return row.stableId == stableId
                                && row.propertyId == propertyId;
                        })) {
        m_pendingMutations.properties.push_back({stableId, propertyId});
    }
    return allocateMutation();
}

const RuntimeSyncCoordinator::MutationSet&
RuntimeSyncCoordinator::submittedMutations() const noexcept
{
    return m_submittedMutations;
}

bool RuntimeSyncCoordinator::isCurrentSubmission(
    HarkenEditor::Bridge::CurrentWorldSyncToken token) const noexcept
{
    return m_runtimeActive && m_outstandingToken && token == *m_outstandingToken
        && token.projectSessionId == m_projectSessionId
        && token.runtimeEpoch == m_runtimeEpoch
        && token.authoringRevision == m_latestAuthoringRevision;
}

RuntimeSyncCoordinator::AllocationResult
RuntimeSyncCoordinator::allocateMutation()
{
    if (m_latestAuthoringRevision == std::numeric_limits<AuthoringRevision>::max())
    {
        failIdentityExhausted();
        return {AllocationState::IdentityExhausted, m_latestAuthoringRevision};
    }

    ++m_latestAuthoringRevision;
    m_flushRequested = true;
    m_barrierState = m_runtimeActive
        ? RevisionBarrierState::Pending
        : RevisionBarrierState::RuntimeUnavailable;
    m_failure = m_runtimeActive
        ? HarkenEditor::Bridge::CurrentWorldSyncStatus::Ok
        : HarkenEditor::Bridge::CurrentWorldSyncStatus::RuntimeUnavailable;
    if (m_runtimeActive && m_submissionBatchDepth == 0
        && !m_coalescingTimer.isActive())
        m_coalescingTimer.start();
    emit barrierChanged();
    return {AllocationState::Allocated, m_latestAuthoringRevision};
}

RuntimeSyncCoordinator::AllocationState
RuntimeSyncCoordinator::beginProjectSession()
{
    if (m_projectSessionId == std::numeric_limits<ProjectSessionId>::max()) {
        failIdentityExhausted();
        return AllocationState::IdentityExhausted;
    }
    ++m_projectSessionId;
    m_coalescingTimer.stop();
    m_outstandingToken.reset();
    m_installedIdentity.reset();
    m_pendingMutations = {true, {}};
    m_submittedMutations = {};
    m_flushRequested = false;
    m_barrierState = m_runtimeActive
        ? RevisionBarrierState::Pending
        : RevisionBarrierState::RuntimeUnavailable;
    m_failure = m_runtimeActive
        ? HarkenEditor::Bridge::CurrentWorldSyncStatus::Ok
        : HarkenEditor::Bridge::CurrentWorldSyncStatus::RuntimeUnavailable;
    emit barrierChanged();
    return AllocationState::Allocated;
}

RuntimeSyncCoordinator::AllocationState RuntimeSyncCoordinator::runtimeStarted()
{
    if (m_runtimeEpoch == std::numeric_limits<RuntimeEpoch>::max()) {
        failIdentityExhausted();
        return AllocationState::IdentityExhausted;
    }
    ++m_runtimeEpoch;
    m_runtimeActive = true;
    m_coalescingTimer.stop();
    m_flushRequested = false;
    m_outstandingToken.reset();
    m_installedIdentity.reset();
    m_pendingMutations = {true, {}};
    m_submittedMutations = {};
    m_barrierState = RevisionBarrierState::Pending;
    m_failure = HarkenEditor::Bridge::CurrentWorldSyncStatus::Ok;
    emit barrierChanged();
    return AllocationState::Allocated;
}

void RuntimeSyncCoordinator::beginSubmissionBatch()
{
    ++m_submissionBatchDepth;
    m_coalescingTimer.stop();
}

void RuntimeSyncCoordinator::endSubmissionBatch()
{
    if (m_submissionBatchDepth == 0)
        return;
    --m_submissionBatchDepth;
    if (m_runtimeActive && m_submissionBatchDepth == 0 && m_flushRequested
        && !m_coalescingTimer.isActive()) {
        m_coalescingTimer.start();
    }
}

bool RuntimeSyncCoordinator::hasOutstandingSubmission() const noexcept
{
    return m_outstandingToken.has_value();
}

void RuntimeSyncCoordinator::flushPendingMutationNow()
{
    if (hasOutstandingSubmission())
        return;
    if (!m_coalescingTimer.isActive() && !m_flushRequested)
        return;
    m_coalescingTimer.stop();
    flushCurrentWorldMutation();
}

void RuntimeSyncCoordinator::runtimeStopping()
{
    m_runtimeActive = false;
    m_coalescingTimer.stop();
    m_outstandingToken.reset();
    m_installedIdentity.reset();
    m_failure = HarkenEditor::Bridge::CurrentWorldSyncStatus::RuntimeUnavailable;
    m_barrierState = RevisionBarrierState::RuntimeUnavailable;
    emit barrierChanged();
}

void RuntimeSyncCoordinator::flushCurrentWorldMutation()
{
    if (m_submissionBatchDepth != 0)
        return;
    if (hasOutstandingSubmission()) {
        m_flushRequested = true;
        return;
    }
    m_flushRequested = false;
    if (m_submissionSerial == std::numeric_limits<SubmissionSerial>::max()) {
        failIdentityExhausted();
        return;
    }
    HarkenEditor::Bridge::CurrentWorldSyncToken token;
    token.projectSessionId = m_projectSessionId;
    token.authoringRevision = m_latestAuthoringRevision;
    token.runtimeEpoch = m_runtimeEpoch;
    token.submissionSerial = ++m_submissionSerial;
    m_submittedMutations = std::move(m_pendingMutations);
    m_pendingMutations = {};
    m_outstandingToken = token;
    m_barrierState = RevisionBarrierState::Pending;
    acceptCurrentWorldSyncResult(m_submit(token));
}

void RuntimeSyncCoordinator::failIdentityExhausted()
{
    m_coalescingTimer.stop();
    m_flushRequested = false;
    m_outstandingToken.reset();
    m_installedIdentity.reset();
    m_failure = HarkenEditor::Bridge::CurrentWorldSyncStatus::IdentityExhausted;
    m_barrierState = RevisionBarrierState::Failed;
    emit barrierChanged();
}

void RuntimeSyncCoordinator::acceptCurrentWorldSyncResult(
    const HarkenEditor::Bridge::CurrentWorldSyncResult& result)
{
    if (!m_outstandingToken || !(result.token == *m_outstandingToken)
        || result.token.projectSessionId != m_projectSessionId
        || result.token.runtimeEpoch != m_runtimeEpoch) {
        return;
    }
    if (result.pending())
        return;
    m_outstandingToken.reset();
    if (result.token.authoringRevision != m_latestAuthoringRevision
        && m_flushRequested) {
        m_barrierState = RevisionBarrierState::Pending;
        m_failure = HarkenEditor::Bridge::CurrentWorldSyncStatus::Ok;
        if (m_runtimeActive && m_submissionBatchDepth == 0
            && !m_coalescingTimer.isActive()) {
            m_coalescingTimer.start();
        }
        return;
    }
    if (result.ok()) {
        m_installedIdentity = InstalledWorldIdentity{
            result.token.projectSessionId,
            result.token.authoringRevision,
            result.token.runtimeEpoch,
            result.worldGeneration,
            result.token.submissionSerial};
        m_barrierState = RevisionBarrierState::Ready;
        emit barrierChanged();
        if (m_runtimeActive && m_submissionBatchDepth == 0 && m_flushRequested
            && !m_coalescingTimer.isActive()) {
            m_coalescingTimer.start();
        }
        return;
    }
    m_failure = result.status;
    m_barrierState = result.status
            == HarkenEditor::Bridge::CurrentWorldSyncStatus::Superseded
        ? RevisionBarrierState::Superseded
        : RevisionBarrierState::Failed;
    emit barrierChanged();
    if (m_runtimeActive && m_submissionBatchDepth == 0 && m_flushRequested
        && !m_coalescingTimer.isActive()) {
        m_coalescingTimer.start();
    }
}

RuntimeSyncCoordinator::RevisionBarrierSnapshot RuntimeSyncCoordinator::barrier(
    AuthoringRevision requiredRevision) const noexcept
{
    RevisionBarrierSnapshot snapshot;
    snapshot.state = m_barrierState;
    snapshot.projectSessionId = m_projectSessionId;
    snapshot.requiredRevision = requiredRevision;
    snapshot.runtimeEpoch = m_runtimeEpoch;
    snapshot.failure = m_failure;
    if (m_installedIdentity) {
        snapshot.installedRevision = m_installedIdentity->authoringRevision;
        snapshot.worldGeneration = m_installedIdentity->worldGeneration;
        snapshot.submissionSerial = m_installedIdentity->submissionSerial;
        if (snapshot.state == RevisionBarrierState::Ready
            && (m_installedIdentity->authoringRevision < requiredRevision
            || m_installedIdentity->authoringRevision
                != m_latestAuthoringRevision)) {
            snapshot.state = RevisionBarrierState::Pending;
        }
    }
    return snapshot;
}

std::optional<RuntimeSyncCoordinator::InstalledWorldIdentity>
RuntimeSyncCoordinator::installedWorld() const noexcept
{
    return m_installedIdentity;
}

bool RuntimeSyncCoordinator::hasInstalledWorld() const noexcept
{
    return m_installedIdentity.has_value()
        && (m_installedIdentity->worldGeneration != 0
            || m_installedIdentity->authoringRevision != 0);
}

}
