#ifndef HARKEN_CORE_MONITOR_EXECUTION_FACT_H
#define HARKEN_CORE_MONITOR_EXECUTION_FACT_H

#include <harken/ObjectId.h>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace harken {

// The source representation is private. Its discriminant selects both the
// body and validity mapping; no inactive union member is interpreted.
enum class MonitorFactBody : std::uint8_t { Execution = 0, Call = 1 };
inline constexpr std::uint8_t kMonitorFactCallBody = 0x80;
inline constexpr std::uint8_t kMonitorFactIdentityIncomplete = 1;
inline constexpr std::uint8_t kMonitorFactControlIncomplete = 2;

struct MonitorFactIdentity {
    std::uint64_t handle;
    VnObjectId project;
    VnObjectId object;
    std::uint64_t world;
    std::uint64_t semantic;
    std::uint64_t lineage;
    std::uint64_t revision;
};

struct MonitorFactRequests {
    std::uint64_t playback;
    std::uint64_t control;
    std::uint64_t selection;
    std::uint64_t controlAction;
    std::uint64_t controlParent;
    std::uint64_t selectionAction;
    std::uint64_t selectionParent;
    std::uint32_t controlOrdinal;
    std::uint32_t selectionOrdinal;
    std::uint8_t playbackOperation;
    std::uint8_t controlOperation;
    std::uint8_t selectionOperation;
    std::uint8_t playbackOrigin;
    std::uint8_t controlOrigin;
    std::uint8_t selectionOrigin;
    std::uint16_t reserved;
};

struct MonitorFactEmitters {
    std::uint64_t origin;
    std::uint64_t hostSession;
    std::uint64_t hostSequence;
    std::uint64_t hostRevision;
    std::uint64_t currentState;
    std::uint64_t currentSpatial;
};

struct MonitorFactRelations {
    std::uint64_t parent;
    std::uint64_t root;
    std::uint64_t action;
    std::uint64_t leaf;
    std::uint64_t owner;
    std::uint64_t logicalVoice;
    std::uint64_t renderActivation;
};

struct MonitorFactPosition {
    std::uint64_t logicCommit;
    std::uint64_t readCutKey;
    std::uint64_t readCutSequence;
    std::uint64_t transport;
    std::uint64_t frame;
    std::uint64_t audioBlock;
    std::uint64_t arranging;
    VnObjectId clip;
    std::uint32_t loop;
    std::uint32_t occurrence;
    union {
        struct { std::uint32_t offset; std::uint32_t span; } audio;
        struct { std::uint32_t exclusiveOrdinal; std::uint32_t reserved; } readCut;
        std::uint64_t ownerEpoch;
    } detail;
};

struct MonitorFactTarget {
    std::uint64_t handle;
    VnObjectId project;
    VnObjectId object;
    std::uint64_t world;
    std::uint64_t semantic;
    std::uint64_t revision;
    std::uint32_t kind;
    std::uint32_t contentKind;
    std::uint32_t scope;
    std::uint32_t reserved;
};

struct MonitorFactRequest {
    std::uint64_t sequence;
    std::uint32_t operation;
    std::uint32_t origin;
};

struct MonitorExecutionBody {
    MonitorFactIdentity identity;
    MonitorFactRequests requests;
    MonitorFactEmitters emitters;
    MonitorFactRelations relations;
    MonitorFactPosition position;
};

struct MonitorCallBody {
    MonitorFactIdentity identity;
    MonitorFactRequest request;
    MonitorFactRequest causedBy;
    MonitorFactEmitters emitters;
    MonitorFactTarget target;
    MonitorFactPosition position;
    std::uint64_t action;
    std::uint64_t root;
};

struct MonitorExecutionFact {
    std::uint64_t engine;
    std::uint64_t sourceSequence;
    std::uint64_t timestamp;
    std::uint64_t validity;
    std::uint8_t kind;
    std::uint8_t subjectNamespace;
    std::uint8_t contentKind;
    std::uint8_t terminalKind;
    std::uint8_t originKind;
    std::uint8_t hostOwnership;
    std::uint8_t bindingStates;
    std::uint8_t flags;
    std::int32_t result;
    std::uint32_t reason;
    union {
        MonitorExecutionBody execution;
        MonitorCallBody call;
    } body;

    MonitorFactBody BodyKind() const noexcept {
        return (flags & kMonitorFactCallBody) != 0 ? MonitorFactBody::Call : MonitorFactBody::Execution;
    }
};

static_assert(sizeof(MonitorFactIdentity) == 72);
static_assert(sizeof(MonitorFactRequests) == 72);
static_assert(sizeof(MonitorFactEmitters) == 48);
static_assert(sizeof(MonitorFactRelations) == 56);
static_assert(sizeof(MonitorFactPosition) == 88);
static_assert(sizeof(MonitorFactTarget) == 80);
static_assert(sizeof(MonitorExecutionBody) == 336);
static_assert(sizeof(MonitorCallBody) == 336);
static_assert(offsetof(MonitorExecutionFact, body) == 48);
static_assert(sizeof(MonitorExecutionFact) == 384);
static_assert(std::is_trivially_copyable_v<MonitorExecutionFact>);
static_assert(std::is_standard_layout_v<MonitorExecutionFact>);

} // namespace harken

#endif
