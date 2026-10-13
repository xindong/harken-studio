// ProjectSyncBridge implementation -- private side of the project-sync cluster.
//
// S2 routes authored objects to harken_core through the in-process C API. The
// editor owns stable IDs; names are labels layered on those IDs for audition.

#include "ProjectSyncBridge.h"
#include "NativeValues.h"
#include "RuntimeLink.h"

#include <harken/Harken.h>
#include <harken_studio_build/MediaContentRevision.h>

#include <QDebug>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace HarkenEditor {
namespace Bridge {

namespace {

struct LayerState {
    std::vector<harkenstudio::ObjectId> children;
};

std::unordered_map<harkenstudio::ObjectId, SoundDesc> g_sounds;
std::unordered_map<harkenstudio::ObjectId, LayerState> g_layers;

} // namespace

bool syncSound(const SoundDesc& desc)
{
    if (harkenstudio::IsNull(desc.objectId) || harkenstudio::IsNull(desc.soundId) || desc.sourceId == 0)
        return false;

    g_sounds[desc.objectId] = desc;
    Detail::runtimeRegisterSoundName(desc.name, desc.objectId, desc.soundId,
                                     desc.sourceId, desc.loopCount,
                                     desc.spatialized3D, desc.attenuationId);
    if (!harkenstudio::IsNull(desc.parentObjectId))
        addLayerChild(desc.parentObjectId, desc.soundId);
    return true;
}

uint64_t soundPcmContentRevision(const std::vector<float>& samples,
                                 uint32_t channels, uint32_t sampleRate,
                                 uint64_t loopStartFrame, uint64_t loopEndFrame)
{
    if (channels == 0 || channels > 8 || sampleRate == 0
        || samples.empty() || samples.size() % channels != 0)
        return 0;
    const float* planes[8]{};
    for (uint32_t channel = 0; channel < channels; ++channel)
        planes[channel] = samples.data() + channel;
    uint64_t revision = 0;
    if (!harkenstudiobuild::ComputeMediaPcmContentRevisionV1(
            sampleRate, channels, samples.size() / channels, planes, channels,
            &revision, loopStartFrame, loopEndFrame))
        return 0;
    return revision;
}

bool syncLayer(const LayerDesc& desc)
{
    if (harkenstudio::IsNull(desc.objectId))
        return false;

    LayerState& state = g_layers[desc.objectId];
    if (!harkenstudio::IsNull(desc.parentObjectId))
        addLayerChild(desc.parentObjectId, desc.objectId);
    return true;
}

bool syncAttenuation(const PackAttenuation& attenuation)
{
    if (harkenstudio::IsNull(attenuation.id))
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

void addLayerChild(harkenstudio::ObjectId parentLayerId, harkenstudio::ObjectId childObjectId)
{
    if (harkenstudio::IsNull(parentLayerId) || harkenstudio::IsNull(childObjectId))
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

void setObjectOutputBus(harkenstudio::ObjectId objectId, harkenstudio::ObjectId busRuntimeId)
{
    Detail::runtimeSetObjectOutputBus(objectId, busRuntimeId);
}

bool runtimeObjectExists(harkenstudio::ObjectId objectId)
{
    return g_sounds.find(objectId) != g_sounds.end() ||
           g_layers.find(objectId) != g_layers.end();
}

void removeRuntimeObject(harkenstudio::ObjectId objectId)
{
    if (harkenstudio::IsNull(objectId))
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
    if (harkenstudio::IsNull(mapping.id))
        return false;

    Detail::runtimeRegisterActionName(mapping.name, mapping.id);
    return true;
}

void unregisterActionNameForPreview(harkenstudio::ObjectId actionId)
{
    if (harkenstudio::IsNull(actionId))
        return;
    Detail::runtimeUnregisterActionName(actionId);
}

} // namespace Bridge
} // namespace HarkenEditor
