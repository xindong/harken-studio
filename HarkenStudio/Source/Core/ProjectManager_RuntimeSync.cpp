
#include "ProjectManager.h"
#include "SoundSourceSemantics.h"
#include "CanonicalRuntimeProjectAdapter.h"
#include "CurrentWorldProjectAdapter.h"
#include "FieldGroupProjectAdapter.h"
#include "LanguageContextService.h"
#include "PlatformContextService.h"
#include "SoundSourceEditing.h"
#include "SoundSourceDerivative.h"
#include "Components/VSound.h"
#include "AttenuationStore.h"
#include "ReferenceSlotResolution.h"
#include "AudioEngine.h"
#include "RuntimeSyncCoordinator.h"
#include "HarkenMath.h"
#include "Components/VAudio.h"
#include "Components/VMixNode.h"
#include "Components/VSpatial.h"
#include "Components/VBus.h"
#include "RuntimeBridge/ProjectSyncBridge.h"
#include "RuntimeBridge/CurrentWorldSyncBridge.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"
#include "RuntimeBridge/PreviewBridge.h"
#include "AssetPipeline/AssetTypes.h"
#include "AssetPipeline/AssetRegistry.h"
#include "../Widgets/LogPanel/LogPanel.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QTimer>
#include <QDebug>
#include <iterator>
#include <algorithm>

namespace VNX {

namespace Bridge = HarkenEditor::Bridge;

namespace {
void startupTrace(const QString& message)
{
    if (!qEnvironmentVariableIsSet("HARKEN_STARTUP_TRACE"))
        return;
    QDir().mkpath(QStringLiteral(".temp"));
    QFile f(QStringLiteral(".temp/editor_startup_trace.log"));
    if (!f.open(QIODevice::Append | QIODevice::Text))
        return;
    const QString line = QStringLiteral("%1 %2\n")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), message);
    f.write(line.toUtf8());
}

bool busPublicationSupported(const ProjectData& project)
{
    const auto scope = currentWorldPlatformScope(
        PlatformContextService::instance().context());
    const auto diagnostics = Bridge::validateBusPublicationSupport(
        project, scope.value_or(ProjectPlatformId{}));
    if (diagnostics.empty())
        return true;
    emit ProjectManager::instance().errorOccurred(diagnostics.front().message);
    return false;
}

void supersedePendingCurrentWorld(RuntimeSyncCoordinator& coordinator)
{
    const auto superseded = Bridge::discardPendingCurrentWorld(
        Bridge::PendingCurrentWorldDisposition::Superseded);
    if (superseded)
        coordinator.acceptCurrentWorldSyncResult(*superseded);
}

}

void ProjectManager::initializeRuntimeSync()
{
    auto& audioEngine = AudioEngine::instance();
    audioEngine.bindVariableRehearsal(m_variableRehearsal);
    audioEngine.bindProjectPropertyPreview(*this);

    auto& assets = AssetRegistry::instance();
    connect(this, &ProjectManager::projectClosed, this, [this] {
        m_runtimeSoundPreparation.clear();
    });
    connect(this, &ProjectManager::itemDeleted, this, [this](ProjectItem* item) {
        const auto remove = [this](const ProjectItem* node, const auto& self) -> void {
            m_runtimeSoundPreparation.remove(node->runtimeId());
            for (const auto& child : node->children) self(child.get(), self);
        };
        if (item) remove(item, remove);
    });
    connect(&assets, &AssetRegistry::assetModified, this, &ProjectManager::resyncSourceToRuntime, Qt::QueuedConnection);
    connect(&assets, &AssetRegistry::assetRegistered, this, &ProjectManager::resyncSourceToRuntime, Qt::QueuedConnection);
    connect(&assets, &AssetRegistry::assetUnregistered, this, &ProjectManager::resyncSourceToRuntime, Qt::QueuedConnection);

    connect(this, &ProjectManager::projectOpened, this,
            [this](ProjectData* project) {
        m_pendingRuntimePreparation.reset();
        if (!runtimePrepared())
            syncProjectToRuntime(project);
    }, Qt::DirectConnection);

    const auto needsParentResync = [](ProjectItem* item) {
        return item != nullptr && item->type == ProjectItem::Type::Layer;
    };
    connect(this, &ProjectManager::itemCreated, this,
            [needsParentResync](ProjectItem* item) {
        if (item == nullptr)
            return;
        resyncItemToRuntime(item);
        if (needsParentResync(item->parent))
            resyncItemToRuntime(item->parent);
    });
    connect(this, &ProjectManager::itemMoved, this,
            [needsParentResync](ProjectItem* item, ProjectItem* oldParent,
                                ProjectItem* newParent) {
        if (item == nullptr)
            return;
        resyncItemToRuntime(item);
        if (needsParentResync(oldParent))
            resyncItemToRuntime(oldParent);
        if (needsParentResync(newParent))
            resyncItemToRuntime(newParent);
    });
    connect(this, &ProjectManager::itemDeleted, this,
            [](ProjectItem* item) {
        if (item == nullptr)
            return;
        unregisterItemFromRuntime(item);
        if (item->parent != nullptr
            && item->parent->type == ProjectItem::Type::Layer) {
            resyncItemToRuntime(item->parent);
            return;
        }
        const auto subtreeAffectsCurrentWorld =
            [](const ProjectItem* root, const auto& self) -> bool {
            if (root == nullptr)
                return false;
            if (affectsCurrentWorld(
                    root->type, CurrentWorldMutationKind::Structure)) {
                return true;
            }
            for (const auto& child : root->children) {
                if (self(child.get(), self))
                    return true;
            }
            return false;
        };
        if (subtreeAffectsCurrentWorld(item, subtreeAffectsCurrentWorld)) {
            AudioEngine::instance().runtimeSyncCoordinator()
                .markCurrentWorldMutation();
        }
    });
    connect(this, &ProjectManager::itemRenamed, this,
            [](ProjectItem* item, const QString&) {
        if (item != nullptr)
            resyncItemToRuntime(item);
    });
    connect(this, &ProjectManager::itemPropertyChanged, this,
            [](ProjectItem* item, const QString& propertyName) {
        if (item == nullptr)
            return;
        static const QSet<QString> objectRegistryProperties = {
            QStringLiteral("streamingMode"),
            QStringLiteral("streaming_mode"),
            QStringLiteral("outputBus"),
            QStringLiteral("output_bus"),
            QStringLiteral("outputBusId"),
            QStringLiteral("is3DEnabled"),
            QStringLiteral("attenuationRef"),
            QStringLiteral("pasteProperties"),
            QStringLiteral("fieldGroups"),
        };
        if (objectRegistryProperties.contains(propertyName)) {
            resyncItemToRuntime(item);
        } else if (affectsCurrentWorld(item->type,
                                      CurrentWorldMutationKind::Property,
                                      propertyName)) {
            AudioEngine::instance().runtimeSyncCoordinator()
                .markCurrentWorldMutation();
        }
    });
    connect(&audioEngine.runtimeSyncCoordinator(),
            &RuntimeSyncCoordinator::barrierChanged,
            this, &ProjectManager::finishRuntimeCloseIfReady,
            Qt::QueuedConnection);
    connect(&audioEngine.runtimeSyncCoordinator(),
            &RuntimeSyncCoordinator::barrierChanged,
            this, &ProjectManager::finishRuntimePreparationIfReady);
}

static harkenstudio::ObjectId runtimeStableIdForItem(const ProjectItem* item)
{
    return item ? item->runtimeId() : harkenstudio::ObjectId{};
}

static std::optional<Bridge::SoundDesc> soundDescriptor(
    const ProjectItem* item, harkenstudio::ObjectId parentRuntimeId,
    const FieldGroupEffectiveSnapshot& snapshot)
{
    auto* ac = item->comp<VAudio>();
    if (!ac)
    {
        return std::nullopt;
    }

    const auto platform = PlatformContextService::instance().context().primary;
    const QString language = SoundSourceEditing::previewLanguage(
        *item, LanguageContextService::instance().language());

    const auto effective = snapshot.definitions.find(toFieldGroupGuid(item->guid));
    if (effective == snapshot.definitions.end())
        return std::nullopt;
    const auto& rating = effective->second.spatialRating;

    Bridge::SoundDesc desc;
    desc.objectId = runtimeStableIdForItem(item);
    desc.parentObjectId = parentRuntimeId;
    desc.name = item->name;
    desc.soundId = desc.objectId;
    const uint32_t sourceID = SoundSourceEditing::activeSourceId(
        *item, platform, language);
    desc.sourceId = sourceID;
    desc.loopCount = ac->loopCount;
    desc.spatialized3D = rating.spatialIs3DEnabled;
    desc.streamingMode = static_cast<uint8_t>(ac->streamingMode);
    if (!harkenprojectschema::IsNull(rating.spatialAttenuationRef))
        desc.attenuationId = snapshot.objects.at(rating.spatialAttenuationRef)->runtimeId();
    return desc;
}

static harkenstudio::ObjectId syncSound(const ProjectItem* item, harkenstudio::ObjectId parentRuntimeId, AudioEngine&,
                          const FieldGroupEffectiveSnapshot& snapshot)
{
    const auto desc = soundDescriptor(item, parentRuntimeId, snapshot);
    if (!desc || !Bridge::syncSound(*desc))
        return {};

    if (desc->sourceId != 0)
    {
        const AssetInfo* asset = AssetRegistry::instance().getAsset(desc->sourceId);
        if (asset && !asset->absolutePath.isEmpty())
        {
            Bridge::mediaRegistryRegister(desc->sourceId, asset->absolutePath);
        }
    }
    return desc->objectId;
}

static harkenstudio::ObjectId syncLayer(const ProjectItem* item, harkenstudio::ObjectId parentRuntimeId, AudioEngine&,
                          const FieldGroupEffectiveSnapshot& snapshot)
{
    const auto effective = snapshot.definitions.find(toFieldGroupGuid(item->guid));
    if (effective == snapshot.definitions.end())
        return {};
    const auto& rating = effective->second.spatialRating;

    Bridge::LayerDesc desc;
    desc.objectId = runtimeStableIdForItem(item);
    desc.parentObjectId = parentRuntimeId;
    desc.name = item->name;
    desc.spatialized3D = rating.spatialIs3DEnabled;
    if (!harkenprojectschema::IsNull(rating.spatialAttenuationRef))
        desc.attenuationId = snapshot.objects.at(rating.spatialAttenuationRef)->runtimeId();

    if (!Bridge::syncLayer(desc))
        return {};
    return desc.objectId;
}


using SyncHandler = harkenstudio::ObjectId (*)(const ProjectItem*, harkenstudio::ObjectId, AudioEngine&,
                                 const FieldGroupEffectiveSnapshot&);

enum class RuntimeTeardown : uint8_t { None, RemoveObject, RemoveActionName };

// Single source of truth for per-type runtime presence: tree-sync dispatch,
// hasRuntimePresence, and unregisterItemFromRuntime all derive from this
// table. Bus and Attenuation materialize through their own dedicated paths
// and intentionally have no row semantics here; Action contributes only a
// Preview name mapping and tears that mapping down through this table.
struct RuntimePresenceEntry {
    ProjectItem::Type type;
    SyncHandler treeSyncHandler;
    RuntimeTeardown teardown;
};

static constexpr RuntimePresenceEntry s_runtimePresence[] = {
    { ProjectItem::Type::None,      nullptr,         RuntimeTeardown::None },
    { ProjectItem::Type::Sound,       &syncSound,      RuntimeTeardown::RemoveObject },
    { ProjectItem::Type::Container,   nullptr,         RuntimeTeardown::None },
    { ProjectItem::Type::Track,       nullptr,         RuntimeTeardown::None },
    { ProjectItem::Type::Piece,       nullptr,         RuntimeTeardown::None },
    { ProjectItem::Type::Layer,       &syncLayer, RuntimeTeardown::RemoveObject },
    { ProjectItem::Type::Bus,         nullptr,         RuntimeTeardown::None },
    { ProjectItem::Type::Action,      nullptr,         RuntimeTeardown::RemoveActionName },
    { ProjectItem::Type::Variable,    nullptr,         RuntimeTeardown::None },
    { ProjectItem::Type::Attenuation, nullptr,         RuntimeTeardown::None },
};

static_assert(std::size(s_runtimePresence) == 10,
              "runtime presence table must cover every ProjectItem::Type");
static_assert([] {
    for (size_t i = 0; i < std::size(s_runtimePresence); ++i)
        if (static_cast<size_t>(s_runtimePresence[i].type) != i)
            return false;
    return true;
}(), "runtime presence table rows must follow the type enum order");

static const RuntimePresenceEntry& runtimePresenceEntry(ProjectItem::Type type)
{
    return s_runtimePresence[static_cast<size_t>(type)];
}

static SyncHandler findSyncHandler(ProjectItem::Type type)
{
    return runtimePresenceEntry(type).treeSyncHandler;
}


static void syncGeneralAudioProperties(ProjectItem* item, harkenstudio::ObjectId objectId,
                                        const FieldGroupEffectiveSnapshot& snapshot)
{
    auto& manager = ProjectManager::instance();
    auto& engine = AudioEngine::instance();
    const auto& groups = snapshot.definitions.at(toFieldGroupGuid(item->guid));
    if (item->comp<VMixNode>())
        manager.pushItemPitchLive(item, snapshot);
    Bridge::setObjectOutputBus(objectId,
        manager.busRuntimeHandleForId(
            snapshot.objects.at(groups.mainOutput.mainOutputBusRef)->objectID));
    engine.setVoiceVolumeByName(item->name, manager.aggregateVolumeDbForItem(item));
    engine.setVoiceOutputVolumeByName(item->name, groups.mainOutput.outputBusVolume);
    if (item->type == ProjectItem::Type::Sound) {
        std::vector<std::pair<harkenstudio::ObjectId, float>> sends;
        for (const auto& send : groups.send.sends)
            sends.emplace_back(manager.busRuntimeHandleForId(
                snapshot.objects.at(send.targetBusID)->objectID), send.sendLevel);
        engine.setVoiceSendsByName(item->name, sends);
    }

    if (item->comp<VSpatial>())
    {
        if (item->type == ProjectItem::Type::Sound)
            engine.setVoiceDistanceScaleByName(
                item->name, groups.spatialRating.spatialAttenuationDistanceScaling);
    }
}


static harkenstudio::ObjectId syncItemToRuntimeOne(ProjectItem* item, harkenstudio::ObjectId parentRuntimeId,
                                     const FieldGroupEffectiveSnapshot& snapshot)
{
    if (!item)
        return parentRuntimeId;

    if (item->objectID == 0)
        return parentRuntimeId;

    auto& engine = AudioEngine::instance();
    harkenstudio::ObjectId createdId = {};
    if (SyncHandler handler = findSyncHandler(item->type))
        createdId = handler(item, parentRuntimeId, engine, snapshot);
    if (!harkenstudio::IsNull(createdId))
        syncGeneralAudioProperties(item, createdId, snapshot);
    if (!harkenstudio::IsNull(createdId) && !harkenstudio::IsNull(parentRuntimeId))
        Bridge::addLayerChild(parentRuntimeId, createdId);
    return !harkenstudio::IsNull(createdId) ? createdId : parentRuntimeId;
}

static void syncItemToRuntime(ProjectItem* item, harkenstudio::ObjectId parentRuntimeId,
                               const FieldGroupEffectiveSnapshot& snapshot)
{
    if (!item) return;

    const harkenstudio::ObjectId childParentId = syncItemToRuntimeOne(item, parentRuntimeId, snapshot);
    for (const auto& child : item->children)
        syncItemToRuntime(child.get(), childParentId, snapshot);
}

bool ProjectManager::hasRuntimePresence(const ProjectItem* item)
{
    return item &&
           runtimePresenceEntry(item->type).teardown != RuntimeTeardown::None;
}

void ProjectManager::unregisterItemFromRuntime(ProjectItem* item)
{
    if (!item) return;
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());

    for (const auto& child : item->children)
        unregisterItemFromRuntime(child.get());

    switch (runtimePresenceEntry(item->type).teardown) {
        case RuntimeTeardown::RemoveActionName:
            Bridge::unregisterActionNameForPreview(runtimeStableIdForItem(item));
            return;
        case RuntimeTeardown::RemoveObject:
            AudioEngine::instance().removeRuntimeObject(runtimeStableIdForItem(item));
            return;
        case RuntimeTeardown::None:
            return;
    }
}

void ProjectManager::resyncRefreshedSourceToRuntime(uint32_t sourceId)
{
    auto& manager = instance();
    const auto* coordinator = manager.m_currentProject && !manager.m_closing
        ? &AudioEngine::instance().runtimeSyncCoordinator() : nullptr;
    if (coordinator && coordinator->barrier(coordinator->currentAuthoringRevision()).state
        == RuntimeSyncCoordinator::RevisionBarrierState::Ready) {
        const auto platform = PlatformContextService::instance().context().primary;
        const auto language = LanguageContextService::instance().language();
        bool referenced = false;
        bool current = true;
        for (auto* item : ProjectPlatformSemantics::referenceOwners(*manager.m_currentProject)) {
            const auto* sound = item->comp<VSound>();
            if (!sound) continue;
            bool usesSource = false;
            for (const auto* set : SoundSourceSemantics::sourceSets(sound->sources)) {
                usesSource |= std::any_of(set->entries.begin(), set->entries.end(),
                    [sourceId](const auto& source) { return source.sourceId == sourceId; });
            }
            if (!usesSource) continue;
            referenced = true;
            const auto* source = SoundSourceEditing::activeSource(
                *item, platform, SoundSourceEditing::previewLanguage(*item, language));
            const auto* asset = AssetRegistry::instance().getAsset(sourceId);
            const auto cached = manager.m_runtimeSoundPreparation.constFind(item->runtimeId());
            if (!source || source->sourceId != sourceId || !asset || asset->contentHash.isEmpty()
                || cached == manager.m_runtimeSoundPreparation.cend()) {
                current = false;
                break;
            }
            const QFileInfo file(asset->absolutePath);
            const RuntimeSoundPreparationKey key{
                asset->absolutePath, asset->contentHash, file.size(),
                file.lastModified().toMSecsSinceEpoch(), *source, sound->scheduleOffsetSec};
            if (!file.isFile() || cached->key != key || cached->revision == 0
                || Bridge::registeredSoundRevision(item->runtimeId()) != cached->revision
                || !Bridge::runtimeObjectExists(item->runtimeId())) {
                current = false;
                break;
            }
        }
        if (referenced && current) return;
    }
    resyncSourceToRuntime(sourceId);
}

void ProjectManager::resyncSourceToRuntime(uint32_t sourceId)
{
    auto& manager = instance();
    for (auto it = manager.m_runtimeSoundPreparation.begin();
         it != manager.m_runtimeSoundPreparation.end();) {
        if (it->key.source.sourceId == sourceId)
            it = manager.m_runtimeSoundPreparation.erase(it);
        else
            ++it;
    }
    if (!manager.m_currentProject || manager.m_closing)
        return;
    for (auto* item : ProjectPlatformSemantics::referenceOwners(*manager.m_currentProject)) {
        const auto* sound = item->comp<VSound>();
        if (!sound) continue;
        for (const auto* set : SoundSourceSemantics::sourceSets(sound->sources)) {
            if (std::any_of(set->entries.begin(), set->entries.end(),
                    [sourceId](const auto& source) { return source.sourceId == sourceId; })) {
                AudioEngine::instance().runtimeSyncCoordinator().markCurrentWorldMutation();
                return;
            }
        }
    }
}

void ProjectManager::resyncItemToRuntime(ProjectItem* item)
{
    resyncItemsToRuntime({&item, 1});
}

void ProjectManager::resyncItemsToRuntime(std::span<ProjectItem* const> items)
{
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());

    std::optional<FieldGroupEffectiveSnapshot> snapshot;
    for (auto* item : items) {
        if (!item || item->objectID == 0) continue;

        if (item->type == ProjectItem::Type::Action)
        {
            syncActionNameForPreview(item);
            AudioEngine::instance().runtimeSyncCoordinator().markCurrentWorldMutation();
            continue;
        }

        const harkenstudio::ObjectId runtimeID = runtimeStableIdForItem(item);
        auto& ae = AudioEngine::instance();
        const auto* project = ProjectManager::instance().currentProject();
        if (!project)
            continue;
        if (!snapshot)
            snapshot = buildFieldGroupEffectiveSnapshot(
                *project, toFieldGroupScope(currentWorldPlatformScope(PlatformContextService::instance().context())));
        if (!snapshot->ok()) {
            ae.runtimeSyncCoordinator().markCurrentWorldMutation();
            continue;
        }
        ae.removeRuntimeObject(runtimeID);

        harkenstudio::ObjectId parentRuntimeId = {};
        ProjectItem* parentItem = item->parent;
        while (parentItem) {
            if (!harkenstudio::IsNull(parentItem->runtimeId())) {
                const harkenstudio::ObjectId candidateId = runtimeStableIdForItem(parentItem);
                if (Bridge::runtimeObjectExists(candidateId)) {
                    parentRuntimeId = candidateId;
                    break;
                }
            }
            parentItem = parentItem->parent;
        }

        syncItemToRuntime(item, parentRuntimeId, *snapshot);

        if (findSyncHandler(item->type))
            qDebug() << "[IncrementalSync] Re-synced" << item->name
                     << "(objectID:" << item->objectID << ") to Runtime";
        else
            qDebug() << "[IncrementalSync] Skipped (no sync handler)" << item->name
                     << "(objectID:" << item->objectID << ")";

        if (affectsCurrentWorld(item->type,
                                CurrentWorldMutationKind::Structure)) {
            const auto allocation =
                ae.runtimeSyncCoordinator().markCurrentWorldMutation();
            if (allocation.state
                == RuntimeSyncCoordinator::AllocationState::IdentityExhausted) {
                qCritical()
                    << "[CurrentWorldSync] authoring revision identity exhausted";
            }
        }
    }
}

void ProjectManager::syncActionNameForPreview(ProjectItem* actionItem)
{
    if (!actionItem || actionItem->type != ProjectItem::Type::Action) return;

    Bridge::ActionNameMapping mapping;
    mapping.id = runtimeStableIdForItem(actionItem);
    mapping.name = actionItem->name;

    if (!Bridge::registerActionNameForPreview(mapping))
    {
        qWarning() << "[PreviewActionName] registration failed:" << actionItem->name;
        return;
    }

    qDebug() << "[PreviewActionName]" << actionItem->name << "-> stable id";
}

bool ProjectManager::prepareRuntimeSounds(const ProjectData& project,
    Bridge::CurrentWorldSoundPreparation& preparation, QString* error)
{
    const auto snapshot = buildFieldGroupEffectiveSnapshot(
        project, toFieldGroupScope(currentWorldPlatformScope(PlatformContextService::instance().context())));
    return prepareRuntimeSounds(snapshot, preparation, error);
}

bool ProjectManager::prepareRuntimeSounds(const FieldGroupEffectiveSnapshot& snapshot,
    Bridge::CurrentWorldSoundPreparation& preparation, QString* error)
{
    if (!snapshot.ok()) {
        if (error && !snapshot.diagnostics.empty())
            *error = snapshot.diagnostics.front().message;
        return false;
    }
    const auto platform = PlatformContextService::instance().context().primary;
    preparation = {};
    for (const auto& [guid, item] : snapshot.objects) {
        if (item->type != ProjectItem::Type::Sound)
            continue;
        auto& revision = preparation.contentRevisions[item->runtimeId()];
        const auto* sound = item->comp<VSound>();
        const auto* source = SoundSourceEditing::activeSource(
            *item, platform, SoundSourceEditing::previewLanguage(
                *item, LanguageContextService::instance().language()));
        const auto* asset = source ? AssetRegistry::instance().getAsset(source->sourceId) : nullptr;
        QString mediaError;
        const auto binding = soundDescriptor(item, {}, snapshot);
        if (sound && asset && binding && binding->sourceId != 0) {
            const QFileInfo file(asset->absolutePath);
            const RuntimeSoundPreparationKey key{
                asset->absolutePath, asset->contentHash, file.size(),
                file.lastModified().toMSecsSinceEpoch(), *source,
                sound->scheduleOffsetSec};
            auto& cache = instance().m_runtimeSoundPreparation;
            const auto cached = cache.constFind(item->runtimeId());
            if (file.isFile() && cached != cache.cend() && cached->key == key
                && cached->revision != 0
                && Bridge::registeredSoundRevision(item->runtimeId()) == cached->revision) {
                revision = cached->revision;
                preparation.sounds.push_back({*binding, asset->absolutePath, {}});
                continue;
            }
            auto media = SoundSourceDerivative::prepare(
                asset->absolutePath, *source, sound->scheduleOffsetSec,
                ConversionSettings{}, &mediaError);
            if (media) {
                revision = Bridge::soundPcmContentRevision(
                    media->samples, media->channels, media->sampleRate,
                    media->loopStartFrame, media->loopEndFrame);
                if (revision != 0) {
                    cache[item->runtimeId()] = {key, revision};
                    preparation.sounds.push_back({*binding, asset->absolutePath,
                        std::move(media->samples), media->channels, media->sampleRate,
                        media->loopStartFrame, media->loopEndFrame});
                }
            }
        }
        if (revision == 0) {
            if (source) {
                const QString reason = QStringLiteral("Sound '%1' is unavailable for preview: %2")
                    .arg(item->name, mediaError.isEmpty()
                        ? QStringLiteral("source media is unavailable") : mediaError);
                LogPanel::logWarning(LogCategory::RuntimeSync, reason);
            }
        }
    }
    return true;
}

void ProjectManager::syncProjectToRuntimeAsync(ProjectData* project,
                                                QObject* context,
                                                std::function<void()> finished)
{
    if (!project || !context) {
        if (finished)
            finished();
        return;
    }

    auto& manager = ProjectManager::instance();
    const auto projectLease = manager.currentProjectLease();
    if (projectLease.get() != project) {
        if (finished)
            finished();
        return;
    }
    if (!busPublicationSupported(*project)) {
        if (finished)
            finished();
        return;
    }
    auto snapshot = buildFieldGroupEffectiveSnapshot(
        *project, toFieldGroupScope(currentWorldPlatformScope(PlatformContextService::instance().context())));
    if (!snapshot.ok()) {
        emit manager.errorOccurred(QStringLiteral(
            "Project field groups are invalid; Runtime sync was not started"));
        if (finished)
            finished();
        return;
    }
    manager.m_runtimePrepared = false;
    manager.m_pendingRuntimePreparation.reset();
    auto& engine = AudioEngine::instance();
    engine.runtimeSyncCoordinator().beginSubmissionBatch();
    supersedePendingCurrentWorld(engine.runtimeSyncCoordinator());
    engine.clearRuntimeObjects();
    engine.clearRuntimeMedia();

    struct PendingNode {
        ProjectItem* item = nullptr;
        harkenstudio::ObjectId parentRuntimeId = {};
    };
    struct State {
        std::shared_ptr<ProjectData> project;
        FieldGroupEffectiveSnapshot snapshot;
        QPointer<QObject> context;
        RuntimeSyncCoordinator::ProjectSessionId projectSessionId = 0;
        RuntimeSyncCoordinator::RuntimeEpoch runtimeEpoch = 0;
        std::function<void()> finished;
        QList<PendingNode> nodes;
        QList<ProjectItem*> actions;
        std::vector<const VAttenuation*> attenuations;
        int attenuationIndex = 0;
        int actionIndex = 0;
        std::shared_ptr<std::function<void()>> step;
        bool submissionBatchHeld = true;
        QMetaObject::Connection contextDestroyedConnection;
    };

    auto state = std::make_shared<State>();
    state->project = projectLease;
    state->snapshot = std::move(snapshot);
    state->context = context;
    const auto initialBarrier = engine.runtimeSyncCoordinator().barrier(0);
    state->projectSessionId = initialBarrier.projectSessionId;
    state->runtimeEpoch = initialBarrier.runtimeEpoch;
    state->finished = std::move(finished);
    state->attenuations = AttenuationStore::all(*project);
    for (auto* binding : project->allFileBindings()) {
        if (!binding || !binding->rootItem)
            continue;
        state->nodes.append(PendingNode{binding->rootItem.get(), {}});
        std::function<void(ProjectItem*)> collectActions = [&](ProjectItem* item) {
            if (!item)
                return;
            if (item->type == ProjectItem::Type::Action && item->objectID != 0)
                state->actions.append(item);
            for (const auto& child : item->children)
                collectActions(child.get());
        };
        collectActions(binding->rootItem.get());
    }

    state->step = std::make_shared<std::function<void()>>();
    *state->step = [state]() {
        if (!state->context) {
            if (state->submissionBatchHeld) {
                AudioEngine::instance().runtimeSyncCoordinator()
                    .endSubmissionBatch();
                state->submissionBatchHeld = false;
            }
            QObject::disconnect(state->contextDestroyedConnection);
            state->step.reset();
            return;
        }
        auto& manager = ProjectManager::instance();
        auto& coordinator = AudioEngine::instance().runtimeSyncCoordinator();
        const auto barrier = coordinator.barrier(0);
        if (manager.currentProject() != state->project.get()
            || barrier.projectSessionId != state->projectSessionId
            || barrier.runtimeEpoch != state->runtimeEpoch) {
            if (state->submissionBatchHeld) {
                coordinator.endSubmissionBatch();
                state->submissionBatchHeld = false;
            }
            QObject::disconnect(state->contextDestroyedConnection);
            state->step.reset();
            return;
        }
        QElapsedTimer budget;
        budget.start();
        while (budget.elapsed() < 6) {
            if (state->attenuationIndex < static_cast<int>(state->attenuations.size())) {
                AttenuationStore::syncToRuntime(
                    *state->project,
                    *state->attenuations.at(static_cast<size_t>(state->attenuationIndex++)));
                continue;
            }
            if (!state->nodes.isEmpty()) {
                const PendingNode pending = state->nodes.takeFirst();
                const harkenstudio::ObjectId childParentId =
                    syncItemToRuntimeOne(pending.item, pending.parentRuntimeId, state->snapshot);
                for (const auto& child : pending.item->children)
                    state->nodes.append({child.get(), childParentId});
                continue;
            }
            if (state->actionIndex < state->actions.size()) {
                ProjectManager::syncActionNameForPreview(
                    state->actions.at(state->actionIndex++));
                continue;
            }
            const auto allocation = coordinator.markCurrentWorldMutation();
            if (allocation.state
                == RuntimeSyncCoordinator::AllocationState::IdentityExhausted) {
                qCritical()
                    << "[CurrentWorldSync] authoring revision identity exhausted";
            }
            manager.m_pendingRuntimePreparation =
                PendingRuntimePreparation{state->projectSessionId,
                                          state->runtimeEpoch,
                                          allocation.revision,
                                          state->context,
                                          std::move(state->finished)};
            if (state->submissionBatchHeld) {
                coordinator.endSubmissionBatch();
                state->submissionBatchHeld = false;
            }
            QObject::disconnect(state->contextDestroyedConnection);
            state->step.reset();
            manager.finishRuntimePreparationIfReady();
            return;
        }
        QTimer::singleShot(1, state->context.data(), *state->step);
    };
    state->contextDestroyedConnection = QObject::connect(
        context, &QObject::destroyed, &manager, [state]() {
        if (state->submissionBatchHeld) {
            AudioEngine::instance().runtimeSyncCoordinator()
                .endSubmissionBatch();
            state->submissionBatchHeld = false;
        }
        state->step.reset();
    });
    QTimer::singleShot(0, context, *state->step);
}


void ProjectManager::syncProjectToRuntime(ProjectData* project)
{
    if (!project) return;
    if (!busPublicationSupported(*project)) return;
    if (!canonicalRuntimeIdentityIndex(*project)) {
        emit ProjectManager::instance().errorOccurred(QStringLiteral(
            "Project Runtime identities are invalid; Runtime sync was not started"));
        return;
    }
    const auto snapshot = buildFieldGroupEffectiveSnapshot(
        *project, toFieldGroupScope(currentWorldPlatformScope(PlatformContextService::instance().context())));
    if (!snapshot.ok()) {
        emit ProjectManager::instance().errorOccurred(QStringLiteral(
            "Project field groups are invalid; Runtime sync was not started"));
        return;
    }
    startupTrace(QStringLiteral("RuntimeSync: syncProjectToRuntime begin"));

    auto& ae = AudioEngine::instance();
    ae.runtimeSyncCoordinator().beginSubmissionBatch();
    supersedePendingCurrentWorld(ae.runtimeSyncCoordinator());
    startupTrace(QStringLiteral("RuntimeSync: before clearRuntimeObjects"));
    ae.clearRuntimeObjects();
    startupTrace(QStringLiteral("RuntimeSync: after clearRuntimeObjects"));
    ae.clearRuntimeMedia();
    startupTrace(QStringLiteral("RuntimeSync: after clearRuntimeMedia"));

    startupTrace(QStringLiteral("RuntimeSync: before sync attenuations"));
    for (const auto* def : AttenuationStore::all(*project))
        AttenuationStore::syncToRuntime(*project, *def);
    startupTrace(QStringLiteral("RuntimeSync: after sync attenuations"));

    startupTrace(QStringLiteral("RuntimeSync: before sync item tree"));
    for (const auto* binding : project->allFileBindings())
    {
        if (binding && binding->rootItem)
        {
            syncItemToRuntime(binding->rootItem.get(), {}, snapshot);
        }
    }
    startupTrace(QStringLiteral("RuntimeSync: after sync item tree"));

    startupTrace(QStringLiteral("RuntimeSync: before sync action names"));
    std::function<void(const ProjectItem*)> syncActionNames = [&](const ProjectItem* item) {
        if (!item) return;
        if (item->type == ProjectItem::Type::Action && item->objectID != 0)
        {
            ProjectManager::syncActionNameForPreview(const_cast<ProjectItem*>(item));
        }
        for (const auto& child : item->children)
            syncActionNames(child.get());
    };
    for (const auto* binding : project->allFileBindings())
    {
        if (binding && binding->rootItem)
            syncActionNames(binding->rootItem.get());
    }
    startupTrace(QStringLiteral("RuntimeSync: after sync action names"));

    qDebug() << "[Bootstrap] Synced" << ae.getRuntimeObjectCount()
             << "objects to Runtime ObjectRegistry,"
             << ae.getRuntimeMediaCount()
             << "media entries to MediaRegistry";

    ProjectManager::instance().m_runtimePrepared = false;

    startupTrace(QStringLiteral("RuntimeSync: before sync current world"));
    const auto allocation = ae.runtimeSyncCoordinator().markCurrentWorldMutation();
    if (allocation.state == RuntimeSyncCoordinator::AllocationState::IdentityExhausted)
        qCritical() << "[CurrentWorldSync] authoring revision identity exhausted";
    else {
        auto& manager = ProjectManager::instance();
        const auto sessionId = ae.runtimeSyncCoordinator()
            .barrier(allocation.revision).projectSessionId;
        manager.m_pendingRuntimePreparation = PendingRuntimePreparation{
            sessionId,
            ae.runtimeSyncCoordinator().barrier(allocation.revision).runtimeEpoch,
            allocation.revision,
            &manager,
            {}};
        ae.runtimeSyncCoordinator().endSubmissionBatch();
        ae.runtimeSyncCoordinator().flushPendingMutationNow();
        manager.finishRuntimePreparationIfReady();
    }
    if (allocation.state == RuntimeSyncCoordinator::AllocationState::IdentityExhausted)
        ae.runtimeSyncCoordinator().endSubmissionBatch();
    startupTrace(QStringLiteral("RuntimeSync: after sync current world"));

    startupTrace(QStringLiteral("RuntimeSync: syncProjectToRuntime end"));
}


void ProjectManager::requestResync(ProjectItem* item)
{
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    if (m_closing || !m_currentProject) {
        m_pendingResyncs.clear();
        m_resyncTimerScheduled = false;
        return;
    }
    if (!item || item->objectID == 0) return;

    m_pendingResyncs.insert(item);

    if (!m_resyncTimerScheduled) {
        m_resyncTimerScheduled = true;
        QTimer::singleShot(0, this, &ProjectManager::flushPendingResyncs);
    }
}

void ProjectManager::flushPendingResyncs()
{
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
    m_resyncTimerScheduled = false;

    if (m_closing || !m_currentProject) {
        m_pendingResyncs.clear();
        return;
    }

    if (m_pendingResyncs.isEmpty()) return;

    QList<ProjectItem*> roots;
    for (ProjectItem* item : m_pendingResyncs) {
        bool coveredByAnother = false;
        for (ProjectItem* ancestor = item->parent; ancestor; ancestor = ancestor->parent) {
            if (m_pendingResyncs.contains(ancestor)) {
                coveredByAnother = true;
                break;
            }
        }
        if (!coveredByAnother)
            roots.append(item);
    }

    m_pendingResyncs.clear();

    for (ProjectItem* item : roots) {
        resyncItemToRuntime(item);
    }
}

}
