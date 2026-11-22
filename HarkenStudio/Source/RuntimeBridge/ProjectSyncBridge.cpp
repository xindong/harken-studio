// ProjectSyncBridge implementation -- private side of the project-sync cluster.
//
// S2 routes authored objects to harken_core through the in-process C API. The
// editor owns stable IDs; names are labels layered on those IDs for audition.

#include "ProjectSyncBridge.h"
#include "NativeValues.h"
#include "RuntimeLink.h"
#include "MediaServices.h"

#include <harken/Harken.h>

#include <QDebug>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace harken::studio {
namespace bridge {

namespace {

struct LayerState {
    std::vector<harken::studio::types::ObjectId> children;
};

std::unordered_map<harken::studio::types::ObjectId, SoundDesc> g_sounds;
std::unordered_map<harken::studio::types::ObjectId, LayerState> g_layers;

} // namespace

bool syncSound(const SoundDesc& desc)
{
    if (harken::studio::types::IsNull(desc.objectId) || harken::studio::types::IsNull(desc.soundId) || desc.sourceId == 0)
        return false;

    g_sounds[desc.objectId] = desc;
    Detail::runtimeRegisterSoundName(desc.name, desc.objectId, desc.soundId,
                                     desc.sourceId, desc.loopCount,
                                     desc.spatialized3D, desc.attenuationId);
    if (!harken::studio::types::IsNull(desc.parentObjectId))
        addLayerChild(desc.parentObjectId, desc.soundId);
    return true;
}

uint64_t soundPcmContentRevision(const std::vector<float>& samples,
                                 uint32_t channels, uint32_t sampleRate,
                                 uint64_t loopStartFrame, uint64_t loopEndFrame)
{
    const auto services = mediaServices();
    return services ? services->pcmContentRevision(samples, channels, sampleRate,
                                                   loopStartFrame, loopEndFrame)
                    : 0;
}

bool syncLayer(const LayerDesc& desc)
{
    if (harken::studio::types::IsNull(desc.objectId))
        return false;

    LayerState& state = g_layers[desc.objectId];
    if (!harken::studio::types::IsNull(desc.parentObjectId))
        addLayerChild(desc.parentObjectId, desc.objectId);
    return true;
}

bool syncAttenuation(const PackAttenuation& attenuation)
{
    if (harken::studio::types::IsNull(attenuation.id))
        return false;
    if (!std::isfinite(attenuation.maxDistance) || !(attenuation.maxDistance > 0.0f))
    {
        qWarning() << "[ProjectSyncBridge] attenuation register skipped:"
                   << "id=" << attenuation.id
                   << "maxDistance=" << attenuation.maxDistance;
        return false;
    }

    VnAttenuationDesc desc{};
    desc.structSize = sizeof(VnAttenuationDesc);
    desc.rolloff = VN_ROLLOFF_LINEAR;
    desc.minDistance = std::min(1.0f, attenuation.maxDistance * 0.5f);
    desc.maxDistance = attenuation.maxDistance;
    desc.airAbsorptionNearHz = 20000.0f;
    desc.airAbsorptionFarHz = 20000.0f;

    for (const PackAttenuation::Slot& slot : attenuation.curveSlots)
    {
        if (slot.slot != 0 || slot.points.empty())
            continue;
        if (slot.points.size() > VN_ATTENUATION_MAX_CURVE_POINTS)
        {
            qWarning() << "[ProjectSyncBridge] attenuation register skipped:"
                       << "id=" << attenuation.id
                       << "pointCount=" << static_cast<qulonglong>(slot.points.size());
            return false;
        }
        if (slot.points.size() > 1 && slot.handles.size() != slot.points.size() - 1)
        {
            qWarning() << "[ProjectSyncBridge] attenuation curve disabled:"
                       << "id=" << attenuation.id
                       << "points=" << static_cast<qulonglong>(slot.points.size())
                       << "handles=" << static_cast<qulonglong>(slot.handles.size());
            break;
        }
        desc.volumeCurve.enabled = 1;
        desc.volumeCurve.pointCount = static_cast<uint32_t>(slot.points.size());
        for (size_t i = 0; i < slot.points.size(); ++i)
        {
            desc.volumeCurve.points[i].x = slot.points[i].distance;
            desc.volumeCurve.points[i].y = slot.points[i].value;
        }
        for (size_t i = 0; i + 1 < slot.points.size(); ++i)
        {
            desc.volumeCurve.handles[i].h1x = slot.handles[i].h1x;
            desc.volumeCurve.handles[i].h1y = slot.handles[i].h1y;
            desc.volumeCurve.handles[i].h2x = slot.handles[i].h2x;
            desc.volumeCurve.handles[i].h2y = slot.handles[i].h2y;
        }
        break;
    }

    VnResult result = Vn_RegisterAttenuation(toNative(attenuation.id), &desc);
    if (result == VN_IN_USE)
        result = Vn_ReplaceAttenuation(toNative(attenuation.id), &desc);
    if (result != VN_OK)
    {
        qWarning() << "[ProjectSyncBridge] attenuation register failed:"
                   << "id=" << attenuation.id
                   << "result=" << static_cast<int>(result);
        return false;
    }
    return true;
}

void addLayerChild(harken::studio::types::ObjectId parentLayerId, harken::studio::types::ObjectId childObjectId)
{
    if (harken::studio::types::IsNull(parentLayerId) || harken::studio::types::IsNull(childObjectId))
        return;

    const auto parentIt = g_layers.find(parentLayerId);
    if (parentIt == g_layers.end())
        return;

    LayerState& state = parentIt->second;
    if (std::find(state.children.begin(), state.children.end(), childObjectId) ==
        state.children.end())
    {
        state.children.push_back(childObjectId);
    }

}

void setObjectOutputBus(harken::studio::types::ObjectId objectId, harken::studio::types::ObjectId busRuntimeId)
{
    Detail::runtimeSetObjectOutputBus(objectId, busRuntimeId);
}

bool runtimeObjectExists(harken::studio::types::ObjectId objectId)
{
    return g_sounds.find(objectId) != g_sounds.end() ||
           g_layers.find(objectId) != g_layers.end();
}

void removeRuntimeObject(harken::studio::types::ObjectId objectId)
{
    if (harken::studio::types::IsNull(objectId))
        return;
    Detail::runtimeUnregisterSoundObject(objectId);
    g_sounds.erase(objectId);
    Detail::runtimeSetObjectOutputBus(objectId, {});
    g_layers.erase(objectId);
}

void clearRuntimeObjects()
{
    g_layers.clear();
    g_sounds.clear();
    Detail::runtimeClearSoundNames();
}

uint32_t runtimeObjectCount()
{
    return static_cast<uint32_t>(g_sounds.size() + g_layers.size());
}

bool registerActionNameForPreview(const ActionNameMapping& mapping)
{
    if (harken::studio::types::IsNull(mapping.id))
        return false;

    Detail::runtimeRegisterActionName(mapping.name, mapping.id);
    return true;
}

void unregisterActionNameForPreview(harken::studio::types::ObjectId actionId)
{
    if (harken::studio::types::IsNull(actionId))
        return;
    Detail::runtimeUnregisterActionName(actionId);
}

} // namespace bridge
} // namespace harken::studio
