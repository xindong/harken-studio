#include <harken/Harken.h>

#include <cstddef>

static_assert(VN_ACTION_OPCODE_PLAY == 0);
static_assert(VN_ACTION_OPCODE_RESET == 4);
static_assert(VN_ACTION_TARGET_SOUND == 0);
static_assert(VN_ACTION_TARGET_BUS == 6);
static_assert(VN_ACTION_FIRING_TERMINAL_RETAINED == 2);
static_assert(VN_ACTION_OUTCOME_CANCELLED == 4);
static_assert(VN_ACTION_STEP_AWAITING_MATERIALIZATION == 7);
static_assert(VN_ACTION_TERMINAL_RUNTIME_ERROR == 11);

static_assert(sizeof(VnPostActionDesc) == 48);
static_assert(offsetof(VnPostActionDesc, structSize) == 0);
static_assert(offsetof(VnPostActionDesc, flags) == 4);
static_assert(offsetof(VnPostActionDesc, actionId) == 8);
static_assert(offsetof(VnPostActionDesc, emitter) == 24);
static_assert(offsetof(VnPostActionDesc, callerSeed) == 32);
static_assert(offsetof(VnPostActionDesc, reserved0) == 40);

static_assert(sizeof(VnActionFiringInfo) == 72);
static_assert(offsetof(VnActionFiringInfo, structSize) == 0);
static_assert(offsetof(VnActionFiringInfo, state) == 4);
static_assert(offsetof(VnActionFiringInfo, outcome) == 8);
static_assert(offsetof(VnActionFiringInfo, terminalReason) == 12);
static_assert(offsetof(VnActionFiringInfo, actionId) == 16);
static_assert(offsetof(VnActionFiringInfo, worldGeneration) == 32);
static_assert(offsetof(VnActionFiringInfo, acceptedLogicSequence) == 40);
static_assert(offsetof(VnActionFiringInfo, parentFiring) == 48);
static_assert(offsetof(VnActionFiringInfo, directStepCount) == 56);
static_assert(offsetof(VnActionFiringInfo, directStepTerminalCount) == 60);
static_assert(offsetof(VnActionFiringInfo, childFiringCount) == 64);
static_assert(offsetof(VnActionFiringInfo, livePlayableCount) == 68);

static_assert(sizeof(VnActionStepResult) == 96);
static_assert(offsetof(VnActionStepResult, structSize) == 0);
static_assert(offsetof(VnActionStepResult, state) == 4);
static_assert(offsetof(VnActionStepResult, result) == 8);
static_assert(offsetof(VnActionStepResult, opcode) == 12);
static_assert(offsetof(VnActionStepResult, ownerActionId) == 16);
static_assert(offsetof(VnActionStepResult, targetStableId) == 32);
static_assert(offsetof(VnActionStepResult, logicCommitSequence) == 48);
static_assert(offsetof(VnActionStepResult, childFiring) == 56);
static_assert(offsetof(VnActionStepResult, producedPlayableInstance) == 64);
static_assert(offsetof(VnActionStepResult, authoredEventIndex) == 72);
static_assert(offsetof(VnActionStepResult, executionOrdinal) == 76);
static_assert(offsetof(VnActionStepResult, depth) == 80);
static_assert(offsetof(VnActionStepResult, targetKind) == 84);
static_assert(offsetof(VnActionStepResult, capturedPlayableCount) == 88);
static_assert(offsetof(VnActionStepResult, remainingPlayableCount) == 92);
