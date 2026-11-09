#include "TransitionSemantics.h"

#include "Components/VSegment.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "PlacementContext.h"
#include "ProjectTypes.h"
#include "PlaybackReferenceValidation.h"
#include "ReferenceSlotResolution.h"
#include "Timeline/TimelineCoordinates.h"
#include "Timeline/TimelineSemantics.h"

#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace VNX {
namespace {

bool equalPoint(const CurvePointDef& left, const CurvePointDef& right)
{
    return left.x == right.x && left.value == right.value;
}

bool equalHandle(const CurveHandlePairDef& left,
                 const CurveHandlePairDef& right)
{
    return left.h1x == right.h1x && left.h1y == right.h1y
        && left.h2x == right.h2x && left.h2y == right.h2y;
}

bool equalCurve(const QVector<CurvePointDef>& left,
                const QVector<CurvePointDef>& right)
{
    if (left.size() != right.size()) return false;
    for (qsizetype i = 0; i < left.size(); ++i)
        if (!equalPoint(left[i], right[i])) return false;
    return true;
}

bool equalHandles(const QVector<CurveHandlePairDef>& left,
                  const QVector<CurveHandlePairDef>& right)
{
    if (left.size() != right.size()) return false;
    for (qsizetype i = 0; i < left.size(); ++i)
        if (!equalHandle(left[i], right[i])) return false;
    return true;
}

QString exitTimingName(TransitionExitTiming timing)
{
    switch (timing) {
    case TransitionExitTiming::Immediate: return QStringLiteral("Immediate");
    case TransitionExitTiming::NaturalEnd: return QStringLiteral("NaturalEnd");
    case TransitionExitTiming::NextBeat: return QStringLiteral("NextBeat");
    case TransitionExitTiming::NextMeasure: return QStringLiteral("NextMeasure");
    case TransitionExitTiming::ExitMarker: return QStringLiteral("ExitMarker");
    }
    return {};
}

std::optional<TransitionExitTiming> parseExitTiming(const QJsonValue& value)
{
    if (!value.isString()) return std::nullopt;
    const QString text = value.toString();
    if (text == QStringLiteral("Immediate")) return TransitionExitTiming::Immediate;
    if (text == QStringLiteral("NaturalEnd")) return TransitionExitTiming::NaturalEnd;
    if (text == QStringLiteral("NextBeat")) return TransitionExitTiming::NextBeat;
    if (text == QStringLiteral("NextMeasure")) return TransitionExitTiming::NextMeasure;
    if (text == QStringLiteral("ExitMarker")) return TransitionExitTiming::ExitMarker;
    return std::nullopt;
}

QString entryModeName(TransitionEntryMode mode)
{
    return mode == TransitionEntryMode::InitialPosition
        ? QStringLiteral("InitialPosition") : QStringLiteral("EntryMarker");
}

std::optional<TransitionEntryMode> parseEntryMode(const QJsonValue& value)
{
    if (!value.isString()) return std::nullopt;
    if (value.toString() == QStringLiteral("EntryMarker"))
        return TransitionEntryMode::EntryMarker;
    if (value.toString() == QStringLiteral("InitialPosition"))
        return TransitionEntryMode::InitialPosition;
    return std::nullopt;
}

QJsonValue identityToJson(const std::optional<QUuid>& identity)
{
    return identity
        ? QJsonValue(identity->toString(QUuid::WithoutBraces))
        : QJsonValue(QJsonValue::Null);
}

bool parseIdentity(const QJsonValue& value, std::optional<QUuid>* out)
{
    if (!out) return false;
    if (value.isNull()) {
        *out = std::nullopt;
        return true;
    }
    if (!value.isString()) return false;
    const QUuid identity(value.toString());
    if (identity.isNull()) return false;
    *out = identity;
    return true;
}

QJsonArray pointsToJson(const QVector<CurvePointDef>& points)
{
    QJsonArray json;
    for (const CurvePointDef& point : points)
        json.append(QJsonObject{{QStringLiteral("x"), point.x},
                                {QStringLiteral("value"), point.value}});
    return json;
}

QJsonArray handlesToJson(const QVector<CurveHandlePairDef>& handles)
{
    QJsonArray json;
    for (const CurveHandlePairDef& handle : handles) {
        json.append(QJsonObject{{QStringLiteral("h1x"), handle.h1x},
                                {QStringLiteral("h1y"), handle.h1y},
                                {QStringLiteral("h2x"), handle.h2x},
                                {QStringLiteral("h2y"), handle.h2y}});
    }
    return json;
}

bool finiteNumber(const QJsonValue& value, float* out)
{
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number)
        || number < -std::numeric_limits<float>::max()
        || number > std::numeric_limits<float>::max())
        return false;
    *out = static_cast<float>(number);
    return true;
}

void normalizeFade(TransitionFadeDef* fade)
{
    if (!fade) return;
    if (fade->curve.isEmpty()) {
        const float offset = fade->offsetSeconds;
        *fade = canonicalTransitionFade(fade->durationSeconds);
        fade->offsetSeconds = offset;
        return;
    }
    for (CurvePointDef& point : fade->curve)
        point.x = std::clamp(point.x, 0.0f, 1.0f);
    curveNormalize(0.0f, 1.0f, fade->curve, fade->handles);
}

QJsonObject fadeToJson(const TransitionFadeDef& fade)
{
    return QJsonObject{
        {QStringLiteral("durationSeconds"), fade.durationSeconds},
        {QStringLiteral("offsetSeconds"), fade.offsetSeconds},
        {QStringLiteral("curve"), pointsToJson(fade.curve)},
        {QStringLiteral("handles"), handlesToJson(fade.handles)}
    };
}

std::optional<TransitionFadeDef> fadeFromJson(const QJsonValue& value)
{
    if (!value.isObject()) return std::nullopt;
    const QJsonObject json = value.toObject();
    TransitionFadeDef fade;
    fade.curve.clear();
    fade.handles.clear();
    if (!finiteNumber(json.value(QStringLiteral("durationSeconds")),
                      &fade.durationSeconds)
        || fade.durationSeconds < 0.0f
        || !finiteNumber(json.value(QStringLiteral("offsetSeconds")), &fade.offsetSeconds)
        || !json.value(QStringLiteral("curve")).isArray()
        || !json.value(QStringLiteral("handles")).isArray())
        return std::nullopt;
    const QJsonArray points = json.value(QStringLiteral("curve")).toArray();
    const QJsonArray handles = json.value(QStringLiteral("handles")).toArray();
    if (points.size() > kCurveMaxPoints) return std::nullopt;
    for (const QJsonValue& pointValue : points) {
        if (!pointValue.isObject()) return std::nullopt;
        const QJsonObject pointJson = pointValue.toObject();
        CurvePointDef point;
        if (!finiteNumber(pointJson.value(QStringLiteral("x")), &point.x)
            || !finiteNumber(pointJson.value(QStringLiteral("value")),
                             &point.value))
            return std::nullopt;
        fade.curve.push_back(point);
    }
    for (const QJsonValue& handleValue : handles) {
        if (!handleValue.isObject()) return std::nullopt;
        const QJsonObject handleJson = handleValue.toObject();
        CurveHandlePairDef handle;
        if (!finiteNumber(handleJson.value(QStringLiteral("h1x")), &handle.h1x)
            || !finiteNumber(handleJson.value(QStringLiteral("h1y")), &handle.h1y)
            || !finiteNumber(handleJson.value(QStringLiteral("h2x")), &handle.h2x)
            || !finiteNumber(handleJson.value(QStringLiteral("h2y")), &handle.h2y))
            return std::nullopt;
        fade.handles.push_back(handle);
    }
    normalizeFade(&fade);
    return fade;
}

TransitionValidation validateFade(const TransitionFadeDef& fade, int index)
{
    if (!std::isfinite(fade.durationSeconds) || fade.durationSeconds < 0.0f
        || !std::isfinite(fade.offsetSeconds) || fade.curve.size() > kCurveMaxPoints)
        return TransitionValidation::failure(
            TransitionError::InvalidFade,
            QStringLiteral("Transition fade is invalid"), index);
    TransitionFadeDef normalized = fade;
    for (const CurvePointDef& point : normalized.curve)
        if (!std::isfinite(point.x) || !std::isfinite(point.value))
            return TransitionValidation::failure(
                TransitionError::InvalidFade,
                QStringLiteral("Transition fade contains a non-finite point"), index);
    for (const CurveHandlePairDef& handle : normalized.handles)
        if (!std::isfinite(handle.h1x) || !std::isfinite(handle.h1y)
            || !std::isfinite(handle.h2x) || !std::isfinite(handle.h2y))
            return TransitionValidation::failure(
                TransitionError::InvalidFade,
                QStringLiteral("Transition fade contains a non-finite handle"), index);
    normalizeFade(&normalized);
    if (!(normalized == fade))
        return TransitionValidation::failure(
            TransitionError::InvalidFade,
            QStringLiteral("Transition fade is not normalized"), index);
    return TransitionValidation::success();
}

QJsonObject ruleToJson(const TransitionRuleDef& rule)
{
    return QJsonObject{
        {QStringLiteral("source"), identityToJson(rule.source)},
        {QStringLiteral("target"), identityToJson(rule.target)},
        {QStringLiteral("exitTiming"), exitTimingName(rule.exitTiming)},
        {QStringLiteral("sourceFadeOut"), fadeToJson(rule.sourceFadeOut)},
        {QStringLiteral("entryMode"), entryModeName(rule.entryMode)},
        {QStringLiteral("targetFadeIn"), fadeToJson(rule.targetFadeIn)},
        {QStringLiteral("sourceTail"), rule.sourceTail},
        {QStringLiteral("targetLeadIn"), rule.targetLeadIn}
    };
}

std::optional<TransitionRuleDef> ruleFromJson(const QJsonValue& value)
{
    if (!value.isObject()) return std::nullopt;
    const QJsonObject json = value.toObject();
    TransitionRuleDef rule;
    if (!parseIdentity(json.value(QStringLiteral("source")), &rule.source)
        || !parseIdentity(json.value(QStringLiteral("target")), &rule.target))
        return std::nullopt;
    const auto exitTiming = parseExitTiming(json.value(QStringLiteral("exitTiming")));
    const auto entryMode = parseEntryMode(json.value(QStringLiteral("entryMode")));
    const auto sourceFade = fadeFromJson(json.value(QStringLiteral("sourceFadeOut")));
    const auto targetFade = fadeFromJson(json.value(QStringLiteral("targetFadeIn")));
    const QJsonValue sourceTail = json.value(QStringLiteral("sourceTail"));
    const QJsonValue targetLeadIn = json.value(QStringLiteral("targetLeadIn"));
    if (!exitTiming || !entryMode || !sourceFade || !targetFade
        || !sourceTail.isBool() || !targetLeadIn.isBool())
        return std::nullopt;
    rule.exitTiming = *exitTiming;
    rule.entryMode = *entryMode;
    rule.sourceFadeOut = *sourceFade;
    rule.targetFadeIn = *targetFade;
    rule.sourceTail = sourceTail.toBool();
    rule.targetLeadIn = targetLeadIn.toBool();
    return rule;
}

bool sameEndpoints(const TransitionRuleDef& left,
                   const TransitionRuleDef& right)
{
    return left.source == right.source && left.target == right.target;
}

class VisitSet
{
public:
    bool enter(const ProjectItem* item)
    {
        if (!item) return false;
        if (item->guid.isNull()) {
            if (pointers_.contains(item)) return false;
            pointers_.insert(item);
            return true;
        }
        if (guids_.contains(item->guid)) return false;
        guids_.insert(item->guid);
        return true;
    }

private:
    QSet<QUuid> guids_;
    QSet<const ProjectItem*> pointers_;
};

struct DownResult
{
    const ProjectItem* segment = nullptr;
    std::vector<QUuid> path;
    bool cycle = false;
};

DownResult resolveDown(const ProjectItem* item,
                       const TimelineItemResolver& resolve,
                       VisitSet& visited,
                       std::vector<QUuid> path)
{
    if (!item) return {};
    if (!visited.enter(item)) return {nullptr, {}, true};
    path.push_back(item->guid);
    if (item->type == ProjectItem::Type::Segment)
        return {item, std::move(path), false};
    if (item->type == ProjectItem::Type::Sound) return {};
    if (item->type == ProjectItem::Type::Track) {
        const VTrack* timeline = item->comp<VTrack>();
        if (!timeline) return {};
        bool cycle = false;
        std::vector<size_t> order(timeline->clips.items.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t left, size_t right) {
            return timeline->clips.items[left].start
                < timeline->clips.items[right].start;
        });
        for (size_t index : order) {
            const ProjectItem* target = resolve
                ? resolve(timeline->clips.items[index].targetGuid) : nullptr;
            DownResult result = resolveDown(target, resolve, visited, path);
            if (result.segment) return result;
            cycle = cycle || result.cycle;
        }
        return {nullptr, {}, cycle};
    }
    bool cycle = false;
    for (const auto& child : item->children) {
        DownResult result = resolveDown(child.get(), resolve, visited, path);
        if (result.segment) return result;
        cycle = cycle || result.cycle;
    }
    return {nullptr, {}, cycle};
}

QString watchPathNames(const std::vector<QUuid>& path, const TimelineItemResolver& resolve)
{
    QString text;
    for (const QUuid& guid : path) {
        const ProjectItem* item = resolve ? resolve(guid) : nullptr;
        const QString label = item ? item->name : guid.toString(QUuid::WithoutBraces);
        if (!text.isEmpty()) text += QStringLiteral(" -> ");
        text += label;
    }
    return text;
}

}

bool TransitionFadeDef::operator==(const TransitionFadeDef& other) const
{
    return durationSeconds == other.durationSeconds
        && offsetSeconds == other.offsetSeconds
        && equalCurve(curve, other.curve)
        && equalHandles(handles, other.handles);
}

bool TransitionRuleDef::hasFade() const
{
    const TransitionFadeDef canonical = canonicalTransitionFade();
    return !(sourceFadeOut == canonical) || !(targetFadeIn == canonical);
}

TransitionValidation TransitionValidation::success()
{
    return {};
}

TransitionValidation TransitionValidation::failure(TransitionError error,
                                                   const QString& message,
                                                   int index)
{
    return {error, message, index};
}

TransitionFadeDef canonicalTransitionFade(float durationSeconds)
{
    TransitionFadeDef fade;
    fade.durationSeconds = durationSeconds;
    fade.curve = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    fade.handles = {{0.45f, 0.0f, 0.55f, 1.0f}};
    return fade;
}

TransitionRuleDef canonicalTransitionRule()
{
    TransitionRuleDef rule;
    rule.sourceFadeOut = canonicalTransitionFade();
    rule.targetFadeIn = canonicalTransitionFade();
    return rule;
}

TransitionTableDef canonicalTransitionTable()
{
    return {{canonicalTransitionRule()}};
}

QJsonObject TransitionTableDef::toJson() const
{
    QJsonArray jsonRules;
    for (const TransitionRuleDef& rule : rules) jsonRules.append(ruleToJson(rule));
    return QJsonObject{{QStringLiteral("rules"), jsonRules}};
}

std::optional<TransitionTableDef> TransitionTableDef::fromJson(
    const QJsonObject& json, QString* error)
{
    if (!json.value(QStringLiteral("rules")).isArray()) {
        if (error) *error = QStringLiteral("Transition table rules must be an array");
        return std::nullopt;
    }
    TransitionTableDef table;
    const QJsonArray rules = json.value(QStringLiteral("rules")).toArray();
    table.rules.reserve(rules.size());
    for (qsizetype i = 0; i < rules.size(); ++i) {
        auto rule = ruleFromJson(rules[i]);
        if (!rule) {
            if (error) *error = QStringLiteral("Transition rule %1 is invalid").arg(i);
            return std::nullopt;
        }
        table.rules.push_back(std::move(*rule));
    }
    const TransitionValidation validation = table.validate();
    if (!validation) {
        if (error) *error = validation.message;
        return std::nullopt;
    }
    return table;
}

TransitionValidation TransitionTableDef::validate() const
{
    int defaults = 0;
    for (size_t i = 0; i < rules.size(); ++i) {
        const TransitionRuleDef& rule = rules[i];
        if (rule.source && rule.source->isNull())
            return TransitionValidation::failure(
                TransitionError::EmptyIdentity,
                QStringLiteral("Transition source identity is empty"),
                static_cast<int>(i));
        if (rule.target && rule.target->isNull())
            return TransitionValidation::failure(
                TransitionError::EmptyIdentity,
                QStringLiteral("Transition target identity is empty"),
                static_cast<int>(i));
        if (rule.isDefault()) ++defaults;
        const TransitionFadeDef* fades[] = {&rule.sourceFadeOut, &rule.targetFadeIn};
        for (const TransitionFadeDef* fade : fades) {
            const TransitionValidation validation = validateFade(
                *fade, static_cast<int>(i));
            if (!validation) return validation;
        }
        for (size_t previous = 0; previous < i; ++previous)
            if (sameEndpoints(rules[previous], rule))
                return TransitionValidation::failure(
                    TransitionError::DuplicateEndpoints,
                    QStringLiteral("Transition endpoint combination is duplicated"),
                    static_cast<int>(i));
    }
    if (defaults == 0)
        return TransitionValidation::failure(
            TransitionError::MissingDefault,
            QStringLiteral("Transition table has no default rule"));
    if (defaults != 1)
        return TransitionValidation::failure(
            TransitionError::MultipleDefaults,
            QStringLiteral("Transition table has multiple default rules"));
    return TransitionValidation::success();
}

TransitionValidation TransitionTableDef::replaceRules(
    std::vector<TransitionRuleDef> replacement)
{
    TransitionTableDef candidate{std::move(replacement)};
    const TransitionValidation validation = candidate.validate();
    if (validation) rules = std::move(candidate.rules);
    return validation;
}

TransitionValidation TransitionTableDef::addRule(const TransitionRuleDef& rule)
{
    std::vector<TransitionRuleDef> candidate = rules;
    candidate.push_back(rule);
    return replaceRules(std::move(candidate));
}

TransitionValidation TransitionTableDef::updateRule(
    int index, const TransitionRuleDef& rule)
{
    if (index < 0 || index >= static_cast<int>(rules.size()))
        return TransitionValidation::failure(
            TransitionError::RuleNotFound,
            QStringLiteral("Transition rule does not exist"), index);
    if (rules[index].isDefault() && !rule.isDefault())
        return TransitionValidation::failure(
            TransitionError::DefaultEndpoints,
            QStringLiteral("Default transition endpoints cannot be changed"), index);
    std::vector<TransitionRuleDef> candidate = rules;
    candidate[index] = rule;
    return replaceRules(std::move(candidate));
}

TransitionValidation TransitionTableDef::removeRule(int index)
{
    if (index < 0 || index >= static_cast<int>(rules.size()))
        return TransitionValidation::failure(
            TransitionError::RuleNotFound,
            QStringLiteral("Transition rule does not exist"), index);
    if (rules[index].isDefault())
        return TransitionValidation::failure(
            TransitionError::DefaultRuleRemoval,
            QStringLiteral("Default transition rule cannot be removed"), index);
    std::vector<TransitionRuleDef> candidate = rules;
    candidate.erase(candidate.begin() + index);
    return replaceRules(std::move(candidate));
}

namespace TransitionSemantics {

TransitionMountState transitionMountState(const ProjectItem& item, PlatformScope scope)
{
    if (item.type != ProjectItem::Type::Container)
        return TransitionMountState::None;
    const VSelection* selection = item.comp<VSelection>();
    if (!selection) return TransitionMountState::None;
    if (selection->isQueue())
        return TransitionMountState::Editable;
    if (selection->mode != SelectionMode::Match
        || selection->selections.table(scope).matchAxes.empty())
        return TransitionMountState::None;
    return selection->variableResponse() == VariableResponse::Live
        ? TransitionMountState::Editable : TransitionMountState::Idle;
}

bool isEndpointCandidate(const ProjectItem& container,
                         const ProjectItem& candidate)
{
    return container.type == ProjectItem::Type::Container
        && validateDirectPlaybackTarget(container, candidate.guid, &candidate);
}

int ruleSpecificity(const TransitionRuleDef& rule)
{
    return (rule.source ? 2 : 0) + (rule.target ? 1 : 0);
}

std::vector<int> rulesForDisplay(const TransitionTableDef& table)
{
    std::vector<int> order(table.rules.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
    std::stable_sort(order.begin(), order.end(), [&](int left, int right) {
        return ruleSpecificity(table.rules[left])
            > ruleSpecificity(table.rules[right]);
    });
    return order;
}

const TransitionRuleDef* matchRule(const TransitionTableDef& table,
                                   const QUuid& source,
                                   const QUuid& target,
                                   int* index)
{
    for (int candidate : rulesForDisplay(table)) {
        const TransitionRuleDef& rule = table.rules[candidate];
        if ((!rule.source || *rule.source == source)
            && (!rule.target || *rule.target == target)) {
            if (index) *index = candidate;
            return &rule;
        }
    }
    if (index) *index = -1;
    return nullptr;
}

TransitionWatchResult resolveWatchReference(
    const ProjectItem* source,
    const ProjectItem& container,
    const PlacementContext* placement,
    const TimelineItemResolver& resolve)
{
    TransitionWatchResult result;
    result.placementContextProvided = placement != nullptr;
    VisitSet visited;
    DownResult down = resolveDown(source, resolve, visited, {});
    if (down.segment) {
        result.referenceSegment = down.segment;
        result.direction = TransitionWatchDirection::Down;
        result.path = std::move(down.path);
        return result;
    }
    if (down.cycle) result.failureReason = QStringLiteral("Cycle detected while resolving downward");

    if (placement) {
        const auto scope = ReferenceSlotResolution::buildResolutionScope(
            &container, *placement, resolve, {});
        result.placementContextValid = scope.placementContextValid;
        if (!scope.placementContextValid)
            result.failureReason = scope.placementContextError;
        else {
            result.path.push_back(container.guid);
            for (const ProjectItem* candidate : scope.placementChain) {
                if (!candidate) continue;
                result.path.push_back(candidate->guid);
                if (candidate->type == ProjectItem::Type::Segment) {
                    result.referenceSegment = candidate;
                    result.direction = TransitionWatchDirection::UpPlacement;
                    return result;
                }
            }
            result.path.clear();
        }
    }

    VisitSet upwardVisited;
    result.path = {container.guid};
    upwardVisited.enter(&container);
    for (const ProjectItem* candidate = container.parent;
         candidate; candidate = candidate->parent) {
        if (!upwardVisited.enter(candidate)) {
            result.path.clear();
            result.failureReason = QStringLiteral("Cycle detected while resolving upward");
            return result;
        }
        result.path.push_back(candidate->guid);
        if (candidate->type == ProjectItem::Type::Segment) {
            result.referenceSegment = candidate;
            result.direction = TransitionWatchDirection::UpOrganization;
            return result;
        }
    }
    result.path.clear();
    if (result.failureReason.isEmpty())
        result.failureReason = placement
            ? QStringLiteral("No Segment exists in placement or organization scope")
            : QStringLiteral("No placement context; organization scope has no Segment");
    return result;
}

TransitionWatchView buildTransitionWatchView(
    const ProjectItem& container,
    const std::optional<QUuid>& sourceWinner,
    const PlacementContext* placement,
    const TimelineItemResolver& resolve)
{
    TransitionWatchView view;
    view.containerGuid = container.guid;
    view.placementContextProvided = placement != nullptr;

    if (!sourceWinner) {
        view.caption = QStringLiteral("no source — no clock lookup");
        return view;
    }
    view.hasSource = true;

    const ProjectItem* source = resolve ? resolve(*sourceWinner) : nullptr;
    const TransitionWatchResult result = resolveWatchReference(source, container, placement, resolve);
    view.direction = result.direction;
    view.path = result.path;
    view.placementContextValid = result.placementContextValid;
    view.failureReason = result.failureReason;

    if (result.found()) {
        view.referenceSegment = result.referenceSegment->guid;
        view.caption = watchPathNames(result.path, resolve);
    } else {
        view.caption = QStringLiteral("no reference — immediate + fade");
    }

    if (result.direction != TransitionWatchDirection::Down) {
        if (!view.placementContextProvided) {
            view.caption += QStringLiteral(" (no placement context; organization scope only)");
        } else if (!view.placementContextValid) {
            view.caption += QStringLiteral(" (placement context invalid: %1; fell back to organization scope)")
                .arg(view.failureReason);
        }
    }

    return view;
}

HandoverResult deriveHandover(const TransitionTableDef& table,
                              const std::optional<QUuid>& source,
                              const std::optional<QUuid>& target)
{
    HandoverResult result;
    if (!source) {
        result.kind = HandoverKind::SilentEntry;
        return result;
    }
    if (target && *source == *target) {
        result.kind = HandoverKind::SameObject;
        return result;
    }

    const TransitionRuleDef* rule = nullptr;
    if (target) {
        rule = matchRule(table, *source, *target, &result.ruleIndex);
    } else {
        for (int candidate : rulesForDisplay(table)) {
            const TransitionRuleDef& current = table.rules[candidate];
            if ((!current.source || *current.source == *source) && !current.target) {
                rule = &current;
                result.ruleIndex = candidate;
                break;
            }
        }
    }
    if (!rule) return result;

    result.kind = target ? HandoverKind::Transition : HandoverKind::SourceExit;
    result.ruleChain.push_back(*source);
    result.referenceChain.push_back(*source);
    if (target) {
        result.ruleChain.push_back(*target);
        result.referenceChain.push_back(*target);
    }
    return result;
}

}
}
