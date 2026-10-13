#pragma once

#include "Utils/ObjectId.h"

// ProjectSyncBridge -- project-sync cluster of the runtime bridge.
//
// Scope: mirroring the authored project into the live engine -- registering
// and updating runtime objects (Actions, containers, buses, attenuation,
// Variable bindings) as the user edits, and tearing them down on
// project close (ProjectManager_RuntimeSync, the
// Core/Actions authoring handlers).
//
// Interface intent (M4 alignment): sync is keyed by stable u64 asset IDs
// (spec §2.3) -- renames re-label, they never re-identify. The push model
// anticipates the M4 hot-reload verbs (spec §5 replace semantics:
// retire-old / new-plays-new), so a desc update through this interface maps
// onto Vn_Replace* after the rewire without reshaping callers. The uint32_t
// ids below are the legacy FNV-1a-32 name-hash currency, opaque to callers.
//
// All functions operate on the bridge-wide ACTIVE engine session (registered
// by PlaybackBridge's EngineSession) plus the process-global object registry;
// they are null-safe no-ops / false when no target exists.
//
// Public bridge headers expose editor-owned types only (Qt/std/editor
// structs); the legacy engine surface never appears here.

#include "RuntimeBridge/PackagingBridge.h"

#include <QString>
#include <cstdint>
#include <vector>

namespace HarkenEditor {
namespace Bridge {

//==============================================================================
// Playable-object sync.
//==============================================================================

struct SoundDesc {
    harkenstudio::ObjectId objectId = {};        // stable editor-minted object id
    harkenstudio::ObjectId parentObjectId = {};  // 0 = none; bridge resolves and reparents
    QString name;
    harkenstudio::ObjectId soundId = {};         // stable playable id registered with harken_core
    uint32_t sourceId = 0;
    uint32_t loopCount = 1;
    bool spatialized3D = false;
    uint8_t streamingMode = 0;
    harkenstudio::ObjectId attenuationId = {};
};

struct LayerDesc {
    harkenstudio::ObjectId objectId = {};        // stable editor-minted object id
    harkenstudio::ObjectId parentObjectId = {};  // 0 = none; bridge resolves and reparents
    QString name;
    bool spatialized3D = false;
    harkenstudio::ObjectId attenuationId = {};
};

bool syncSound(const SoundDesc& desc);
uint64_t soundPcmContentRevision(const std::vector<float>& samples,
                                 uint32_t channels, uint32_t sampleRate,
                                 uint64_t loopStartFrame = 0,
                                 uint64_t loopEndFrame = 0);
bool syncLayer(const LayerDesc& desc);
// Only acts when the parent resolves to a layer container.
void addLayerChild(harkenstudio::ObjectId parentLayerId, harkenstudio::ObjectId childObjectId);
bool syncAttenuation(const PackAttenuation& attenuation);
void setObjectOutputBus(harkenstudio::ObjectId objectId, harkenstudio::ObjectId busRuntimeId);
bool runtimeObjectExists(harkenstudio::ObjectId objectId);
void removeRuntimeObject(harkenstudio::ObjectId objectId);
void clearRuntimeObjects();
uint32_t runtimeObjectCount();

//==============================================================================
// Action registration.
//==============================================================================

struct ActionNameMapping {
    harkenstudio::ObjectId id{};              // stable editor-minted Action id
    QString name;
};

bool registerActionNameForPreview(const ActionNameMapping& mapping);
void unregisterActionNameForPreview(harkenstudio::ObjectId actionId);

} // namespace Bridge
} // namespace HarkenEditor
