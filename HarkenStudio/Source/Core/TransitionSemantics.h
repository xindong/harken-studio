#pragma once
#include "ProjectPlatform.h"

#include "CurveSemantics.h"
#include "Timeline/TimelineSemantics.h"

#include <QJsonObject>
#include <QString>
#include <QUuid>

#include <cstdint>
#include <optional>
#include <vector>

namespace VNX {

class PlacementContext;
struct ProjectItem;

enum class TransitionExitTiming : uint8_t
{
    Immediate = 0,
    NaturalEnd,
    NextBeat,
    NextBar,
    NextMarker,
    ExitMarker
};

enum class TransitionEntryMode : uint8_t
{
    EntryMarker = 0,
    SamePosition
};

struct TransitionFadeDef
{
    float durationSeconds = 0.5f;
    QVector<CurvePointDef> curve{{0.0f, 0.0f}, {1.0f, 1.0f}};
    QVector<CurveHandlePairDef> handles{{0.45f, 0.0f, 0.55f, 1.0f}};

    bool operator==(const TransitionFadeDef& other) const;
};

struct TransitionRuleDef
{
    std::optional<QUuid> source;
    std::optional<QUuid> target;
    TransitionExitTiming exitTiming = TransitionExitTiming::NextBar;
    TransitionFadeDef sourceFadeOut;
    std::optional<QUuid> bridge;
    TransitionFadeDef bridgeFadeIn;
    TransitionFadeDef bridgeFadeOut;
    TransitionEntryMode entryMode = TransitionEntryMode::EntryMarker;
    TransitionFadeDef targetFadeIn;

    bool isDefault() const { return !source && !target; }
    bool operator==(const TransitionRuleDef&) const = default;
};

enum class TransitionError : uint8_t
{
    None = 0,
    InvalidJson,
    InvalidFade,
    EmptyIdentity,
    MissingDefault,
    MultipleDefaults,
    DuplicateEndpoints,
    DefaultEndpoints,
    BridgeSamePosition,
    RuleNotFound,
    DefaultRuleRemoval,
    InvalidContainer,
    UnknownTimeline,
    CoordinateOverflow,
    Cycle
};

struct TransitionValidation
{
    TransitionError error = TransitionError::None;
    QString message;
    int index = -1;

    bool ok() const { return error == TransitionError::None; }
    explicit operator bool() const { return ok(); }

    static TransitionValidation success();
    static TransitionValidation failure(TransitionError error,
                                        const QString& message,
                                        int index = -1);
};

struct TransitionTableDef
{
    std::vector<TransitionRuleDef> rules;

    QJsonObject toJson() const;
    static std::optional<TransitionTableDef> fromJson(
        const QJsonObject& json, QString* error = nullptr);

    TransitionValidation validate() const;
    TransitionValidation replaceRules(std::vector<TransitionRuleDef> replacement);
    TransitionValidation addRule(const TransitionRuleDef& rule);
    TransitionValidation updateRule(int index, const TransitionRuleDef& rule);
    TransitionValidation removeRule(int index);

    bool operator==(const TransitionTableDef&) const = default;
};

TransitionFadeDef canonicalTransitionFade(float durationSeconds = 0.5f);
TransitionRuleDef canonicalTransitionRule();
TransitionTableDef canonicalTransitionTable();

enum class TransitionMountState : uint8_t { None = 0, Idle = 1, Editable = 2 };

namespace TransitionSemantics {

TransitionMountState transitionMountState(const ProjectItem& item, PlatformScope scope = std::nullopt);
bool isEndpointCandidate(const ProjectItem& container,
                         const ProjectItem& candidate);
bool isBridgeCandidate(const ProjectItem& candidate);
int ruleSpecificity(const TransitionRuleDef& rule);
std::vector<int> rulesForDisplay(const TransitionTableDef& table);
const TransitionRuleDef* matchRule(const TransitionTableDef& table,
                                   const QUuid& source,
                                   const QUuid& target,
                                   int* index = nullptr);

struct SamePositionResult
{
    TransitionValidation validation;
    SegmentTimeSpec targetTime;
    int64_t targetPosition = 0;
    int64_t targetOffset = 0;
    int64_t targetNominalLength = 0;

    bool known() const { return validation.ok(); }
    explicit operator bool() const { return known(); }
};

SamePositionResult samePositionMap(int64_t sourcePosition,
                                   const ProjectItem& source,
                                   const ProjectItem& target,
                                   const TimelineItemResolver& resolve,
                                   const TimelineNaturalDurationResolver& naturalDuration = {});

enum class TransitionWatchDirection : uint8_t
{
    None = 0,
    Down,
    UpPlacement,
    UpOrganization
};

struct TransitionWatchResult
{
    const ProjectItem* referenceSegment = nullptr;
    TransitionWatchDirection direction = TransitionWatchDirection::None;
    std::vector<QUuid> path;
    QString failureReason;
    bool placementContextProvided = false;
    bool placementContextValid = true;

    bool found() const { return referenceSegment != nullptr; }
    explicit operator bool() const { return found(); }
};

TransitionWatchResult resolveWatchReference(
    const ProjectItem* source,
    const ProjectItem& container,
    const PlacementContext* placement,
    const TimelineItemResolver& resolve);

struct TransitionWatchView
{
    QUuid containerGuid;
    bool hasSource = false;
    TransitionWatchDirection direction = TransitionWatchDirection::None;
    std::vector<QUuid> path;
    std::optional<QUuid> referenceSegment;
    bool placementContextProvided = false;
    bool placementContextValid = true;
    QString failureReason;
    QString caption;

    bool found() const { return referenceSegment.has_value(); }
    bool operator==(const TransitionWatchView&) const = default;
};

TransitionWatchView buildTransitionWatchView(
    const ProjectItem& container,
    const std::optional<QUuid>& sourceWinner,
    const PlacementContext* placement,
    const TimelineItemResolver& resolve);

enum class HandoverKind : uint8_t
{
    Transition = 0,
    SourceExit,
    SilentEntry,
    SameObject,
    NoMatch
};

struct HandoverResult
{
    HandoverKind kind = HandoverKind::NoMatch;
    int ruleIndex = -1;
    std::vector<QUuid> referenceChain;
    std::vector<QUuid> ruleChain;

    bool hasTransition() const
    {
        return kind == HandoverKind::Transition
            || kind == HandoverKind::SourceExit;
    }
};

HandoverResult deriveHandover(const TransitionTableDef& table,
                              const std::optional<QUuid>& source,
                              const std::optional<QUuid>& target);

}
}
