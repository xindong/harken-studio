#include "AudioEngine.h"

#include "AttenuationStore.h"
#include "Components/VMixNode.h"
#include "Tooling/Harkenizer/Core/CurrentWorldProjectAdapter.h"
#include "Tooling/Harkenizer/Core/CanonicalRuntimeProjectAdapterPrivate.h"
#include "FieldGroupProjectAdapter.h"
#include "PlatformContextService.h"
#include "ProjectManager.h"
#include "RuntimeSyncCoordinator.h"
#include "RuntimeBridge/PreviewBridge.h"
#include "RuntimeBridge/CurrentWorldSyncBridge.h"

#include <algorithm>
#include <map>
#include <utility>

namespace VNX {

using LiveProperty = harkenplayableproperties::PlayablePropertyIdV1;
using LivePropertyKey = std::pair<uint8_t, harkenstudio::ObjectId>;
using LivePropertyValues = std::map<LivePropertyKey, float>;
using LiveEffectChain = std::vector<HarkenEditor::Bridge::PreviewVoiceEffectEntry>;

template<typename Key, typename Value>
bool updateCurvePreview(QHash<Key, QVector<std::pair<QUuid, Value>>>& previews,
                        const Key& key, const QUuid& previewId,
                        std::optional<Value> value)
{
    auto found = previews.find(key);
    if (!value && found == previews.end()) return false;
    if (found == previews.end()) found = previews.insert(key, {});
    auto& entries = *found;
    const auto entry = std::find_if(entries.begin(), entries.end(),
        [&previewId](const auto& candidate) { return candidate.first == previewId; });
    if (!value && entry == entries.end()) return false;
    if (entry != entries.end()) entries.erase(entry);
    if (value) entries.emplace_back(previewId, std::move(*value));
    if (entries.empty()) previews.erase(found);
    return true;
}

struct LivePropertyPreview {
    using Effect = HarkenEditor::Bridge::PreviewEffectSource;
    struct BusEffect {
        harkenstudio::ObjectId busId{};
        harkenstudio::ObjectId definitionId{};
        Effect effect;
    };

    struct Definition {
        float volumeDb = 0;
        float pitchCents = 0;
        LivePropertyValues fields;
        std::vector<Effect> effects;
        std::optional<LiveEffectChain> chain;

        void refreshMix(ProjectItem& item, const PlayablePropertyLocatorV1& locator,
                        PlatformScope scope)
        {
            const auto value = [&](LiveProperty property) {
                const auto state = PlayablePropertyAuthoringV1::readProperty(
                    item, locator, static_cast<uint8_t>(property), scope);
                return state && state->present ? static_cast<float>(state->value.toDouble()) : 0.0f;
            };
            volumeDb = value(LiveProperty::Volume);
            pitchCents = value(LiveProperty::Pitch);
        }
    };
    QHash<QUuid, DefinitionFieldGroups> groups;
    QHash<harkenstudio::ObjectId, Definition> definitions;
    QHash<uint64_t, LivePropertyValues> applied;
    QHash<uint64_t, std::map<harkenstudio::ObjectId, Effect>> appliedEffects;
    std::vector<BusEffect> busEffects;
    QHash<uint64_t, std::optional<LiveEffectChain>> appliedChains;
    QHash<QUuid, QVector<std::pair<QUuid, AttenuationSnapshot>>> attenuations;
    QSet<QUuid> pendingAttenuations;
    bool dirty = true;
};

void AudioEngine::bindProjectPropertyPreview(ProjectManager& project)
{
    if (livePropertyPreview_) return;
    livePropertyPreview_ = std::make_shared<LivePropertyPreview>();
    const auto invalidate = [this] { livePropertyPreview_->dirty = true; };
    const auto updateProperty = [this](ProjectItem* item, const QString& property) {
        if (property == QLatin1String("dirty")) return;
        auto& preview = *livePropertyPreview_;
        if (preview.dirty) return;
        if (item && (property == QLatin1String("volume")
            || property == QLatin1String("pitch"))) {
            const auto definition = preview.definitions.find(item->runtimeId());
            const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*item);
            if (definition != preview.definitions.end() && locator) {
                definition->refreshMix(*item, *locator,
                    currentWorldPlatformScope(PlatformContextService::instance().context()));
                return;
            }
        }
        preview.dirty = true;
    };
    connect(&project, &ProjectManager::itemPropertyPreviewed, this, updateProperty);
    connect(&project, &ProjectManager::itemPropertyChanged, this, updateProperty);
    connect(&project, &ProjectManager::itemCreated, this, invalidate);
    connect(&project, &ProjectManager::itemMoved, this, invalidate);
    connect(&project, &ProjectManager::itemDeleted, this, [this, &project] {
        auto& preview = *livePropertyPreview_;
        for (auto it = preview.groups.begin(); it != preview.groups.end();) {
            if (!project.findItemByGuid(it.key())) it = preview.groups.erase(it);
            else ++it;
        }
        preview.dirty = true;
    });
    connect(&project, &ProjectManager::fieldGroupsPreviewed, this,
        [this](ProjectItem* item, const DefinitionFieldGroups& groups) {
            if (!item) return;
            if (groups == item->fieldGroups) livePropertyPreview_->groups.remove(item->guid);
            else livePropertyPreview_->groups[item->guid] = groups;
            livePropertyPreview_->dirty = true;
        });
    connect(&project, &ProjectManager::bindingCurvePreviewed, this,
        [this](ProjectItem* item, harkenstudio::ObjectId id, const QUuid& previewId,
               const std::optional<ParameterBindingDef>& binding) {
            std::optional<BindingRuntimeProgram> program;
            if (binding) {
                auto* project = ProjectManager::instance().currentProject();
                if (!project || !item) return;
                BindingRuntimeLowering lowering;
                if (BindingRuntimeLoweringSemantics::appendLoweredBinding(
                        *project, *item, *binding, &lowering) != BindingLowerStatus::Ok) return;
                program = std::move(lowering.programs.front());
            }
            if (updateCurvePreview(runtimeBindingPreviews_, id, previewId, std::move(program)))
                runtimeBindingsPending_ = true;
        });
    connect(&project, &ProjectManager::attenuationCurvePreviewed, this,
        [this](const QUuid& guid, const QUuid& previewId,
               const std::optional<AttenuationSnapshot>& attenuation) {
            if (updateCurvePreview(livePropertyPreview_->attenuations, guid, previewId, attenuation))
                livePropertyPreview_->pendingAttenuations.insert(guid);
        });
    connect(&project, &ProjectManager::projectClosed, this, &AudioEngine::resetLivePropertyPreview);
}

void AudioEngine::resetLivePropertyPreview()
{
    runtimeBindingPreviews_.clear();
    runtimeBindingsPending_ = true;
    if (livePropertyPreview_) {
        auto pending = std::move(livePropertyPreview_->pendingAttenuations);
        for (auto it = livePropertyPreview_->attenuations.cbegin();
             it != livePropertyPreview_->attenuations.cend(); ++it)
            pending.insert(it.key());
        *livePropertyPreview_ = {};
        livePropertyPreview_->pendingAttenuations = std::move(pending);
    }
}

void AudioEngine::applyLivePropertyPreview()
{
    auto* project = ProjectManager::instance().currentProject();
    if (!livePropertyPreview_ || !runtimeSessionActive_ || !project) return;
    auto& preview = *livePropertyPreview_;
    for (auto it = preview.pendingAttenuations.begin(); it != preview.pendingAttenuations.end();) {
        const auto* authored = AttenuationStore::findByGuid(project, *it);
        const auto value = preview.attenuations.constFind(*it);
        if (!authored || AttenuationStore::syncToRuntime(*project,
                value == preview.attenuations.cend() ? authored->snapshot() : value->back().second))
            it = preview.pendingAttenuations.erase(it);
        else ++it;
    }
    std::vector<HarkenEditor::Bridge::PlaybackPathObservation> paths;
    if (!HarkenEditor::Bridge::capturePlaybackPaths(paths)) return;
    if (paths.empty()) {
        preview.applied.clear();
        preview.appliedChains.clear();
        preview.appliedEffects.clear();
    }
    if (preview.dirty) {
        const auto scope = currentWorldPlatformScope(PlatformContextService::instance().context());
        const auto snapshot = buildFieldGroupEffectiveSnapshot(*project, toFieldGroupScope(scope),
            [&preview](const ProjectItem& item) {
                const auto found = preview.groups.constFind(item.guid);
                return found == preview.groups.cend() ? &item.fieldGroups : &*found;
            });
        if (!snapshot.ok()) return;
        const auto sourceIndex = HarkenEditor::Bridge::canonicalRuntimeProjectSourceIndex(*project);
        if (!sourceIndex || !sourceIndex->valid) return;
        preview.definitions.clear();
        bool unresolvedEffects = false;
        for (const auto& [guid, groups] : snapshot.definitions) {
            auto* item = ProjectManager::instance().findItemByGuid(fromFieldGroupGuid(guid));
            const auto locator = item ? PlayablePropertyAuthoringV1::definitionLocator(*item) : std::nullopt;
            if (!locator) continue;
            auto& definition = preview.definitions[item->runtimeId()];
            definition.refreshMix(*item, *locator, scope);
            definition.fields = {
                {{static_cast<uint8_t>(LiveProperty::OutputBusVolume), {}}, groups.output.outputBusVolume},
                {{static_cast<uint8_t>(LiveProperty::SpatialPanning), {}}, groups.spatialRating.spatialPanning},
                {{static_cast<uint8_t>(LiveProperty::SpatializationMix), {}},
                    HarkenEditor::Bridge::resolvedStudioSpatializationMix(*item, *sourceIndex)},
                {{static_cast<uint8_t>(LiveProperty::SpatialAttenuationDistanceScaling), {}},
                    groups.spatialRating.spatialAttenuationDistanceScaling}};
            definition.chain.emplace();
            for (const auto& entry : groups.effects.effects.entries()) {
                const auto* effect = EffectChainSemantics::resolve(entry, *project);
                if (!effect || !EffectChainSemantics::validate(*effect)) {
                    definition.chain.reset();
                    unresolvedEffects = true;
                    break;
                }
                if (effect->type != EffectType::Limiter) {
                    definition.chain.reset();
                    break;
                }
                definition.chain->push_back({entry.runtimeId(), static_cast<uint32_t>(effect->type),
                    entry.bypass, effect->parameters[0], effect->parameters[1]});
            }
            for (const auto& send : groups.send.sends) {
                if (send.mode != SendMode::Mix) continue;
                const auto target = snapshot.objects.find(send.targetBusID);
                if (target != snapshot.objects.end())
                    definition.fields[{static_cast<uint8_t>(LiveProperty::Sends), target->second->runtimeId()}]
                        = send.sendLevel;
            }
            for (const auto& entry : groups.effects.effects.entries()) {
                const auto* effect = EffectChainSemantics::resolve(entry, *project);
                if (!effect) continue;
                const auto preset = sourceIndex->byQtGuid.find(entry.effectGuid);
                const auto presetId = entry.mode == EffectMode::Preset && preset != sourceIndex->byQtGuid.end()
                    ? preset->second->runtimeId() : harkenstudio::ObjectId{};
                definition.effects.push_back({entry.runtimeId(), presetId,
                    static_cast<uint32_t>(effect->type), effect->parameters, entry.bypass});
            }
        }
        preview.busEffects.clear();
        for (const auto* item : sourceIndex->buses) {
            const auto* mix = item->comp<VMixNode>();
            if (!mix) continue;
            for (const auto& entry : mix->effectsForPlatform(scope).entries()) {
                const auto* effect = EffectChainSemantics::resolve(entry, *project);
                if (!effect) continue;
                const auto definitionId = toFieldGroupGuid(
                    entry.mode == EffectMode::Preset ? entry.effectGuid : item->guid);
                preview.busEffects.push_back({item->runtimeId(), definitionId,
                    {entry.runtimeId(), {}, static_cast<uint32_t>(effect->type),
                        effect->parameters, entry.bypass}});
            }
        }
        preview.dirty = unresolvedEffects;
    }
    const auto installedWorld = runtimeSyncCoordinator().installedWorld();
    if (installedWorld)
        for (const auto& busEffect : preview.busEffects)
            (void)HarkenEditor::Bridge::updateLivePreviewBusEffect(installedWorld->runtimeEpoch,
                runtimeSyncCoordinator().currentAuthoringRevision(), busEffect.busId,
                busEffect.definitionId, busEffect.effect);
    QSet<uint64_t> active;
    for (const auto& path : paths) {
        active.insert(path.voiceInstance);
        (void)HarkenEditor::Bridge::refreshPreviewVoiceBindings(path.voiceInstance);
        const auto leaf = preview.definitions.constFind(path.soundId);
        if (leaf == preview.definitions.cend()) continue;
        auto effectState = preview.appliedEffects.find(path.voiceInstance);
        if (effectState == preview.appliedEffects.end()) {
            std::vector<HarkenEditor::Bridge::PreviewEffectSource> installed;
            if (HarkenEditor::Bridge::captureInstalledPreviewEffects(
                    path.worldGeneration, path.soundId, installed)) {
                effectState = preview.appliedEffects.insert(path.voiceInstance, {});
                for (auto& effect : installed) effectState->emplace(effect.id, std::move(effect));
            }
        }
        for (const auto& effect : leaf->effects) {
            if (leaf->chain) break;
            if (effectState == preview.appliedEffects.end()) break;
            auto& appliedEffects = *effectState;
            const auto previous = appliedEffects.find(effect.id);
            if (previous == appliedEffects.end()) continue;
            auto next = previous->second;
            if (next.type == effect.type && next.presetId == effect.presetId)
                next.parameters = effect.parameters;
            next.bypass = effect.bypass;
            if (next == previous->second) continue;
            if (HarkenEditor::Bridge::setPreviewVoiceEffect(path.voiceInstance, effect.id,
                    next.type, next.parameters, next.bypass) == HarkenEditor::Bridge::kEngineOk)
                previous->second = std::move(next);
        }
        auto values = leaf->fields;
        float volumeDb = 0;
        float pitchCents = 0;
        bool complete = true;
        for (const auto& row : path.rows) {
            if (row.occurrenceOrdinal != UINT32_MAX) continue;
            const auto definition = preview.definitions.constFind(row.stableId);
            if (definition == preview.definitions.cend()) { complete = false; break; }
            volumeDb += definition->volumeDb;
            pitchCents += definition->pitchCents;
        }
        if (!complete) continue;
        values[{static_cast<uint8_t>(LiveProperty::Volume), {}}] = volumeDb;
        values[{static_cast<uint8_t>(LiveProperty::Pitch), {}}] = std::clamp(pitchCents, -7200.0f, 7200.0f);
        auto& acceptedChain = preview.appliedChains[path.voiceInstance];
        if (leaf->chain && acceptedChain != leaf->chain) {
            const auto result = HarkenEditor::Bridge::setPreviewVoiceEffectChain(
                path.voiceInstance, *leaf->chain);
            if (result == HarkenEditor::Bridge::kEngineOk) acceptedChain = leaf->chain;
        }
        auto& applied = preview.applied[path.voiceInstance];
        for (const auto& [key, value] : values) {
            const auto previous = applied.find(key);
            if (previous != applied.end() && previous->second == value) continue;
            if (key.first == static_cast<uint8_t>(LiveProperty::Sends)
                && std::none_of(path.sends.begin(), path.sends.end(),
                    [&key](const auto& send) { return send.targetBusId == key.second; })) continue;
            const auto result = HarkenEditor::Bridge::setPreviewVoiceProperty(
                path.voiceInstance, key.first, value, key.second);
            if (result == HarkenEditor::Bridge::kEngineOk) applied[key] = value;
        }
    }
    for (auto it = preview.applied.begin(); it != preview.applied.end();) {
        if (!active.contains(it.key())) it = preview.applied.erase(it);
        else ++it;
    }
    for (auto it = preview.appliedChains.begin(); it != preview.appliedChains.end();) {
        if (!active.contains(it.key())) it = preview.appliedChains.erase(it);
        else ++it;
    }
    for (auto it = preview.appliedEffects.begin(); it != preview.appliedEffects.end();) {
        if (!active.contains(it.key())) it = preview.appliedEffects.erase(it);
        else ++it;
    }
}

}
