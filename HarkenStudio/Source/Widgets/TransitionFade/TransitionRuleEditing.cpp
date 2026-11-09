#include "TransitionRuleEditing.h"

#include "Core/ActionManager.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/Components/VSound.h"
#include "Core/Components/VSelection.h"
#include "Core/ItemTypeIcons.h"
#include "Core/LanguageContextService.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/SoundSourceEditing.h"
#include "Core/Timeline/TimelineCoordinates.h"
#include "Core/Timeline/TimelineSemantics.h"
#include "Core/TransitionFieldGating.h"

#include <QCoreApplication>
#include <QJsonObject>

#include <algorithm>

namespace VNX {

namespace TransitionRuleEditing {

const TransitionTableDef* table(const ProjectItem* container)
{
    const auto* selection = container ? container->comp<VSelection>() : nullptr;
    return selection ? &selection->transitions : nullptr;
}

const TransitionRuleDef* find(const ProjectItem* container, const TransitionEndpoints& endpoints)
{
    if (const TransitionTableDef* rules = table(container))
        for (const TransitionRuleDef& rule : rules->rules)
            if (rule.source == endpoints.first && rule.target == endpoints.second)
                return &rule;
    return nullptr;
}

bool commit(ProjectItem* container, const TransitionTableDef& candidate,
            const QString& undoText, QObject* source)
{
    if (!container)
        return false;
    return ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        QJsonObject{
            {QStringLiteral("authoring_handle"), static_cast<qint64>(container->objectID)},
            {QStringLiteral("property"), QStringLiteral("transitions")},
            {QStringLiteral("value"), candidate.toJson()},
            {QStringLiteral("undo_text"), undoText}},
        ActionSource::GUI, source).success;
}

bool update(ProjectItem* container, const TransitionEndpoints& endpoints,
            const std::function<void(TransitionRuleDef&)>& edit,
            const QString& undoText, QObject* source)
{
    const TransitionTableDef* rules = table(container);
    if (!rules)
        return false;
    for (size_t index = 0; index < rules->rules.size(); ++index) {
        TransitionRuleDef rule = rules->rules[index];
        if (rule.source != endpoints.first || rule.target != endpoints.second)
            continue;
        edit(rule);
        if (rule == rules->rules[index])
            return false;
        TransitionTableDef candidate = *rules;
        return candidate.updateRule(static_cast<int>(index), rule)
            && commit(container, candidate, undoText, source);
    }
    return false;
}

}

namespace {

constexpr double kPlaceholderBodySeconds = 4.0;
constexpr double kPlaceholderEdgeSeconds = 2.0;

struct Extent
{
    double pickup = kPlaceholderEdgeSeconds;
    double body = kPlaceholderBodySeconds;
    double tail = kPlaceholderEdgeSeconds;
};

double seconds(int64_t coordinate, const SegmentTimeSpec& time)
{
    return std::max(0.0, TimelineCoordinates::toSeconds(coordinate, time).value_or(0.0));
}

Extent describe(const std::optional<QUuid>& guid, TransitionPreviewClip* clip)
{
    Extent extent;
    if (!guid) {
        clip->name = QCoreApplication::translate("TransitionPreview", "* (Any)");
        return extent;
    }
    auto& manager = ProjectManager::instance();
    const ProjectItem* item = manager.findItemByGuid(*guid);
    if (!item) {
        clip->name = QCoreApplication::translate("TransitionPreview", "Missing");
        return extent;
    }
    clip->name = item->name;
    clip->icon = iconForItem(*item);
    if (item->color != 0)
        clip->color = QColor::fromRgba(item->color);
    if (item->comp<VSound>()) {
        const auto* source = SoundSourceEditing::activeSource(*item,
            PlatformContextService::instance().context().primary,
            SoundSourceEditing::previewLanguage(*item, LanguageContextService::instance().language()));
        const auto* asset = source ? AssetRegistry::instance().getAsset(source->sourceId) : nullptr;
        if (!asset || asset->absolutePath.isEmpty() || asset->metadata.totalSamples == 0
            || asset->metadata.sampleRate == 0)
            return extent;
        const SourceWaveformView view(*source, asset->metadata.totalSamples);
        if (view.endFrame() <= view.startFrame())
            return extent;
        clip->known = true;
        clip->waveformPath = asset->absolutePath;
        clip->waveformView = view;
        return {0.0, double(view.endFrame() - view.startFrame()) / asset->metadata.sampleRate, 0.0};
    }
    const TimelineSemanticsResult span = TimelineSemantics::nominalSpanOf(
        *item, [&manager](const QUuid& id) { return manager.findItemByGuid(id); });
    if (!span || span.nominal.end <= span.nominal.start)
        return extent;
    clip->known = true;
    return {seconds(span.pickup, span.time), seconds(span.nominal.end - span.nominal.start, span.time),
            seconds(span.tail, span.time)};
}

}

TransitionPreview buildTransitionPreview(const ProjectItem& container, const TransitionRuleDef& rule)
{
    const TransitionFieldGating gating = TransitionSemantics::gatingFor(container, rule, nullptr,
        [](const QUuid& guid) { return ProjectManager::instance().findItemByGuid(guid); });
    TransitionPreview preview;
    preview.fadeOut = rule.sourceFadeOut;
    preview.fadeIn = rule.targetFadeIn;

    const Extent source = describe(rule.source, &preview.source);
    const bool naturalEnd = rule.exitTiming == TransitionExitTiming::NaturalEnd
        || (rule.exitTiming == TransitionExitTiming::ExitMarker
            && gating.exitMarker.effect == FieldAvail::Unavailable);
    if (naturalEnd) {
        preview.source.start = -(source.pickup + source.body + source.tail);
        preview.source.end = 0.0;
    } else {
        preview.source.start = -(source.pickup + source.body);
        preview.source.end = source.tail;
    }
    preview.source.playedStart = preview.source.start;
    preview.source.playedEnd = rule.sourceTail && gating.sourceTail.effect != FieldAvail::Unavailable
        ? preview.source.end : 0.0;

    const Extent target = describe(rule.target, &preview.target);
    const bool entryMarker = rule.entryMode == TransitionEntryMode::EntryMarker
        && gating.entryMarker.effect != FieldAvail::Unavailable;
    preview.target.start = entryMarker ? -target.pickup : 0.0;
    preview.target.end = preview.target.start + target.pickup + target.body + target.tail;
    preview.target.playedStart = rule.targetLeadIn && gating.targetLeadIn.effect != FieldAvail::Unavailable
        ? preview.target.start : 0.0;
    preview.target.playedEnd = preview.target.end;
    return preview;
}

}
