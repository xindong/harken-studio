#include "PackPlaySelection.h"
#include "PackPlayInitFailure.h"

#include <cstdio>
#include <cstring>

namespace {

bool BindValidAction(harken::EnginePlayableStartRow* start,
                     harken::EnginePlayableServiceRow* service,
                     VnActionFiringHandle firing, std::uint32_t ordinal,
                     const char* guid) {
    start->slot = 0;
    start->generation = 1;
    start->serviceCount = 1;
    start->actionFiring = firing;
    start->rootService = {0, 1};
    service->identity = {0, 1};
    service->invocation = start;
    service->invocationGeneration = 1;
    service->selectionCount = 1;
    service->selectedChildOrdinals[0] = ordinal;
    service->selectionSeedProbe.captured = true;
    service->selectionSeedProbe.derivedCallerSeed = 5;
    service->selectionSeedProbe.acceptedPostOrdinal = 1;
    service->selectionSeedProbe.selectionOrdinal = 1;
    service->selectionSeedProbe.resetEpoch = 0;
    return harken::ParseObjectId(guid, &service->leaf.stableId);
}

void RetireServiceRow(harken::EnginePlayableStartRow* start,
                      harken::EnginePlayableServiceRow* service) {
    start->actionFiring = 0;
    start->serviceCount = 0;
    service->leaf.stableId = {};
    service->selectionCount = 0;
}

int NestedFiringSelectionSelfTest() {
    constexpr const char* kLeaf = "3dfbe430-399d-4e1c-88ff-5af4a56d6e96";
    constexpr const char* kOtherLeaf = "519dd49c-982e-4986-a814-3643256e430e";
    VnObjectId action{}, childAction{}, nestedAction{};
    harken::ActionFiringTable table;
    harken::PreparedActionFiring root, child, nested, otherRoot, otherChild;
    if (!harken::ParseObjectId(kLeaf, &action) ||
        !harken::ParseObjectId(kOtherLeaf, &childAction) ||
        !harken::ParseObjectId("11111111-2222-3333-4444-555555555555", &nestedAction) ||
        !table.Reset(5) ||
        !table.PrepareRoot(action, 1, 1, &root) ||
        !table.PrepareChild(root, childAction, 1, 1, &child) ||
        !table.PrepareChild(child, nestedAction, 1, 1, &nested) ||
        !table.CommitTree(&root, 1) ||
        !table.PrepareRoot(action, 1, 1, &otherRoot) ||
        !table.PrepareChild(otherRoot, childAction, 1, 1, &otherChild) ||
        !table.CommitTree(&otherRoot, 2)) {
        std::fprintf(stderr, "error: nested firing fixture failed\n");
        return 38;
    }
    static harken::EnginePlayableStartRow starts[2];
    static harken::EnginePlayableServiceRow services[2];
    if (!BindValidAction(&starts[0], &services[0], nested.Handle(), 1, kLeaf) ||
        !BindValidAction(&starts[1], &services[1], otherChild.Handle(), 2, kOtherLeaf)) {
        return 39;
    }
    starts[1].slot = 1;
    starts[1].rootService = {1, 1};
    services[1].identity = {1, 1};
    for (unsigned index = 0; index < 2; ++index) {
        const auto firing = index == 0 ? root.Handle() : otherRoot.Handle();
        const auto& expected = services[index].leaf.stableId;
        harkenhost::FiringSelectionCopy selection;
        harkenhost::FiringQueueWindowCopy window;
        harkenhost::FiringQueueEndCopy end;
        if (!harkenhost::CaptureFiringSelection(starts, 2, services, 2, firing, &selection, &table) ||
            selection.leafId != expected ||
            !harkenhost::ObserveFiringQueueWindow(starts, 2, services, 2, firing, &window, &table) ||
            window.starts != 1 ||
            !harkenhost::CaptureFiringQueueEnd(starts, 2, services, 2, firing, &end, &table) ||
            !end.rowPresent || end.leafId != expected) {
            std::fprintf(stderr, "error: nested diagnostics lost or mixed firing trees\n");
            return 40;
        }
    }
    harkenhost::FiringSelectionCopy selection;
    harkenhost::FiringQueueWindowCopy window;
    harkenhost::FiringQueueEndCopy end;
    if (harkenhost::CaptureFiringSelection(starts + 1, 1, services, 2, root.Handle(), &selection, &table) ||
        !harkenhost::ObserveFiringQueueWindow(starts + 1, 1, services, 2, root.Handle(), &window, &table) ||
        window.starts != 0 ||
        !harkenhost::CaptureFiringQueueEnd(starts + 1, 1, services, 2, root.Handle(), &end, &table) ||
        end.rowPresent) {
        std::fprintf(stderr, "error: unrelated firing was included\n");
        return 41;
    }
    std::printf("selection-copy-self-test nested descendants exclude unrelated trees\n");
    return 0;
}


int QueueWindowDiagSelfTest() {
    constexpr VnActionFiringHandle kFiring = 42;
    constexpr const char* kSuccessor = "519dd49c-982e-4986-a814-3643256e430e";
    static harken::EnginePlayableStartRow starts[2];
    static harken::EnginePlayableServiceRow services[2];
    const auto bind = [](harken::EnginePlayableStartRow* start,
                         harken::EnginePlayableServiceRow* service,
                         std::uint32_t startSlot, std::uint64_t generation,
                         std::uint32_t serviceSlot, VnActionFiringHandle firing) {
        start->slot = startSlot;
        start->generation = generation;
        start->serviceCount = 1;
        start->actionFiring = firing;
        start->rootService = {serviceSlot, 1};
        service->identity = {serviceSlot, 1};
        service->invocation = start;
        service->invocationGeneration = generation;
    };

    services[0].queueSuccessorLeaf = {};
    services[0].queueNextState = {};
    services[1].queueSuccessorLeaf = {};
    services[1].queueNextState = {};
    bind(&starts[0], &services[0], 0, 1, 0, kFiring);
    starts[1].actionFiring = 0;

    char line[320] = {};
    harkenhost::FiringQueueWindowCopy unseen{};
    if (!harkenhost::ObserveFiringQueueWindow(nullptr, 0, nullptr, 0, kFiring,
                                             &unseen) ||
        unseen.starts != 0 || unseen.successorPresent ||
        !harkenhost::FormatFiringQueueWindowLine(unseen, 1, 1, kFiring, line,
                                                sizeof(line)) ||
        std::strcmp(line,
                    "pack-play queue-window trigger=1/1 starts=0 successor=none firing=42\n") != 0 ||
        std::strstr(line, "pack-play trigger ") != nullptr ||
        std::strncmp(line, "pack-play selection-child", 25) == 0) {
        std::fprintf(stderr, "error: queue-window empty tally was %s\n", line);
        return 6;
    }

    harkenhost::FiringQueueWindowCopy onlyFirst{};
    for (int tick = 0; tick < 3; ++tick) {
        if (!harkenhost::ObserveFiringQueueWindow(&starts[0], 1, &services[0], 1,
                                                 kFiring, &onlyFirst)) {
            std::fprintf(stderr, "error: queue-window first-only observe failed\n");
            return 7;
        }
    }
    if (onlyFirst.starts != 1 || onlyFirst.successorPresent ||
        !harkenhost::FormatFiringQueueWindowLine(onlyFirst, 2, 8, kFiring, line,
                                                sizeof(line)) ||
        std::strcmp(line,
                    "pack-play queue-window trigger=2/8 starts=1 successor=none firing=42\n") != 0 ||
        std::strstr(line, "pack-play trigger ") != nullptr ||
        std::strncmp(line, "pack-play selection-child", 25) == 0) {
        std::fprintf(stderr, "error: queue-window first-only line was %s\n", line);
        return 8;
    }

    harkenhost::FiringQueueWindowCopy later{};
    if (!harkenhost::ObserveFiringQueueWindow(&starts[0], 1, services, 2, kFiring,
                                             &later) ||
        later.starts != 1 || later.successorPresent) {
        std::fprintf(stderr, "error: queue-window early tick saw a successor\n");
        return 9;
    }
    if (!harken::ParseObjectId(kSuccessor, &services[0].queueSuccessorLeaf.stableId)) {
        std::fprintf(stderr, "error: queue-window successor guid rejected\n");
        return 10;
    }
    services[0].queueNextState.currentChildOrdinal = 3;
    bind(&starts[1], &services[1], 1, 4, 1, kFiring);
    if (!harkenhost::ObserveFiringQueueWindow(starts, 2, services, 2, kFiring, &later)) {
        std::fprintf(stderr, "error: queue-window second tick observe failed\n");
        return 11;
    }
    services[0].queueSuccessorLeaf.stableId = {};
    services[0].queueNextState.currentChildOrdinal = 0;
    starts[0].actionFiring = 0;
    if (!harkenhost::ObserveFiringQueueWindow(starts, 2, services, 2, kFiring, &later)) {
        std::fprintf(stderr, "error: queue-window retire tick observe failed\n");
        return 12;
    }
    if (later.starts < 2 || !later.successorPresent ||
        harken::ObjectIdString(later.successorId) != kSuccessor ||
        !later.successorOrdinalPresent || later.successorOrdinal != 3 ||
        !harkenhost::FormatFiringQueueWindowLine(later, 2, 8, kFiring, line,
                                                sizeof(line)) ||
        std::strcmp(
            line,
            "pack-play queue-window trigger=2/8 starts=2 "
            "successor=519dd49c-982e-4986-a814-3643256e430e#3 firing=42\n") != 0 ||
        std::strstr(line, "successor=none") != nullptr ||
        std::strstr(line, kSuccessor) == nullptr ||
        std::strstr(line, "pack-play trigger ") != nullptr ||
        std::strncmp(line, "pack-play selection-child", 25) == 0 ||
        !harkenhost::PrintFiringQueueWindow(later, 2, 8, kFiring)) {
        std::fprintf(stderr, "error: queue-window second-segment line was %s\n", line);
        return 13;
    }
    harkenhost::FiringQueueWindowCopy capped{};
    constexpr std::uint32_t kCap =
        harkenhost::FiringQueueWindowCopy::kMaxDistinctStarts;
    static harken::EnginePlayableStartRow capStarts[kCap + 1];
    static harken::EnginePlayableServiceRow capServices[kCap + 1];
    for (std::uint32_t index = 0; index <= kCap; ++index) {
        bind(&capStarts[index], &capServices[index], index, 1, index, kFiring);
    }
    if (!harkenhost::ObserveFiringQueueWindow(capStarts, kCap + 1, capServices,
                                             kCap + 1, kFiring, &capped) ||
        capped.starts != kCap || !capped.startsTruncated ||
        !harkenhost::FormatFiringQueueWindowLine(capped, 1, 1, kFiring, line,
                                                sizeof(line)) ||
        std::strcmp(line,
                    "pack-play queue-window trigger=1/1 starts=32+ "
                    "successor=none firing=42\n") != 0 ||
        std::strstr(line, "pack-play trigger ") != nullptr ||
        std::strncmp(line, "pack-play selection-child", 25) == 0) {
        std::fprintf(stderr, "error: queue-window truncated line was %s\n", line);
        return 14;
    }

    std::printf("queue-window-self-test empty starts=0 successor=none\n");
    std::printf("queue-window-self-test first-only starts=1 successor=none\n");
    std::printf("queue-window-self-test second-later starts=%u successor=%s\n",
                later.starts, kSuccessor);
    std::printf("queue-window-self-test truncated starts=32+\n");
    return 0;
}

}

int QueueHeardDiagSelfTest() {
    constexpr VnActionFiringHandle kFiring = 42;
    constexpr const char* kSuccessor = "519dd49c-982e-4986-a814-3643256e430e";
    constexpr std::uint64_t kFrame = 76800;
    static harken::EnginePlayableStartRow starts[1];
    static harken::EnginePlayableServiceRow service;
    starts[0].slot = 0;
    starts[0].generation = 1;
    starts[0].serviceCount = 1;
    starts[0].actionFiring = kFiring;
    starts[0].rootService = {0, 1};
    service.identity = {0, 1};
    service.invocation = &starts[0];
    service.invocationGeneration = 1;
    service.queueSuccessorLeaf = {};
    service.queueNextState = {};
    service.queueSuccessorProgramPlane = 1;
    service.queueSuccessorSchedule = {};
    service.schedule = {};

    char line[320] = {};
    harkenhost::FiringQueueWindowCopy quiet{};
    if (!harkenhost::ObserveFiringQueueWindow(starts, 1, &service, 1, kFiring, &quiet)) {
        std::fprintf(stderr, "error: queue-heard quiet observe failed\n");
        return 15;
    }
    harkenhost::NoteFiringQueueHeardAudio(&quiet, true);
    harkenhost::NoteFiringQueueHeardAudio(&quiet, true);
    if (quiet.successorPresent || quiet.firstKnown || quiet.scheduledKnown || quiet.silenceToSoundEntries != 1 ||
        !harkenhost::FormatFiringQueueHeardLine(quiet, 1, 1, kFiring, line, sizeof(line)) ||
        std::strcmp(line,
                    "pack-play queue-heard trigger=1/1 scheduled=unknown first=unknown heard=0 successor=none firing=42\n") != 0 ||
        std::strstr(line, "pack-play trigger ") != nullptr ||
        std::strncmp(line, "pack-play selection-child", 25) == 0) {
        std::fprintf(stderr, "error: queue-heard quiet line was %s\n", line);
        return 16;
    }
    service.startFrame = 1000;
    if (!harkenhost::ObserveFiringQueueWindow(starts, 1, &service, 1, kFiring, &quiet) ||
        quiet.firstKnown ||
        !harkenhost::FormatFiringQueueHeardLine(quiet, 1, 1, kFiring, line, sizeof(line)) ||
        std::strcmp(line,
                    "pack-play queue-heard trigger=1/1 scheduled=unknown first=unknown heard=0 successor=none firing=42\n") != 0) {
        std::fprintf(stderr, "error: queue-heard first unknown did not stick: %s\n", line);
        return 24;
    }

    if (!harken::ParseObjectId(kSuccessor, &service.queueSuccessorLeaf.stableId)) {
        std::fprintf(stderr, "error: queue-heard successor guid rejected\n");
        return 17;
    }
    service.queueNextState.currentChildOrdinal = 3;
    service.queueSuccessorSchedule.eventCount = 1;
    service.queueSuccessorSchedule.entryAnchorFrame = kFrame;
    service.startFrame = 1000;
    harkenhost::FiringQueueWindowCopy silent{};
    if (!harkenhost::ObserveFiringQueueWindow(starts, 1, &service, 1, kFiring, &silent) ||
        !silent.successorPresent || !silent.scheduledKnown || silent.scheduledFrame != kFrame ||
        !silent.firstKnown || silent.firstFrame != 1000) {
        std::fprintf(stderr, "error: queue-heard did not copy the successor entry frame\n");
        return 18;
    }
    harkenhost::NoteFiringQueueHeardAudio(&silent, true);
    if (silent.silenceToSoundEntries != 1 ||
        !harkenhost::FormatFiringQueueHeardLine(silent, 1, 1, kFiring, line, sizeof(line)) ||
        std::strcmp(
            line,
            "pack-play queue-heard trigger=1/1 scheduled=76800 first=1000 heard=0 "
            "successor=519dd49c-982e-4986-a814-3643256e430e#3 firing=42\n") != 0) {
        std::fprintf(stderr, "error: queue-heard scheduled-silent line was %s\n", line);
        return 19;
    }

    harkenhost::FiringQueueWindowCopy sounded = silent;
    harkenhost::NoteFiringQueueHeardAudio(&sounded, false);
    harkenhost::NoteFiringQueueHeardAudio(&sounded, true);
    if (sounded.silenceToSoundEntries != 2 || silent.silenceToSoundEntries != 1 ||
        !harkenhost::FormatFiringQueueHeardLine(sounded, 1, 1, kFiring, line, sizeof(line)) ||
        std::strcmp(
            line,
            "pack-play queue-heard trigger=1/1 scheduled=76800 first=1000 heard=1 "
            "successor=519dd49c-982e-4986-a814-3643256e430e#3 firing=42\n") != 0 ||
        std::strstr(line, "pack-play trigger ") != nullptr ||
        std::strncmp(line, "pack-play selection-child", 25) == 0 ||
        !harkenhost::PrintFiringQueueHeard(sounded, 1, 1, kFiring)) {
        std::fprintf(stderr, "error: queue-heard reentry line was %s\n", line);
        return 20;
    }

    harkenhost::FiringQueueWindowCopy held = silent;
    harkenhost::NoteFiringQueueHeardObservation(&held, false, false);
    harkenhost::NoteFiringQueueHeardObservation(&held, true, true);
    if (held.silenceToSoundEntries != 1 || !held.busWasNonSilent ||
        silent.silenceToSoundEntries != 1 ||
        !harkenhost::FormatFiringQueueHeardLine(held, 1, 1, kFiring, line, sizeof(line)) ||
        std::strcmp(
            line,
            "pack-play queue-heard trigger=1/1 scheduled=76800 first=1000 heard=0 "
            "successor=519dd49c-982e-4986-a814-3643256e430e#3 firing=42\n") != 0) {
        std::fprintf(stderr, "error: queue-heard stale snapshot became a reentry: %s\n", line);
        return 23;
    }

    service.queueSuccessorProgramPlane = 0;
    service.startFrame = 1000;
    service.schedule = {};
    service.schedule.eventCount = 1;
    service.schedule.entryAnchorFrame = kFrame;
    harkenhost::FiringQueueWindowCopy plane0{};
    if (!harkenhost::ObserveFiringQueueWindow(starts, 1, &service, 1, kFiring, &plane0) ||
        !plane0.firstKnown || plane0.firstFrame != 1000 ||
        !plane0.scheduledKnown || plane0.scheduledFrame != kFrame ||
        plane0.firstFrame == plane0.scheduledFrame ||
        !harkenhost::FormatFiringQueueHeardLine(plane0, 1, 1, kFiring, line, sizeof(line)) ||
        std::strcmp(
            line,
            "pack-play queue-heard trigger=1/1 scheduled=76800 first=1000 heard=0 "
            "successor=519dd49c-982e-4986-a814-3643256e430e#3 firing=42\n") != 0) {
        std::fprintf(stderr, "error: queue-heard plane0 used schedule as first: %s\n", line);
        return 25;
    }
    service.queueSuccessorProgramPlane = 1;
    service.schedule = {};
    service.queueSuccessorSchedule = {};
    harkenhost::FiringQueueWindowCopy unknown{};
    if (!harkenhost::ObserveFiringQueueWindow(starts, 1, &service, 1, kFiring, &unknown) ||
        !unknown.successorPresent || unknown.scheduledKnown) {
        std::fprintf(stderr, "error: queue-heard missing frame was treated as known\n");
        return 21;
    }
    service.queueSuccessorSchedule.eventCount = 1;
    service.queueSuccessorSchedule.entryAnchorFrame = 1;
    service.startFrame = 4242;
    if (!harkenhost::ObserveFiringQueueWindow(starts, 1, &service, 1, kFiring, &unknown) ||
        unknown.scheduledKnown || unknown.firstFrame != 1000 || !unknown.firstKnown ||
        !harkenhost::FormatFiringQueueHeardLine(unknown, 1, 1, kFiring, line, sizeof(line)) ||
        std::strcmp(line,
                    "pack-play queue-heard trigger=1/1 scheduled=unknown first=1000 heard=0 "
                    "successor=519dd49c-982e-4986-a814-3643256e430e#3 firing=42\n") != 0) {
        std::fprintf(stderr, "error: queue-heard late frame overwrote unknown: %s\n", line);
        return 22;
    }

    std::printf("queue-heard-self-test no-successor scheduled=unknown heard=0\n");
    std::printf("queue-heard-self-test frame-76800 one-onset heard=0\n");
    std::printf("queue-heard-self-test frame-76800 reentry heard=1\n");
    std::printf("queue-heard-self-test first=1000 sticks\n");
    std::printf("queue-heard-self-test plane0 first=startFrame\n");
    return 0;
}

int QueueEndProbeSelfTest() {
    constexpr VnActionFiringHandle kFiring = 42;
    constexpr const char* kFirst = "62c8a083-3a44-45ad-92b3-0f7d4d16b4d0";
    constexpr const char* kSuccessor = "14ad7edb-54b1-430f-a3ec-ac811bee1c2d";
    static harken::EnginePlayableStartRow start;
    static harken::EnginePlayableServiceRow service;
    char line[320] = {};
    harkenhost::FiringQueueEndCopy none{};
    if (!harkenhost::CaptureFiringQueueEnd(nullptr, 0, nullptr, 0, kFiring, &none) ||
        none.rowPresent ||
        !harkenhost::FormatFiringQueueEndLine(none, 1, 1, kFiring, line, sizeof(line)) ||
        std::strcmp(line, "pack-play queue-end trigger=1/1 endLeaf=none endStart=0 successorRender=0 successorArm=0 signalOnly=0 continuationPrepared=0 lifecycle=0 naturalEnd=0 frontier=0 reason=0 firing=42\n") != 0 ||
        std::strstr(line, "pack-play trigger ") != nullptr ||
        std::strncmp(line, "pack-play selection-child", 25) == 0) {
        std::fprintf(stderr, "error: queue-end empty line was %s\n", line);
        return 26;
    }
    start.slot = 0;
    start.generation = 1;
    start.serviceCount = 1;
    start.actionFiring = kFiring;
    start.rootService = {0, 1};
    service = {};
    service.identity = {0, 1};
    service.invocation = &start;
    service.invocationGeneration = 1;
    service.startFrame = 4096;
    service.queueSuccessorRenderHandle = 0;
    service.queueSuccessorArm = {};
    service.queueSignalOnlyNoOverlap = false;
    service.queueContinuationPrepared = false;
    service.queueNaturalEnd = false;
    service.controlLifecycle = harken::PlayableControlLifecycle::Preparing;
    service.controlReason = harken::PlayableControlReason::None;
    service.frontierEntry = {};
    if (!harken::ParseObjectId(kFirst, &service.leaf.stableId)) {
        std::fprintf(stderr, "error: queue-end first guid rejected\n");
        return 27;
    }
    harkenhost::FiringQueueEndCopy firstRow{};
    if (!harkenhost::CaptureFiringQueueEnd(&start, 1, &service, 1, kFiring, &firstRow) ||
        !harkenhost::FormatFiringQueueEndLine(firstRow, 1, 1, kFiring, line, sizeof(line)) ||
        std::strcmp(line, "pack-play queue-end trigger=1/1 endLeaf=62c8a083-3a44-45ad-92b3-0f7d4d16b4d0 endStart=4096 successorRender=0 successorArm=0 signalOnly=0 continuationPrepared=0 lifecycle=1 naturalEnd=0 frontier=0 reason=0 firing=42\n") != 0) {
        std::fprintf(stderr, "error: queue-end first row was %s\n", line);
        return 28;
    }
    service.queueSuccessorRenderHandle = 9;
    service.queueSuccessorArm.slot = 1;
    service.queueSuccessorArm.generation = 1;
    service.queueSuccessorArm.renderSlot = 2;
    service.queueSuccessorArm.renderGeneration = 3;
    harkenhost::FiringQueueEndCopy installed{};
    if (!harkenhost::CaptureFiringQueueEnd(&start, 1, &service, 1, kFiring, &installed) ||
        !installed.successorRender || !installed.successorArm ||
        !harkenhost::FormatFiringQueueEndLine(installed, 1, 1, kFiring, line, sizeof(line)) ||
        std::strcmp(line, "pack-play queue-end trigger=1/1 endLeaf=62c8a083-3a44-45ad-92b3-0f7d4d16b4d0 endStart=4096 successorRender=1 successorArm=1 signalOnly=0 continuationPrepared=0 lifecycle=1 naturalEnd=0 frontier=0 reason=0 firing=42\n") != 0) {
        std::fprintf(stderr, "error: queue-end installed row was %s\n", line);
        return 29;
    }
    harkenhost::FiringQueueEndCopy kept = installed;
    start.actionFiring = 0;
    if (!harkenhost::CaptureFiringQueueEnd(&start, 1, &service, 1, kFiring, &kept) ||
        !harkenhost::FormatFiringQueueEndLine(kept, 1, 1, kFiring, line, sizeof(line)) ||
        std::strcmp(line, "pack-play queue-end trigger=1/1 endLeaf=62c8a083-3a44-45ad-92b3-0f7d4d16b4d0 endStart=4096 successorRender=1 successorArm=1 signalOnly=0 continuationPrepared=0 lifecycle=1 naturalEnd=0 frontier=0 reason=0 firing=42\n") != 0) {
        std::fprintf(stderr, "error: queue-end retired firing cleared the last bound row: %s\n", line);
        return 32;
    }
    start.actionFiring = kFiring;
    if (!harken::ParseObjectId(kSuccessor, &service.leaf.stableId)) {
        return 30;
    }
    service.startFrame = 33792;
    // Promote copies successor handles onto arm/renderHandle, then clears them.
    service.queueSuccessorRenderHandle = 0;
    service.queueSuccessorArm = {};
    harkenhost::FiringQueueEndCopy promoted{};
    if (!harkenhost::CaptureFiringQueueEnd(&start, 1, &service, 1, kFiring, &promoted) ||
        !harkenhost::FormatFiringQueueEndLine(promoted, 1, 1, kFiring, line, sizeof(line)) ||
        std::strcmp(line, "pack-play queue-end trigger=1/1 endLeaf=14ad7edb-54b1-430f-a3ec-ac811bee1c2d endStart=33792 successorRender=0 successorArm=0 signalOnly=0 continuationPrepared=0 lifecycle=1 naturalEnd=0 frontier=0 reason=0 firing=42\n") != 0 ||
        !harkenhost::PrintFiringQueueEnd(promoted, 1, 1, kFiring)) {
        std::fprintf(stderr, "error: queue-end promoted row was %s\n", line);
        return 31;
    }
    service.queueSignalOnlyNoOverlap = true;
    service.queueContinuationPrepared = true;
    service.controlLifecycle = harken::PlayableControlLifecycle::LogicalComplete;
    service.queueNaturalEnd = true;
    service.frontierEntry.coordinator.slot = 0;
    service.frontierEntry.coordinator.generation = 1;
    service.frontierEntry.attempt = 1;
    service.frontierEntry.owner.child = 1;
    service.frontierEntry.owner.childEpoch = 1;
    service.frontierEntry.owner.parent = 0;
    service.frontierEntry.owner.parentEpoch = 0;
    service.frontierEntry.owner.invocation = 1;
    service.frontierEntry.owner.invocationEpoch = 1;
    service.frontierEntry.owner.serviceSlot = 0;
    service.frontierEntry.owner.serviceGeneration = 1;
    if (!service.frontierEntry.Valid()) {
        std::fprintf(stderr, "error: queue-end frontier receipt was not Valid()\n");
        return 33;
    }
    harkenhost::FiringQueueEndCopy gated{};
    constexpr const char* kGated =
        "pack-play queue-end trigger=1/1 endLeaf=14ad7edb-54b1-430f-a3ec-ac811bee1c2d endStart=33792 successorRender=0 successorArm=0 signalOnly=1 continuationPrepared=1 lifecycle=4 naturalEnd=1 frontier=1 reason=0 firing=42\n";
    if (!harkenhost::CaptureFiringQueueEnd(&start, 1, &service, 1, kFiring, &gated) ||
        !harkenhost::FormatFiringQueueEndLine(gated, 1, 1, kFiring, line, sizeof(line)) ||
        std::strcmp(line, kGated) != 0) {
        std::fprintf(stderr, "error: queue-end promote gate was %s\n", line);
        return 34;
    }
    harkenhost::FiringQueueEndCopy gatedKept = gated;
    start.actionFiring = 0;
    if (!harkenhost::CaptureFiringQueueEnd(&start, 1, &service, 1, kFiring, &gatedKept) ||
        !harkenhost::FormatFiringQueueEndLine(gatedKept, 1, 1, kFiring, line, sizeof(line)) ||
        std::strcmp(line, kGated) != 0) {
        std::fprintf(stderr, "error: queue-end retired firing cleared promote gate: %s\n", line);
        return 35;
    }
    start.actionFiring = kFiring;
    service.queueSignalOnlyNoOverlap = false;
    service.queueContinuationPrepared = true;
    service.queueNaturalEnd = false;
    service.controlLifecycle = harken::PlayableControlLifecycle::FailingSourceActive;
    service.controlReason = harken::PlayableControlReason::StartRefused;
    service.frontierEntry = {};
    harkenhost::FiringQueueEndCopy refused{};
    constexpr const char* kRefused =
        "pack-play queue-end trigger=1/1 endLeaf=14ad7edb-54b1-430f-a3ec-ac811bee1c2d endStart=33792 successorRender=0 successorArm=0 signalOnly=0 continuationPrepared=1 lifecycle=6 naturalEnd=0 frontier=0 reason=5 firing=42\n";
    if (!harkenhost::CaptureFiringQueueEnd(&start, 1, &service, 1, kFiring, &refused) ||
        !harkenhost::FormatFiringQueueEndLine(refused, 1, 1, kFiring, line, sizeof(line)) ||
        std::strcmp(line, kRefused) != 0) {
        std::fprintf(stderr, "error: queue-end failing source was %s\n", line);
        return 36;
    }
    harkenhost::FiringQueueEndCopy refusedKept = refused;
    start.actionFiring = 0;
    if (!harkenhost::CaptureFiringQueueEnd(&start, 1, &service, 1, kFiring, &refusedKept) ||
        !harkenhost::FormatFiringQueueEndLine(refusedKept, 1, 1, kFiring, line, sizeof(line)) ||
        std::strcmp(line, kRefused) != 0) {
        std::fprintf(stderr, "error: queue-end retired firing cleared control reason: %s\n", line);
        return 37;
    }
    std::printf("queue-end-self-test none endStart=0\n");
    std::printf("queue-end-self-test first-leaf start=4096 render=0\n");
    std::printf("queue-end-self-test installed leaf-unchanged\n");
    std::printf("queue-end-self-test promoted leaf start=33792\n");
    return 0;
}

int main() {
    if (const int nested = NestedFiringSelectionSelfTest()) {
        return nested;
    }
    if (const int initFailure = harkenhost::RunPackPlayInitFailureSelfTest()) {
        return initFailure;
    }
    if (const int stageLife = harkenhost::RunRealDeviceInitStageLifetimeSelfTest()) {
        return stageLife;
    }
    if (const int openPlan = harkenhost::RunPlaybackOpenPlanSelfTest()) {
        return openPlan;
    }
    constexpr VnActionFiringHandle kFiring = 42;
    constexpr const char* kGuid = "3dfbe430-399d-4e1c-88ff-5af4a56d6e96";
    harken::EnginePlayableStartRow start{};
    harken::EnginePlayableServiceRow service{};
    if (!BindValidAction(&start, &service, kFiring, 1, kGuid)) {
        std::fprintf(stderr, "error: selection-copy fixture guid rejected\n");
        return 1;
    }

    harkenhost::FiringSelectionCopy bound{};
    if (!harkenhost::CaptureFiringSelection(&start, 1, &service, 1, kFiring, &bound) ||
        !bound.captured || !bound.ordinalPresent || bound.ordinal != 1 ||
        harken::ObjectIdString(bound.leafId) != kGuid ||
        !bound.seedProbe.captured ||
        bound.seedProbe.derivedCallerSeed != 5 ||
        bound.seedProbe.acceptedPostOrdinal != 1 ||
        bound.seedProbe.selectionOrdinal != 1 ||
        bound.seedProbe.resetEpoch != 0) {
        std::fprintf(stderr, "error: bound action snapshot was not captured\n");
        return 2;
    }

    RetireServiceRow(&start, &service);
    harkenhost::FiringSelectionCopy after{};
    if (harkenhost::CaptureFiringSelection(&start, 1, &service, 1, kFiring, &after) ||
        after.captured) {
        std::fprintf(stderr, "error: retired service row was still readable\n");
        return 3;
    }
    if (!bound.captured || bound.ordinal != 1 ||
        harken::ObjectIdString(bound.leafId) != kGuid ||
        !harkenhost::PrintCapturedSelectionChild(bound, 1, 1, kFiring) ||
        !harkenhost::PrintCapturedSelectionSeed(bound, 1, 1, kFiring)) {
        std::fprintf(stderr, "error: copy did not survive service retirement\n");
        return 4;
    }

    harken::EnginePlayableStartRow ambiguous[2]{};
    ambiguous[0].actionFiring = kFiring;
    ambiguous[1].actionFiring = kFiring;
    harkenhost::FiringSelectionCopy rejected{};
    if (harkenhost::CaptureFiringSelection(ambiguous, 2, &service, 1, kFiring, &rejected) ||
        !rejected.rejected || rejected.captured) {
        std::fprintf(stderr, "error: ambiguous firing was accepted\n");
        return 5;
    }
    std::printf("selection-copy-self-test before-retirement captured ordinal=1 guid=%s\n", kGuid);
    std::printf("selection-copy-self-test after-retirement captured=0\n");
    if (const int queueWindow = QueueWindowDiagSelfTest()) {
        return queueWindow;
    }
    if (const int queueHeard = QueueHeardDiagSelfTest()) {
        return queueHeard;
    }
    if (const int queueEnd = QueueEndProbeSelfTest()) {
        return queueEnd;
    }
    return 0;
}
