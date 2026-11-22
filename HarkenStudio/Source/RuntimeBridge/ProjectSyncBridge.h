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


#include <QString>
#include <cstdint>
#include <vector>

namespace harken::studio {
namespace bridge {

//==============================================================================
// Playable-object sync.
//==============================================================================

struct PackCurveHandle
{
    float h1x = 0.0f;
    float h1y = 0.0f;
    float h2x = 0.0f;
    float h2y = 0.0f;
};

struct PackAttenuation
{
    harken::studio::types::ObjectId id{};
    float maxDistance = 100.0f;

    struct CurvePoint
    {
        float distance = 0.0f;
        float value = 0.0f;
    };
    struct Slot
    {
        uint8_t slot = 0;    // runtime curve slot (0 volume)
        std::vector<CurvePoint> points;
        std::vector<PackCurveHandle> handles;
    };
    // Named curveSlots (not "slots") because Qt's keyword macros are in
    // effect wherever QObject-based editor code includes this header.
    std::vector<Slot> curveSlots;
};

struct SoundDesc {
    harken::studio::types::ObjectId objectId = {};        // stable editor-minted object id
    harken::studio::types::ObjectId parentObjectId = {};  // 0 = none; bridge resolves and reparents
    QString name;
    harken::studio::types::ObjectId soundId = {};         // stable playable id registered with harken_core
    uint32_t sourceId = 0;
    uint32_t loopCount = 1;
    bool spatialized3D = false;
    uint8_t streamingMode = 0;
    harken::studio::types::ObjectId attenuationId = {};
};

struct LayerDesc {
    harken::studio::types::ObjectId objectId = {};        // stable editor-minted object id
    harken::studio::types::ObjectId parentObjectId = {};  // 0 = none; bridge resolves and reparents
    QString name;
    bool spatialized3D = false;
    harken::studio::types::ObjectId attenuationId = {};
};

bool syncSound(const SoundDesc& desc);
uint64_t soundPcmContentRevision(const std::vector<float>& samples,
                                 uint32_t channels, uint32_t sampleRate,
                                 uint64_t loopStartFrame = 0,
                                 uint64_t loopEndFrame = 0);
bool syncLayer(const LayerDesc& desc);
// Only acts when the parent resolves to a layer container.
void addLayerChild(harken::studio::types::ObjectId parentLayerId, harken::studio::types::ObjectId childObjectId);
bool syncAttenuation(const PackAttenuation& attenuation);
void setObjectOutputBus(harken::studio::types::ObjectId objectId, harken::studio::types::ObjectId busRuntimeId);
bool runtimeObjectExists(harken::studio::types::ObjectId objectId);
void removeRuntimeObject(harken::studio::types::ObjectId objectId);
void clearRuntimeObjects();
uint32_t runtimeObjectCount();

//==============================================================================
// Action registration.
//==============================================================================

struct ActionNameMapping {
    harken::studio::types::ObjectId id{};              // stable editor-minted Action id
    QString name;
};

bool registerActionNameForPreview(const ActionNameMapping& mapping);
void unregisterActionNameForPreview(harken::studio::types::ObjectId actionId);

} // namespace bridge
} // namespace harken::studio
