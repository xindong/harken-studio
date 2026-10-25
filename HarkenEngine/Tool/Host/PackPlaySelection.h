#ifndef HARKEN_HOST_PACK_PLAY_SELECTION_H
#define HARKEN_HOST_PACK_PLAY_SELECTION_H

#include "Engine.h"

#include <harken/Harken.h>

#include <cstdio>
#include <string>

namespace harkenhost {

inline bool MatchesFiring(VnActionFiringHandle candidate, VnActionFiringHandle root,
                          const harken::ActionFiringTable* firings) {
    return candidate == root ||
        (firings != nullptr && firings->BelongsToTree(root, candidate));
}

struct FiringSelectionCopy {
    bool captured = false;
    bool rejected = false;
    bool ordinalPresent = false;
    std::uint32_t ordinal = 0;
    VnObjectId leafId{};
    harken::SelectionSeedProbeCopyV1 seedProbe{};
};

// Copy while this firing's root service row is still bound. The trigger
// window later retires that row and clears actionFiring, so the success
// path must print this copy and must not scan again.
inline bool CaptureFiringSelection(
    const harken::EnginePlayableStartRow* starts, std::size_t startCount,
    const harken::EnginePlayableServiceRow* services, std::size_t serviceCount,
    VnActionFiringHandle firing, FiringSelectionCopy* copy,
    const harken::ActionFiringTable* firings = nullptr) {
    if (copy == nullptr || copy->captured || copy->rejected || firing == 0) {
        return copy != nullptr && copy->captured;
    }
    if (starts == nullptr || services == nullptr) {
        return false;
    }
    const harken::EnginePlayableStartRow* start = nullptr;
    for (std::size_t index = 0; index < startCount; ++index) {
        const harken::EnginePlayableStartRow& candidate = starts[index];
        if (!MatchesFiring(candidate.actionFiring, firing, firings)) {
            continue;
        }
        if (start != nullptr) {
            copy->rejected = true;
            return false;
        }
        start = &candidate;
    }
    if (start == nullptr || !start->rootService.Valid() ||
        start->rootService.slot >= serviceCount) {
        return false;
    }
    const harken::EnginePlayableServiceRow& row = services[start->rootService.slot];
    if (row.identity != start->rootService || !row.InvocationValid() ||
        &row.Invocation() != start ||
        row.selectionCount > row.selectedChildOrdinals.size() ||
        harken::IsNull(row.leaf.stableId)) {
        return false;
    }
    copy->leafId = row.leaf.stableId;
    copy->ordinalPresent = row.selectionCount != 0;
    if (copy->ordinalPresent) {
        copy->ordinal = row.selectedChildOrdinals[0];
    }
    copy->seedProbe = row.selectionSeedProbe;
    copy->captured = true;
    return true;
}

inline bool CaptureFiringSelection(harken::Engine* engine, VnActionFiringHandle firing,
                                   FiringSelectionCopy* copy) {
    if (engine == nullptr) {
        return copy != nullptr && copy->captured && !copy->rejected && firing != 0;
    }
    return CaptureFiringSelection(engine->playableStarts.data(), engine->playableStarts.size(),
                                  engine->playableServices.data(), engine->playableServices.size(),
                                  firing, copy, &engine->actionFirings);
}

inline bool PrintCapturedSelectionChild(const FiringSelectionCopy& copy,
                                        std::uint32_t triggerNumber,
                                        std::uint32_t repeats,
                                        VnActionFiringHandle firing) {
    if (!copy.captured || copy.rejected || harken::IsNull(copy.leafId) ||
        firing == 0 || triggerNumber == 0 || repeats == 0) {
        return false;
    }
    char ordinalText[16] = "-";
    if (copy.ordinalPresent) {
        std::snprintf(ordinalText, sizeof(ordinalText), "%u", copy.ordinal);
    }
    const std::string guid = harken::ObjectIdString(copy.leafId);
    std::printf(
        "pack-play selection-child trigger=%u/%u ordinal=%s guid=%s firing=%llu\n",
        triggerNumber, repeats, ordinalText, guid.c_str(),
        static_cast<unsigned long long>(firing));
    return true;
}

inline bool PrintCapturedSelectionSeed(const FiringSelectionCopy& copy,
                                       std::uint32_t triggerNumber,
                                       std::uint32_t repeats,
                                       VnActionFiringHandle firing) {
    if (!copy.captured || copy.rejected || !copy.seedProbe.captured ||
        firing == 0 || triggerNumber == 0 || repeats == 0) {
        return false;
    }
    std::printf(
        "pack-play selection-seed trigger=%u/%u derivedCallerSeed=%llu "
        "acceptedPostOrdinal=%llu selectionOrdinal=%llu resetEpoch=%llu\n",
        triggerNumber, repeats,
        static_cast<unsigned long long>(copy.seedProbe.derivedCallerSeed),
        static_cast<unsigned long long>(copy.seedProbe.acceptedPostOrdinal),
        static_cast<unsigned long long>(copy.seedProbe.selectionOrdinal),
        static_cast<unsigned long long>(copy.seedProbe.resetEpoch));
    return true;
}


// Read-only Random_Queue window tally. CaptureFiringSelection keeps the
// first bound row and then returns, so a later segment is invisible there.
// This copy is filled on every observation while the trigger window runs:
// how many distinct playable starts matched the firing, and the first
// non-null queueSuccessorLeaf on such a start's root service row. The leaf
// is cleared when that row retires, so the guid and the row's
// queueNextState.currentChildOrdinal are stored at the moment they are seen.
// This does not call Select and does not write Queue state.
struct FiringQueueWindowCopy {
    static constexpr std::uint32_t kMaxDistinctStarts = 32;
    struct StartIdentity {
        std::uint32_t slot = UINT32_MAX;
        std::uint64_t generation = 0;
    };
    StartIdentity seenStarts[kMaxDistinctStarts]{};
    std::uint32_t seenStartCount = 0;
    std::uint32_t starts = 0;
    bool startsTruncated = false;
    bool successorPresent = false;
    bool successorOrdinalPresent = false;
    std::uint32_t successorOrdinal = 0;
    VnObjectId successorId{};
    bool scheduledKnown = false;
    std::uint64_t scheduledFrame = 0;
    bool firstKnown = false;
    std::uint64_t firstFrame = 0;
    bool busWasNonSilent = false;
    std::uint32_t silenceToSoundEntries = 0;
};

inline void NoteQueueSuccessorLeaf(const harken::EnginePlayableServiceRow& row,
                                   FiringQueueWindowCopy* copy) {
    if (copy == nullptr || copy->successorPresent ||
        harken::IsNull(row.queueSuccessorLeaf.stableId)) {
        return;
    }
    copy->successorId = row.queueSuccessorLeaf.stableId;
    copy->successorPresent = true;
    copy->successorOrdinal = row.QueueNextState().currentChildOrdinal;
    copy->successorOrdinalPresent = true;
    const auto& successorSchedule = row.queueSuccessorProgramPlane == 0
        ? row.schedule
        : row.queueSuccessorSchedule;
    if (successorSchedule.eventCount != 0) {
        copy->scheduledKnown = true;
        copy->scheduledFrame = successorSchedule.entryAnchorFrame;
    }
    copy->firstKnown = true;
    copy->firstFrame = row.startFrame;
}

inline bool ObserveFiringQueueWindow(
    const harken::EnginePlayableStartRow* starts, std::size_t startCount,
    const harken::EnginePlayableServiceRow* services, std::size_t serviceCount,
    VnActionFiringHandle firing, FiringQueueWindowCopy* copy,
    const harken::ActionFiringTable* firings = nullptr) {
    if (copy == nullptr || firing == 0) {
        return false;
    }
    if ((startCount != 0 && starts == nullptr) ||
        (serviceCount != 0 && services == nullptr)) {
        return false;
    }
    for (std::size_t index = 0; index < startCount; ++index) {
        const harken::EnginePlayableStartRow& start = starts[index];
        if (!MatchesFiring(start.actionFiring, firing, firings)) {
            continue;
        }
        bool seen = false;
        for (std::uint32_t seenIndex = 0; seenIndex < copy->seenStartCount;
             ++seenIndex) {
            const FiringQueueWindowCopy::StartIdentity& key =
                copy->seenStarts[seenIndex];
            if (key.slot == start.slot && key.generation == start.generation) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            if (copy->seenStartCount < FiringQueueWindowCopy::kMaxDistinctStarts) {
                FiringQueueWindowCopy::StartIdentity& key =
                    copy->seenStarts[copy->seenStartCount++];
                key.slot = start.slot;
                key.generation = start.generation;
                ++copy->starts;
            } else {
                copy->startsTruncated = true;
            }
        }
        if (services == nullptr || !start.rootService.Valid() ||
            start.rootService.slot >= serviceCount) {
            continue;
        }
        const harken::EnginePlayableServiceRow& row =
            services[start.rootService.slot];
        if (row.identity == start.rootService) {
            NoteQueueSuccessorLeaf(row, copy);
        }
    }
    return true;
}

inline bool ObserveFiringQueueWindow(harken::Engine* engine,
                                     VnActionFiringHandle firing,
                                     FiringQueueWindowCopy* copy) {
    if (copy == nullptr || firing == 0) {
        return false;
    }
    if (engine == nullptr) {
        return false;
    }
    return ObserveFiringQueueWindow(
        engine->playableStarts.data(), engine->playableStarts.size(),
        engine->playableServices.data(), engine->playableServices.size(),
        firing, copy, &engine->actionFirings);
}

inline bool FormatFiringQueueWindowLine(const FiringQueueWindowCopy& copy,
                                        std::uint32_t triggerNumber,
                                        std::uint32_t repeats,
                                        VnActionFiringHandle firing,
                                        char* buffer, std::size_t bufferSize) {
    if (buffer == nullptr || bufferSize == 0 || firing == 0 ||
        triggerNumber == 0 || repeats == 0) {
        return false;
    }
    char successor[96];
    if (!copy.successorPresent || harken::IsNull(copy.successorId)) {
        if (std::snprintf(successor, sizeof(successor), "none") < 0) {
            return false;
        }
    } else {
        const std::string guid = harken::ObjectIdString(copy.successorId);
        const int written = copy.successorOrdinalPresent
            ? std::snprintf(successor, sizeof(successor), "%s#%u", guid.c_str(),
                            copy.successorOrdinal)
            : std::snprintf(successor, sizeof(successor), "%s", guid.c_str());
        if (written < 0 || static_cast<std::size_t>(written) >= sizeof(successor)) {
            return false;
        }
    }
    const int written = std::snprintf(
        buffer, bufferSize,
        "pack-play queue-window trigger=%u/%u starts=%u%s successor=%s firing=%llu\n",
        triggerNumber, repeats, copy.starts, copy.startsTruncated ? "+" : "",
        successor, static_cast<unsigned long long>(firing));
    return written > 0 && static_cast<std::size_t>(written) < bufferSize;
}

inline bool PrintFiringQueueWindow(const FiringQueueWindowCopy& copy,
                                   std::uint32_t triggerNumber,
                                   std::uint32_t repeats,
                                   VnActionFiringHandle firing) {
    char line[320];
    if (!FormatFiringQueueWindowLine(copy, triggerNumber, repeats, firing, line,
                                     sizeof(line))) {
        return false;
    }
    if (std::fputs(line, stdout) < 0) {
        return false;
    }
    return std::fflush(stdout) == 0;
}



inline void NoteFiringQueueHeardAudio(FiringQueueWindowCopy* copy,
                                      bool nonSilentBlock) {
    if (copy == nullptr) {
        return;
    }
    if (nonSilentBlock && !copy->busWasNonSilent) {
        ++copy->silenceToSoundEntries;
    }
    copy->busWasNonSilent = nonSilentBlock;
}

// A failed or repeated snapshot is not a silent block. Skipping the note
// keeps busWasNonSilent, so the next fresh non-silent block is not a reentry.
inline void NoteFiringQueueHeardObservation(FiringQueueWindowCopy* copy,
                                            bool haveNewBlock,
                                            bool nonSilentBlock) {
    if (!haveNewBlock) {
        return;
    }
    NoteFiringQueueHeardAudio(copy, nonSilentBlock);
}

inline bool FormatQueueSuccessorToken(const FiringQueueWindowCopy& copy,
                                      char* successor, std::size_t successorSize) {
    if (successor == nullptr || successorSize == 0) {
        return false;
    }
    if (!copy.successorPresent || harken::IsNull(copy.successorId)) {
        const int written = std::snprintf(successor, successorSize, "none");
        return written > 0 && static_cast<std::size_t>(written) < successorSize;
    }
    const std::string guid = harken::ObjectIdString(copy.successorId);
    const int written = copy.successorOrdinalPresent
        ? std::snprintf(successor, successorSize, "%s#%u", guid.c_str(),
                        copy.successorOrdinal)
        : std::snprintf(successor, successorSize, "%s", guid.c_str());
    return written > 0 && static_cast<std::size_t>(written) < successorSize;
}

inline bool FormatFiringQueueHeardLine(const FiringQueueWindowCopy& copy,
                                       std::uint32_t triggerNumber,
                                       std::uint32_t repeats,
                                       VnActionFiringHandle firing,
                                       char* buffer, std::size_t bufferSize) {
    if (buffer == nullptr || bufferSize == 0 || firing == 0 ||
        triggerNumber == 0 || repeats == 0) {
        return false;
    }
    char successor[96];
    if (!FormatQueueSuccessorToken(copy, successor, sizeof(successor))) {
        return false;
    }
    char scheduled[32];
    const int scheduledWritten = copy.scheduledKnown
        ? std::snprintf(scheduled, sizeof(scheduled), "%llu",
                        static_cast<unsigned long long>(copy.scheduledFrame))
        : std::snprintf(scheduled, sizeof(scheduled), "unknown");
    if (scheduledWritten < 0 ||
        static_cast<std::size_t>(scheduledWritten) >= sizeof(scheduled)) {
        return false;
    }
    char first[32];
    const int firstWritten = copy.firstKnown
        ? std::snprintf(first, sizeof(first), "%llu",
                        static_cast<unsigned long long>(copy.firstFrame))
        : std::snprintf(first, sizeof(first), "unknown");
    if (firstWritten < 0 ||
        static_cast<std::size_t>(firstWritten) >= sizeof(first)) {
        return false;
    }
    const int written = std::snprintf(
        buffer, bufferSize,
        "pack-play queue-heard trigger=%u/%u scheduled=%s first=%s heard=%u successor=%s firing=%llu\n",
        triggerNumber, repeats, scheduled, first,
        copy.silenceToSoundEntries >= 2u ? 1u : 0u, successor,
        static_cast<unsigned long long>(firing));
    return written > 0 && static_cast<std::size_t>(written) < bufferSize;
}

inline bool PrintFiringQueueHeard(const FiringQueueWindowCopy& copy,
                                  std::uint32_t triggerNumber,
                                  std::uint32_t repeats,
                                  VnActionFiringHandle firing) {
    char line[320];
    if (!FormatFiringQueueHeardLine(copy, triggerNumber, repeats, firing, line,
                                    sizeof(line))) {
        return false;
    }
    if (std::fputs(line, stdout) < 0) {
        return false;
    }
    return std::fflush(stdout) == 0;
}



struct FiringQueueEndCopy {
    bool rowPresent = false;
    bool leafPresent = false;
    VnObjectId leafId{};
    std::uint64_t startFrame = 0;
    bool successorRender = false;
    bool successorArm = false;
    bool signalOnly = false;
    bool continuationPrepared = false;
    unsigned lifecycle = 0;
    bool naturalEnd = false;
    bool frontier = false;
    unsigned reason = 0;
};

inline bool CaptureFiringQueueEnd(
    const harken::EnginePlayableStartRow* starts, std::size_t startCount,
    const harken::EnginePlayableServiceRow* services, std::size_t serviceCount,
    VnActionFiringHandle firing, FiringQueueEndCopy* out,
    const harken::ActionFiringTable* firings = nullptr) {
    if (out == nullptr || firing == 0) {
        return false;
    }
    if ((startCount != 0 && starts == nullptr) ||
        (serviceCount != 0 && services == nullptr)) {
        return false;
    }
    FiringQueueEndCopy seen{};
    bool found = false;
    for (std::size_t index = 0; index < startCount; ++index) {
        const harken::EnginePlayableStartRow& start = starts[index];
        if (!MatchesFiring(start.actionFiring, firing, firings) || services == nullptr ||
            !start.rootService.Valid() ||
            start.rootService.slot >= serviceCount) {
            continue;
        }
        const harken::EnginePlayableServiceRow& row =
            services[start.rootService.slot];
        if (row.identity != start.rootService) {
            continue;
        }
        seen.rowPresent = true;
        seen.leafPresent = !harken::IsNull(row.leaf.stableId);
        seen.leafId = row.leaf.stableId;
        seen.startFrame = row.startFrame;
        seen.successorRender = row.queueSuccessorRenderHandle != 0;
        seen.successorArm = row.queueSuccessorArm.Valid();
        seen.signalOnly = row.queueSignalOnlyNoOverlap;
        seen.continuationPrepared = row.queueContinuationPrepared;
        seen.lifecycle = static_cast<unsigned>(row.controlLifecycle);
        seen.naturalEnd = row.queueNaturalEnd;
        seen.frontier = row.frontierEntry.Valid();
        seen.reason = static_cast<unsigned>(row.controlReason);
        found = true;
    }
    if (found) {
        *out = seen;
    }
    return true;
}

inline bool CaptureFiringQueueEnd(harken::Engine* engine,
                                  VnActionFiringHandle firing,
                                  FiringQueueEndCopy* out) {
    if (out == nullptr || firing == 0) {
        return false;
    }
    if (engine == nullptr) {
        return true;
    }
    return CaptureFiringQueueEnd(
        engine->playableStarts.data(), engine->playableStarts.size(),
        engine->playableServices.data(), engine->playableServices.size(),
        firing, out, &engine->actionFirings);
}

inline bool FormatFiringQueueEndLine(const FiringQueueEndCopy& copy,
                                     std::uint32_t triggerNumber,
                                     std::uint32_t repeats,
                                     VnActionFiringHandle firing,
                                     char* buffer, std::size_t bufferSize) {
    if (buffer == nullptr || bufferSize == 0 || firing == 0 ||
        triggerNumber == 0 || repeats == 0) {
        return false;
    }
    char leaf[80];
    char startText[32];
    if (!copy.rowPresent || !copy.leafPresent || harken::IsNull(copy.leafId)) {
        if (std::snprintf(leaf, sizeof(leaf), "none") < 0) {
            return false;
        }
    } else {
        const std::string guid = harken::ObjectIdString(copy.leafId);
        const int written = std::snprintf(leaf, sizeof(leaf), "%s", guid.c_str());
        if (written < 0 || static_cast<std::size_t>(written) >= sizeof(leaf)) {
            return false;
        }
    }
    const int startWritten = copy.rowPresent
        ? std::snprintf(startText, sizeof(startText), "%llu",
                        static_cast<unsigned long long>(copy.startFrame))
        : std::snprintf(startText, sizeof(startText), "0");
    if (startWritten < 0 ||
        static_cast<std::size_t>(startWritten) >= sizeof(startText)) {
        return false;
    }
    const int written = std::snprintf(
        buffer, bufferSize,
        "pack-play queue-end trigger=%u/%u endLeaf=%s endStart=%s successorRender=%u successorArm=%u signalOnly=%u continuationPrepared=%u lifecycle=%u naturalEnd=%u frontier=%u reason=%u firing=%llu\n",
        triggerNumber, repeats, leaf, startText,
        copy.successorRender ? 1u : 0u, copy.successorArm ? 1u : 0u,
        copy.rowPresent && copy.signalOnly ? 1u : 0u,
        copy.rowPresent && copy.continuationPrepared ? 1u : 0u,
        copy.rowPresent ? copy.lifecycle : 0u,
        copy.rowPresent && copy.naturalEnd ? 1u : 0u,
        copy.rowPresent && copy.frontier ? 1u : 0u,
        copy.rowPresent ? copy.reason : 0u,
        static_cast<unsigned long long>(firing));
    return written > 0 && static_cast<std::size_t>(written) < bufferSize;
}

inline bool PrintFiringQueueEnd(const FiringQueueEndCopy& copy,
                                std::uint32_t triggerNumber,
                                std::uint32_t repeats,
                                VnActionFiringHandle firing) {
    char line[320];
    if (!FormatFiringQueueEndLine(copy, triggerNumber, repeats, firing, line,
                                  sizeof(line))) {
        return false;
    }
    if (std::fputs(line, stdout) < 0) {
        return false;
    }
    return std::fflush(stdout) == 0;
}


}

#endif
