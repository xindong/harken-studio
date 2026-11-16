#include "harken/core/TimelinePlanner.h"

#include <algorithm>
#include <cstdint>

namespace harken {

bool TimelinePlanner::Reset(const Config& config, ScheduledTransactionPool* pool,
                            DeferredIntentPool* deferredPool,
                            const PlayableInstanceTable* instances,
                            const OccurrenceStatePool* occurrences,
                            TimelineArmTable* armTable) {
    if (pool == nullptr || !pool->Valid() || deferredPool == nullptr ||
        (deferredPool->Count() != 0 || deferredPool->PreparedCount() != 0) ||
        instances == nullptr) {
        return false;
    }
    *this = TimelinePlanner{}; // all preconditions passed; replace prior state

    config_ = config;
    pool_ = pool;
    deferredPool_ = deferredPool;
    instances_ = instances;
    occurrences_ = occurrences;
    armTable_ = armTable;

    const std::size_t deferredCapacity = deferredPool->Capacity();
    deferredProgram_.assign(deferredCapacity, nullptr);
    deferredInput_.assign(deferredCapacity, PostInput{});
    deferredSlotUsed_.assign(deferredCapacity, 0);
    const std::size_t rollingCapacity =
        config.maxRollingOccurrences == 0
            ? deferredCapacity
            : std::min<std::size_t>(config.maxRollingOccurrences,
                                    deferredCapacity);
    rolling_.assign(rollingCapacity, RollingCursor{});
    scratchEvents_.assign(pool->MaxTransactionEvents(), ScheduledEventInput{});
    staleClaimScratch_.clear();
    staleClaimScratch_.reserve(deferredCapacity);

    materializationsRetried_ = 0;
    materializationReserveFailures_ = 0;
    trimmedFadeOutGroups_ = 0;

    const std::uint64_t bootstrapCutoff =
        pool_->WindowStartFrame() + pool_->QuantumFrames();
    pool_->Publish(bootstrapCutoff, instances_, occurrences_, armTable_);

    valid_ = true;
    return true;
}

bool TimelinePlanner::ResetForTransport() noexcept {
    if (!valid_ || pool_ == nullptr || !pool_->Valid() ||
        deferredPool_ == nullptr || deferredPool_->Count() != 0 ||
        deferredPool_->PreparedCount() != 0 ||
        instances_ == nullptr ||
        deferredProgram_.size() != deferredPool_->Capacity() ||
        deferredInput_.size() != deferredPool_->Capacity() ||
        deferredSlotUsed_.size() != deferredPool_->Capacity() ||
        scratchEvents_.size() != pool_->MaxTransactionEvents()) {
        return false;
    }
    std::fill(deferredProgram_.begin(), deferredProgram_.end(), nullptr);
    std::fill(deferredInput_.begin(), deferredInput_.end(), PostInput{});
    std::fill(deferredSlotUsed_.begin(), deferredSlotUsed_.end(), 0);
    committedDeferredProgramCount_ = 0;
    std::fill(rolling_.begin(), rolling_.end(), RollingCursor{});
    std::fill(scratchEvents_.begin(), scratchEvents_.end(),
              ScheduledEventInput{});
    staleClaimScratch_.clear();
    rollingEventScratch_ = {};
    materializationsRetried_ = 0;
    materializationReserveFailures_ = 0;
    trimmedFadeOutGroups_ = 0;
    const std::uint64_t bootstrapCutoff =
        pool_->WindowStartFrame() + pool_->QuantumFrames();
    pool_->Publish(bootstrapCutoff, instances_, occurrences_, armTable_);
    return true;
}

ReserveResult TimelinePlanner::ReserveAt(const ProgramView& program,
                                         std::uint64_t entryAnchor,
                                         TransportEpoch epoch,
                                         PrePublishLatePolicy policy,
                                         std::uint64_t* outFirst,
                                         std::uint64_t* outLast,
                                         bool trimBeforeFrontier,
                                         std::uint64_t publicationFrontier) {
    if (publicationFrontier == UINT64_MAX) {
        publicationFrontier = pool_->PublishCutoffFrame();
    }
    std::size_t eventCount = 0;
    for (std::size_t i = 0; i < program.eventCount; ++i) {
        const EventTemplate& t = program.events[i];
        const std::uint64_t frame =
            AnchoredFrame(entryAnchor, t.frameOffsetFromEntry);
        if (trimBeforeFrontier && frame < publicationFrontier && !t.frontier.Present()) {
            continue;
        }
        ScheduledEventInput& e = scratchEvents_[eventCount++];
        e.executeFrame = frame;
        e.logicCommitSequence = t.logicCommitSequence;
        e.executionOrdinal = t.executionOrdinal;
        e.eventIndex = t.eventIndex;
        e.transactionOrdinal = t.transactionOrdinal;
        e.subeventPhase = t.subeventPhase;
        e.payload = t.payload;
        e.authoredFadeOutCandidate = t.authoredFadeOutCandidate;
        e.armToken = t.armToken;
        e.frontier = t.frontier;
    }
    if (eventCount == 0) {
        ReserveResult empty{};
        empty.status = ReserveStatus::DroppedLate;
        empty.transactionId = program.transactionId;
        *outFirst = 0;
        *outLast = 0;
        return empty;
    }
    TransactionRequest req;
    req.transactionId = program.transactionId;
    req.transportEpoch = epoch;
    req.ownerInstanceHandle = program.ownerInstance;
    req.ownerInstanceScheduleEpoch = program.ownerInstanceScheduleEpoch;
    req.occurrence = program.occurrence;
    req.program = program.runtimeProgram;
    req.armToken = program.armToken;
    req.immediateEntry = program.immediateEntry;
    req.events = scratchEvents_.data();
    req.eventCount = eventCount;
    const ReserveResult r = pool_->Reserve(req, policy, publicationFrontier);
    *outFirst = r.firstFrame;
    *outLast = r.lastFrame;
    return r;
}

void TimelinePlanner::ReleaseDeferredSlot(std::uint64_t payload) {
    const std::size_t slot = static_cast<std::size_t>(payload);
    if (slot >= deferredProgram_.size()) {
        return;
    }
    if (deferredSlotUsed_[slot] == 1) --committedDeferredProgramCount_;
    deferredSlotUsed_[slot] = 0;
    deferredProgram_[slot] = nullptr;
    deferredInput_[slot] = PostInput{};
}

PostResult TimelinePlanner::FileProgram(const ProgramView& program,
                                        std::uint64_t entryAnchor,
                                        const PostInput& input,
                                        bool countAsReschedule,
                                        bool trimBeforeFrontier,
                                        std::uint64_t publicationFrontier) {
    if (publicationFrontier == UINT64_MAX) {
        publicationFrontier = pool_->PublishCutoffFrame();
    }
    PostResult res;
    res.transactionId = program.transactionId;
    std::uint64_t first = 0;
    std::uint64_t last = 0;
    const ReserveResult r = ReserveAt(program, entryAnchor, input.transportEpoch,
                                      input.latePolicy, &first, &last,
                                      trimBeforeFrontier, publicationFrontier);
    switch (r.status) {
        case ReserveStatus::Committed:
            res.status = (countAsReschedule || r.rescheduled)
                             ? PostStatus::Rescheduled
                             : PostStatus::Reserved;
            res.firstFrame = r.firstFrame;
            res.lastFrame = r.lastFrame;
            res.committed = r.committed;
            return res;
        case ReserveStatus::DroppedLate:
            res.status = PostStatus::Dropped;
            return res;
        case ReserveStatus::Refused:
            if (r.refusal == ReserveRefusal::NeedsBoundaryResolution) {
                // NextEquivalentBoundary: the pool cannot resolve a musical
                // boundary, so the planner does it through the marker grid.
                return ResolveBoundary(program, input, publicationFrontier);
            }
            res.status = PostStatus::Refused;
            res.refusal = r.refusal;
            return res;
    }
    return res;
}

PostResult TimelinePlanner::ResolveBoundary(
    const ProgramView& program, const PostInput& input,
    std::uint64_t publicationFrontier) {
    PostResult res;
    res.transactionId = program.transactionId;

    auto applyMissing = [&]() {
        if (input.missingBoundaryPolicy == MissingBoundaryPolicy::Drop) {
            res.status = PostStatus::Dropped;
        } else {
            res.status = PostStatus::Refused;
            res.refusal = ReserveRefusal::Late;
        }
        return res;
    };

    if (program.clock == nullptr || program.markerIndex == nullptr) {
        return applyMissing(); // no grid to resolve against
    }
    // Express the frontier in the segment's own entry-relative position, find the
    // next grid line strictly after it, then place the play's entry on that
    // boundary's absolute frame.
    const std::int64_t entryRelFrontier =
        static_cast<std::int64_t>(publicationFrontier) -
        static_cast<std::int64_t>(program.entryAnchorFrame);
    const std::int64_t nativePos =
        (program.timeType == SegmentTimeType::Musical)
            ? program.clock->FrameToTick(SampleFrame{entryRelFrontier}).value
            : program.clock->FrameToTime(SampleFrame{entryRelFrontier}).value;
    const BoundaryCandidate boundary =
        program.markerIndex->NextBoundary(nativePos, program.boundaryType);
    if (!boundary.hasCandidate) {
        return applyMissing();
    }
    const std::int64_t absolute =
        static_cast<std::int64_t>(program.entryAnchorFrame) + boundary.frame.value;
    const std::uint64_t newAnchor =
        absolute < 0 ? 0 : static_cast<std::uint64_t>(absolute);

    // Keep the resolved entry on the boundary. Preroll before the publish
    // frontier is unavailable and is omitted; the remaining body is never
    // shifted to make room for it.
    PostInput shifted = input;
    shifted.latePolicy = PrePublishLatePolicy::Refuse;
    return FileProgram(program, newAnchor, shifted, /*countAsReschedule=*/true,
                       /*trimBeforeFrontier=*/true, publicationFrontier);
}

PostResult TimelinePlanner::PostPlay(const ProgramView& program,
                                     const PostInput& input) {
    PostResult res;
    res.transactionId = program.transactionId;
    if (!valid_) {
        return res;
    }
    if (program.rollingGroups) {
        return PostRolling(program, input);
    }

    const std::int64_t decisionFrame =
        static_cast<std::int64_t>(program.entryAnchorFrame) -
        program.decisionLeadFrames;
    const bool lateDecision =
        static_cast<std::int64_t>(input.currentFrame) > decisionFrame;

    if (program.eventCount == 0 || program.events == nullptr) {
        res.status = PostStatus::Refused;
        res.refusal = ReserveRefusal::EmptyTransaction;
        res.lateDecision = lateDecision;
        return res;
    }
    if (program.eventCount > scratchEvents_.size()) {
        res.status = PostStatus::Refused;
        res.refusal = ReserveRefusal::TransactionEventsExceeded;
        res.lateDecision = lateDecision;
        return res;
    }

    std::uint64_t firstFrame =
        AnchoredFrame(program.entryAnchorFrame, program.events[0].frameOffsetFromEntry);
    std::uint64_t lastFrame = firstFrame;
    for (std::size_t i = 1; i < program.eventCount; ++i) {
        const std::uint64_t frame =
            AnchoredFrame(program.entryAnchorFrame,
                          program.events[i].frameOffsetFromEntry);
        firstFrame = std::min(firstFrame, frame);
        lastFrame = std::max(lastFrame, frame);
    }
    const std::uint64_t span = lastFrame - firstFrame;
    const std::uint64_t windowEnd = pool_->WindowEndFrame();
    const std::uint64_t frontier = pool_->PublishCutoffFrame();
    const std::uint64_t writableWindow =
        frontier < windowEnd ? windowEnd - frontier : 0;
    if (span > pool_->MaxTransactionSpanFrames() || span >= writableWindow) {
        res.status = PostStatus::Refused;
        res.refusal = ReserveRefusal::SpanExceeded;
        res.lateDecision = lateDecision;
        return res;
    }

    if (lastFrame >= pool_->WindowEndFrame()) {
        PreparedDeferredPost prepared{};
        res = PrepareDeferredPlay(program, input, &prepared);
        if (res.status == PostStatus::Deferred &&
            !CommitPreparedDeferredPlay(&prepared)) {
            (void)AbandonPreparedDeferredPlay(&prepared);
            res.status = PostStatus::Refused;
            res.refusal = ReserveRefusal::PoolFull;
        }
        return res;
    }

    res = FileProgram(program, program.entryAnchorFrame, input,
                      /*countAsReschedule=*/false);
    res.lateDecision = lateDecision;
    return res;
}

PostResult TimelinePlanner::PrepareDeferredPlay(
    const ProgramView& program, const PostInput& input,
    PreparedDeferredPost* out) {
    PostResult res{};
    res.transactionId = program.transactionId;
    if (out == nullptr || out->Valid() || !valid_ || program.rollingGroups ||
        program.events == nullptr || program.eventCount == 0 ||
        program.eventCount > scratchEvents_.size()) {
        res.refusal = ReserveRefusal::EmptyTransaction;
        return res;
    }
    const std::int64_t decisionFrame =
        static_cast<std::int64_t>(program.entryAnchorFrame) -
        program.decisionLeadFrames;
    res.lateDecision =
        static_cast<std::int64_t>(input.currentFrame) > decisionFrame;
    std::uint64_t firstFrame = AnchoredFrame(
        program.entryAnchorFrame, program.events[0].frameOffsetFromEntry);
    std::uint64_t lastFrame = firstFrame;
    for (std::size_t index = 1; index < program.eventCount; ++index) {
        const std::uint64_t frame = AnchoredFrame(
            program.entryAnchorFrame,
            program.events[index].frameOffsetFromEntry);
        firstFrame = std::min(firstFrame, frame);
        lastFrame = std::max(lastFrame, frame);
    }
    const std::uint64_t frontier = pool_->PublishCutoffFrame();
    const std::uint64_t windowEnd = pool_->WindowEndFrame();
    const std::uint64_t writableWindow =
        frontier < windowEnd ? windowEnd - frontier : 0;
    if (lastFrame < windowEnd) {
        res.refusal = ReserveRefusal::OutsideWindow;
        return res;
    }
    if (lastFrame - firstFrame > pool_->MaxTransactionSpanFrames() ||
        lastFrame - firstFrame >= writableWindow) {
        res.refusal = ReserveRefusal::SpanExceeded;
        return res;
    }
    std::size_t slot = deferredProgram_.size();
    for (std::size_t index = 0; index < deferredSlotUsed_.size(); ++index) {
        if (deferredSlotUsed_[index] == 0) {
            slot = index;
            break;
        }
    }
    if (slot == deferredProgram_.size()) {
        res.refusal = ReserveRefusal::PoolFull;
        return res;
    }
    DeferredIntent intent{};
    intent.ownerDomain = DeferredIntentOwnerDomain::Playable;
    intent.ownerHandle = program.ownerInstance;
    intent.ownerEpoch = program.ownerInstanceScheduleEpoch;
    intent.dueFrame = lastFrame;
    intent.payload = static_cast<std::uint64_t>(slot);
    intent.commitSequence = input.logicCommitSequence;
    const DeferredIntentAddResult added = deferredPool_->AddPrepared(intent);
    if (added.status != DeferredIntentRefusal::None) {
        res.refusal = ReserveRefusal::PoolFull;
        return res;
    }
    deferredProgram_[slot] = &program;
    deferredInput_[slot] = input;
    deferredSlotUsed_[slot] = 2;
    *out = {added.token, static_cast<std::uint32_t>(slot)};
    res.status = PostStatus::Deferred;
    return res;
}

bool TimelinePlanner::CommitPreparedDeferredPlay(
    PreparedDeferredPost* prepared) {
    if (prepared == nullptr || !prepared->Valid() ||
        prepared->slot >= deferredSlotUsed_.size() ||
        deferredSlotUsed_[prepared->slot] != 2 ||
        !deferredPool_->CommitPrepared(prepared->intent)) {
        return false;
    }
    deferredSlotUsed_[prepared->slot] = 1;
    ++committedDeferredProgramCount_;
    *prepared = {};
    return true;
}

bool TimelinePlanner::AbandonPreparedDeferredPlay(
    PreparedDeferredPost* prepared) {
    if (prepared == nullptr || !prepared->Valid() ||
        prepared->slot >= deferredSlotUsed_.size() ||
        deferredSlotUsed_[prepared->slot] != 2 ||
        !deferredPool_->AbandonPrepared(prepared->intent)) {
        return false;
    }
    ReleaseDeferredSlot(prepared->slot);
    *prepared = {};
    return true;
}

PostResult TimelinePlanner::FileRollingGroup(
    RollingCursor& cursor, RollingGroup group, bool materialization,
    bool* trimmedFadeOut, std::uint64_t publicationFrontier) {
    const ProgramView& source = *cursor.program;
    std::size_t first = source.eventCount;
    std::size_t last = source.eventCount;
    for (std::size_t i = 0; i < source.eventCount; ++i) {
        const std::uint32_t phase = source.events[i].subeventPhase;
        const bool selected =
            group == RollingGroup::Start
                ? phase <= 30 &&
                      AnchoredFrame(source.entryAnchorFrame,
                                    source.events[i].frameOffsetFromEntry) ==
                          AnchoredFrame(source.entryAnchorFrame,
                                        source.events[0].frameOffsetFromEntry)
                : group == RollingGroup::FadeOut
                      ? phase == 30 &&
                            AnchoredFrame(
                                source.entryAnchorFrame,
                                source.events[i].frameOffsetFromEntry) !=
                                AnchoredFrame(
                                    source.entryAnchorFrame,
                                    source.events[0].frameOffsetFromEntry)
                      : phase == 50;
        if (!selected) {
            continue;
        }
        if (first == source.eventCount) {
            first = i;
        }
        last = i + 1;
    }
    if (first == source.eventCount) {
        if (group == RollingGroup::FadeOut) {
            ++trimmedFadeOutGroups_;
            cursor.authoredFadeOutPublished = false;
            cursor.next = RollingGroup::Stop;
            if (trimmedFadeOut != nullptr) {
                *trimmedFadeOut = true;
            }
            return FileRollingGroup(cursor, RollingGroup::Stop,
                                    materialization, nullptr,
                                    publicationFrontier);
        }
        PostResult empty{};
        empty.status = PostStatus::Dropped;
        empty.transactionId = source.transactionId;
        return empty;
    }
    ProgramView groupView = source;
    groupView.events = source.events + first;
    groupView.eventCount = last - first;
    if (group == RollingGroup::Stop && groupView.eventCount == 1) {
        rollingEventScratch_ = *groupView.events;
        rollingEventScratch_.authoredFadeOutCandidate =
            cursor.authoredFadeOutPublished;
        groupView.events = &rollingEventScratch_;
    }
    groupView.transactionId =
        source.transactionId * 3u + static_cast<std::uint32_t>(group) + 1u;
    return FileProgram(groupView, groupView.entryAnchorFrame, cursor.input,
                       materialization, true, publicationFrontier);
}

bool TimelinePlanner::AdvanceRollingCursor(RollingCursor& cursor,
                                            DeferredIntentToken claimToken,
                                            PostStatus status) {
    if (status == PostStatus::Refused) {
        return false;
    }
    RollingGroup next = cursor.next == RollingGroup::Start
                            ? (cursor.fadeOutStartFrame != 0
                                   ? RollingGroup::FadeOut
                                   : RollingGroup::Stop)
                            : cursor.next == RollingGroup::FadeOut
                                  ? RollingGroup::Stop
                                  : RollingGroup::Done;
    if (next == RollingGroup::Done) {
        if (claimToken.Valid()) {
            (void)deferredPool_->Release(claimToken);
        }
        cursor = RollingCursor{};
        return true;
    }
    if (next == RollingGroup::FadeOut) {
        bool hasFadeOut = false;
        const std::uint64_t startFrame = AnchoredFrame(
            cursor.program->entryAnchorFrame,
            cursor.program->events[0].frameOffsetFromEntry);
        for (std::size_t i = 0; i < cursor.program->eventCount; ++i) {
            hasFadeOut = hasFadeOut ||
                         (cursor.program->events[i].subeventPhase == 30 &&
                          AnchoredFrame(cursor.program->entryAnchorFrame,
                                        cursor.program->events[i]
                                            .frameOffsetFromEntry) != startFrame);
        }
        if (!hasFadeOut) {
            next = RollingGroup::Stop;
        }
    }
    std::uint64_t dueFrame = 0;
    for (std::size_t i = 0; i < cursor.program->eventCount; ++i) {
        const std::uint32_t phase = cursor.program->events[i].subeventPhase;
        if ((next == RollingGroup::FadeOut && phase == 30) ||
            (next == RollingGroup::Stop && phase == 50)) {
            dueFrame = AnchoredFrame(
                cursor.program->entryAnchorFrame,
                cursor.program->events[i].frameOffsetFromEntry);
            break;
        }
    }
    DeferredIntent intent{};
    intent.ownerDomain = DeferredIntentOwnerDomain::Playable;
    intent.ownerHandle = cursor.program->ownerInstance;
    intent.ownerEpoch = cursor.program->ownerInstanceScheduleEpoch;
    intent.dueFrame = dueFrame;
    intent.payload = static_cast<std::uint64_t>(&cursor - rolling_.data());
    intent.commitSequence = cursor.input.logicCommitSequence;
    DeferredIntentAddResult replacement =
        claimToken.Valid()
            ? deferredPool_->Replace(claimToken, intent)
            : (cursor.deferred.Valid()
                   ? DeferredIntentAddResult{DeferredIntentRefusal::None,
                                             cursor.deferred}
                   : deferredPool_->AddClaimable(intent));
    if (replacement.status != DeferredIntentRefusal::None) {
        if (claimToken.Valid()) {
            (void)deferredPool_->Release(claimToken);
        }
        cursor.deferred = {};
        cursor.state = RollingState::Aborted;
        if (armTable_ != nullptr && cursor.armToken.Valid()) {
            (void)armTable_->RequestCancel(cursor.armToken);
        }
        return false;
    }
    cursor.next = next;
    cursor.deferred = replacement.token;
    return true;
}

PostResult TimelinePlanner::PostRolling(const ProgramView& program,
                                        const PostInput& input) {
    PostResult refused{};
    refused.status = PostStatus::Refused;
    refused.refusal = ReserveRefusal::PoolFull;
    refused.transactionId = program.transactionId;
    RollingCursor* cursor = nullptr;
    for (RollingCursor& candidate : rolling_) {
        if (candidate.state == RollingState::Free) {
            cursor = &candidate;
            break;
        }
    }
    if (cursor == nullptr) {
        return refused;
    }
    *cursor = RollingCursor{};
    cursor->state = RollingState::AwaitingWindow;
    cursor->program = &program;
    cursor->input = input;
    cursor->programGeneration = program.runtimeProgram.programGeneration;
    cursor->occurrence = program.occurrence;
    cursor->armToken = program.armToken;
    cursor->next = RollingGroup::Start;
    for (std::size_t i = 0; i < program.eventCount; ++i) {
        const EventTemplate& event = program.events[i];
        const std::uint64_t frame =
            AnchoredFrame(program.entryAnchorFrame,
                          event.frameOffsetFromEntry);
        if (event.subeventPhase == 20) {
            cursor->startFrame = frame;
        } else if (event.subeventPhase == 30 &&
                   frame != cursor->startFrame) {
            cursor->fadeOutStartFrame = frame;
            cursor->fadeOutEnvelopeIndex =
                static_cast<std::uint32_t>(event.payload);
        } else if (event.subeventPhase == 50) {
            cursor->stopFrame = frame;
            cursor->fadeOutEndFrame = frame;
        }
    }
    cursor->next = cursor->fadeOutStartFrame != 0
                       ? RollingGroup::FadeOut
                       : RollingGroup::Stop;
    DeferredIntent intent{};
    intent.ownerDomain = DeferredIntentOwnerDomain::Playable;
    intent.ownerHandle = program.ownerInstance;
    intent.ownerEpoch = program.ownerInstanceScheduleEpoch;
    intent.dueFrame = cursor->next == RollingGroup::FadeOut
                          ? cursor->fadeOutStartFrame
                          : cursor->stopFrame;
    intent.payload = static_cast<std::uint64_t>(cursor - rolling_.data());
    intent.commitSequence = input.logicCommitSequence;
    const DeferredIntentAddResult future =
        deferredPool_->AddClaimable(intent);
    if (future.status != DeferredIntentRefusal::None) {
        *cursor = RollingCursor{};
        return refused;
    }
    cursor->deferred = future.token;
    cursor->state = RollingState::Publishing;
    const RollingGroup futureGroup = cursor->next;
    cursor->next = RollingGroup::Start;
    const PostResult start =
        FileRollingGroup(*cursor, RollingGroup::Start, false);
    if (start.status == PostStatus::Refused ||
        start.status == PostStatus::Dropped) {
        (void)deferredPool_->Cancel(cursor->deferred);
        *cursor = RollingCursor{};
        return start;
    }
    cursor->next = futureGroup;
    cursor->state = RollingState::AwaitingWindow;
    return start;
}

bool TimelinePlanner::HasLiveProgramGeneration(
    ProgramGeneration generation) const {
    for (const RollingCursor& cursor : rolling_) {
        if (cursor.state != RollingState::Free &&
            cursor.programGeneration == generation) {
            return true;
        }
    }
    return false;
}

bool TimelinePlanner::HasProgramReference(
    TimelineTransactionProgramRef program) const noexcept {
    if (program.program == nullptr || program.programGeneration == 0) return false;
    const auto matches = [&](const ProgramView* candidate) {
        return candidate != nullptr && candidate->runtimeProgram.program == program.program &&
            candidate->runtimeProgram.programGeneration == program.programGeneration;
    };
    for (std::size_t index = 0; index < deferredProgram_.size(); ++index) {
        if (deferredSlotUsed_[index] != 0 && matches(deferredProgram_[index])) return true;
    }
    for (const auto& cursor : rolling_) {
        if (cursor.state != RollingState::Free && matches(cursor.program)) return true;
    }
    return false;
}

std::uint32_t TimelinePlanner::LiveRollingCountForTests() const {
    std::uint32_t count = 0;
    for (const RollingCursor& cursor : rolling_) {
        if (cursor.state != RollingState::Free) {
            ++count;
        }
    }
    return count;
}

bool TimelinePlanner::SetRollingDeferredGenerationForTests(
    TimelineArmToken armToken, std::uint64_t generation) {
    for (RollingCursor& cursor : rolling_) {
        if (cursor.state == RollingState::Free ||
            cursor.armToken.slot != armToken.slot ||
            cursor.armToken.generation != armToken.generation) {
            continue;
        }
        DeferredIntentToken updated{};
        if (!deferredPool_->SetTokenGenerationForTests(
                cursor.deferred, generation, &updated)) {
            return false;
        }
        cursor.deferred = updated;
        return true;
    }
    return false;
}

bool TimelinePlanner::RollingAbortPending(TimelineArmToken armToken) const {
    for (const RollingCursor& cursor : rolling_) {
        if (cursor.state == RollingState::Aborted &&
            cursor.armToken.slot == armToken.slot &&
            cursor.armToken.generation == armToken.generation) {
            return true;
        }
    }
    return false;
}

bool TimelinePlanner::AbortRollingOccurrence(TimelineArmToken armToken) {
    for (RollingCursor& cursor : rolling_) {
        if (cursor.state == RollingState::Free ||
            cursor.armToken.slot != armToken.slot ||
            cursor.armToken.generation != armToken.generation) {
            continue;
        }
        if (cursor.deferred.Valid()) {
            (void)deferredPool_->Cancel(cursor.deferred);
            cursor.deferred = {};
        }
        cursor.state = RollingState::Aborted;
        return true;
    }
    return false;
}

bool TimelinePlanner::CompleteRollingAbort(TimelineArmToken armToken) {
    if (armTable_ != nullptr) {
        ArmState armState = ArmState::Free;
        if (armTable_->State(armToken, &armState) &&
            armState != ArmState::Tombstoned) {
            return false;
        }
    }
    for (RollingCursor& cursor : rolling_) {
        if (cursor.state != RollingState::Aborted ||
            cursor.armToken.slot != armToken.slot ||
            cursor.armToken.generation != armToken.generation) {
            continue;
        }
        cursor = RollingCursor{};
        return true;
    }
    return false;
}

void TimelinePlanner::DropStaleDeferred() {
    if (!valid_) return;
    staleClaimScratch_.clear();
    deferredPool_->DropStale(*instances_, nullptr, &staleClaimScratch_);
    ReleaseStaleDeferredClaims();
}

std::uint32_t TimelinePlanner::ClaimDeferredDue(
    std::uint64_t windowEndFrame, DeferredIntentClaim* out,
    std::uint32_t capacity, std::uint32_t* staleDropped,
    std::uint32_t* dueCount, std::uint64_t currentQuantum) {
    if (!valid_) {
        if (staleDropped != nullptr) *staleDropped = 0;
        if (dueCount != nullptr) *dueCount = 0;
        return 0;
    }
    staleClaimScratch_.clear();
    const auto claimed = deferredPool_->ClaimDue(windowEndFrame, *instances_,
        out, capacity, staleDropped, dueCount, currentQuantum, &staleClaimScratch_);
    ReleaseStaleDeferredClaims();
    return claimed;
}

void TimelinePlanner::ReleaseStaleDeferredClaims() {
    for (const auto& claim : staleClaimScratch_) {
        const auto& intent = claim.intent;
        const std::size_t slot = static_cast<std::size_t>(intent.payload);
        if (slot < rolling_.size() &&
            rolling_[slot].state != RollingState::Free &&
            rolling_[slot].state != RollingState::Aborted &&
            rolling_[slot].deferred.slot == claim.token.slot &&
            rolling_[slot].deferred.generation == claim.token.generation) {
            if (armTable_ != nullptr && rolling_[slot].armToken.Valid()) {
                (void)armTable_->RequestCancel(rolling_[slot].armToken);
            }
            rolling_[slot].deferred = {};
            rolling_[slot].state = RollingState::Aborted;
        } else if (slot < deferredProgram_.size() && deferredSlotUsed_[slot] == 1 &&
                   deferredProgram_[slot] != nullptr &&
                   deferredProgram_[slot]->ownerInstance == intent.ownerHandle &&
                   deferredProgram_[slot]->ownerInstanceScheduleEpoch == intent.ownerEpoch) {
            ReleaseDeferredSlot(intent.payload);
        }
    }
}

TimelineDeferredMaterializeStatus TimelinePlanner::MaterializeDeferredClaim(
    const DeferredIntentClaim& claim, TransportEpoch transportEpoch,
    std::uint64_t cutoffFrame) {
    if (!valid_ ||
        claim.intent.ownerDomain != DeferredIntentOwnerDomain::Playable) {
        return TimelineDeferredMaterializeStatus::NotOwned;
    }
    const DeferredIntent& intent = claim.intent;
    const std::size_t slot = static_cast<std::size_t>(intent.payload);
    if (slot < rolling_.size() &&
        rolling_[slot].state != RollingState::Free &&
        rolling_[slot].state != RollingState::Aborted &&
        rolling_[slot].deferred.slot == claim.token.slot &&
        rolling_[slot].deferred.generation == claim.token.generation) {
        RollingCursor& cursor = rolling_[slot];
        cursor.state = RollingState::Publishing;
        if (cursor.input.transportEpoch != transportEpoch) {
            (void)deferredPool_->Release(claim.token);
            cursor = RollingCursor{};
            return TimelineDeferredMaterializeStatus::Consumed;
        }
        bool trimmedFadeOut = false;
        const PostResult rollingResult =
            FileRollingGroup(cursor, cursor.next, true, &trimmedFadeOut,
                             cutoffFrame);
        if (rollingResult.status == PostStatus::Refused) {
            if (IsTransientReserveRefusal(rollingResult.refusal)) {
                cursor.state = RollingState::AwaitingWindow;
                ++materializationsRetried_;
                return TimelineDeferredMaterializeStatus::Retry;
            }
            ++materializationReserveFailures_;
            (void)deferredPool_->Release(claim.token);
            cursor.deferred = {};
            cursor.state = RollingState::Aborted;
            if (armTable_ != nullptr && cursor.armToken.Valid()) {
                (void)armTable_->RequestCancel(cursor.armToken);
            }
            return TimelineDeferredMaterializeStatus::Consumed;
        }
        if (cursor.next == RollingGroup::FadeOut &&
            (rollingResult.status == PostStatus::Reserved ||
             rollingResult.status == PostStatus::Rescheduled)) {
            cursor.authoredFadeOutPublished = true;
        }
        if (trimmedFadeOut &&
            (rollingResult.status == PostStatus::Reserved ||
             rollingResult.status == PostStatus::Rescheduled ||
             rollingResult.status == PostStatus::Dropped)) {
            (void)deferredPool_->Release(claim.token);
            cursor = RollingCursor{};
            return TimelineDeferredMaterializeStatus::Consumed;
        }
        if (AdvanceRollingCursor(cursor, claim.token,
                                 rollingResult.status) &&
            cursor.state != RollingState::Free) {
            cursor.state = RollingState::AwaitingWindow;
        }
        return TimelineDeferredMaterializeStatus::Consumed;
    }
    if (slot >= deferredProgram_.size() || deferredSlotUsed_[slot] != 1 ||
        deferredProgram_[slot] == nullptr) {
        (void)deferredPool_->Release(claim.token);
        return TimelineDeferredMaterializeStatus::Consumed;
    }
    const ProgramView& program = *deferredProgram_[slot];
    const PostInput input = deferredInput_[slot];
    if (input.transportEpoch != transportEpoch) {
        (void)deferredPool_->Release(claim.token);
        ReleaseDeferredSlot(intent.payload);
        return TimelineDeferredMaterializeStatus::Consumed;
    }
    const PostResult result =
        FileProgram(program, program.entryAnchorFrame, input,
                    /*countAsReschedule=*/false,
                    /*trimBeforeFrontier=*/false, cutoffFrame);
    if (result.status == PostStatus::Refused &&
        IsTransientReserveRefusal(result.refusal)) {
        ++materializationsRetried_;
        return TimelineDeferredMaterializeStatus::Retry;
    }
    if (result.status == PostStatus::Refused) {
        ++materializationReserveFailures_;
    }
    (void)deferredPool_->Release(claim.token);
    ReleaseDeferredSlot(intent.payload);
    return TimelineDeferredMaterializeStatus::Consumed;
}

std::uint64_t TimelinePlanner::DeferredBlockerBefore(
    std::uint64_t cutoffFrame) const {
    if (!valid_) {
        return UINT64_MAX;
    }
    std::uint64_t blocker =
        deferredPool_->EarliestHeldDueBefore(cutoffFrame);
    if (committedDeferredProgramCount_ == 0) return blocker;
    for (std::size_t slot = 0; slot < deferredProgram_.size(); ++slot) {
        if (deferredSlotUsed_[slot] != 1 || deferredProgram_[slot] == nullptr) {
            continue;
        }
        const ProgramView& program = *deferredProgram_[slot];
        if (program.events == nullptr || program.eventCount == 0) {
            continue;
        }
        std::uint64_t firstFrame = AnchoredFrame(
            program.entryAnchorFrame,
            program.events[0].frameOffsetFromEntry);
        for (std::size_t event = 1; event < program.eventCount; ++event) {
            firstFrame = std::min(
                firstFrame,
                AnchoredFrame(program.entryAnchorFrame,
                              program.events[event].frameOffsetFromEntry));
        }
        if (firstFrame < cutoffFrame) {
            blocker = std::min(blocker, firstFrame);
        }
    }
    return blocker;
}

bool TimelinePlanner::ReclaimConsumedQuantum() {
    if (!valid_) {
        return false;
    }
    const std::uint64_t before = pool_->WindowStartQuantum();
    (void)pool_->Advance();
    return pool_->WindowStartQuantum() != before;
}

void TimelinePlanner::PublishThrough(std::uint64_t cutoffFrame) {
    if (!valid_) {
        return;
    }
    pool_->Publish(cutoffFrame, instances_, occurrences_, armTable_);
}

std::uint64_t TimelinePlanner::MaterializeDue(
    TransportEpoch transportEpoch, std::uint64_t cutoffFrame,
    std::uint64_t windowEndFrame) {
    if (!valid_) {
        return UINT64_MAX;
    }
    DropStaleDeferred();
    const std::uint64_t budgetQuantum = pool_->WindowStartQuantum();
    DeferredIntentClaim claim{};
    while (true) {
        std::uint32_t passDueCount = 0;
        const std::uint32_t claimed = ClaimDeferredDue(
            windowEndFrame, &claim, 1, nullptr, &passDueCount,
            budgetQuantum);
        if (claimed == 0) {
            break;
        }
        const TimelineDeferredMaterializeStatus result =
            MaterializeDeferredClaim(claim, transportEpoch, cutoffFrame);
        if (result == TimelineDeferredMaterializeStatus::Retry ||
            result == TimelineDeferredMaterializeStatus::NotOwned) {
            const std::uint64_t retryQuantum =
                budgetQuantum == UINT64_MAX ? UINT64_MAX : budgetQuantum + 1u;
            (void)deferredPool_->Retry(claim.token, retryQuantum);
        }
    }
    return DeferredBlockerBefore(cutoffFrame);
}

void TimelinePlanner::AdvanceQuantum(TransportEpoch transportEpoch) {
    if (!ReclaimConsumedQuantum() || !pool_->Valid()) {
        return;
    }
    const std::uint64_t publishQuanta =
        std::max<std::uint64_t>(pool_->PublishLeadQuanta(), 1u);
    if (publishQuanta >
        (UINT64_MAX - pool_->WindowStartFrame()) / pool_->QuantumFrames()) {
        return;
    }
    const std::uint64_t cutoff =
        pool_->WindowStartFrame() + publishQuanta * pool_->QuantumFrames();
    const std::uint64_t blocker =
        MaterializeDue(transportEpoch, cutoff, pool_->WindowEndFrame());
    const std::uint64_t safeCutoff =
        blocker == UINT64_MAX
            ? cutoff
            : std::min(cutoff,
                       blocker - blocker % pool_->QuantumFrames());
    PublishThrough(safeCutoff);
}

} // namespace harken
