#include "harken_program_registration/VoiceStartInstall.h"

#include "HarkenApiInternal.h"
#include "harken/core/PlayablePropertyCut.h"

#include <cstddef>

namespace harkenprogramreg {

VnResult PlayVoiceStartInstall(const VoiceStartInstall& install,
                               VnVoiceHandle* outHandle,
                               bool* outStopDeferred) {
    if (outHandle == nullptr || outStopDeferred == nullptr) {
        return VN_INVALID_ARG;
    }
    *outHandle = 0;
    *outStopDeferred = false;
    if (!install.startsAudibly) {
        // A placeholder occurrence has no voice to start and no sound to name.
        // Its soundId is zero because there is no sound, not because zero is an
        // invalid id: the engine can register a real sound as id zero. The
        // explicit flag prevents the placeholder from being mistaken for it.
        return VN_INVALID_ARG;
    }
    if (!install.outputBusResolved) {
        // No bus to route the dry path to and no caller default was applied at
        // registration. This primitive does not invent a bus.
        return VN_INVALID_ARG;
    }

    VnPlayDesc desc{};
    desc.structSize = sizeof(VnPlayDesc);
    // The staged install: the play resolves and reserves but publishes nothing,
    // and the commit publishes one indivisible command batch that the audio drain
    // applies within one quantum. Installing onto an already-started voice can
    // leave earlier blocks audible at engine defaults.
    desc.reserved0 = VN_PLAY_FLAG_STAGED_INSTALL;
    desc.soundId = install.soundId;
    desc.busId = install.outputBus;
    desc.volumeDb = install.baseVolumeDb;
    desc.pitchRatio = install.basePitchRatio;
    desc.loopCount = install.loopCount;
    // is3DEnabled fixes the spatialized flag at play; a 3D voice follows a point
    // source, a 2D voice keeps the pan image.
    desc.spatialized = install.is3DEnabled ? 1u : 0u;

    VnVoiceHandle staged = 0;
    const VnResult playResult = harken::detail::PlayVoiceRaw(&desc, &staged);
    if (playResult != VN_OK) {
        return playResult;
    }
    // Nothing has been published for a staged voice, so cancelling it is a
    // logic-side bookkeeping step that cannot be refused and leaves nothing
    // audible behind — the primitive always owns its own cleanup.
    const auto rollback = [staged](VnResult failure) {
        (void)Vn_Stop(staged, 0);
        return failure;
    };

    const VnResult outputVolumeResult = Vn_SetVoiceOutputVolume(
        staged, install.outputBusVolumeDb, 0);
    if (outputVolumeResult != VN_OK) return rollback(outputVolumeResult);

    for (std::size_t slot = 0; slot < install.sends.size(); ++slot) {
        const VoiceSend& send = install.sends[slot];
        const VnResult sendResult = Vn_SetVoiceSend(
            staged, static_cast<std::uint32_t>(slot), send.targetBus,
            send.level);
        if (sendResult != VN_OK) {
            return rollback(sendResult);
        }
    }

    // Spatial placement, installed through the same public seams the executor
    // would use. pan and width ride the API-direct slots; the distance scale
    // and, when one resolved, the Attenuation binding apply the 3D response. A
    // 2D voice stores the 3D-only values with no render effect.
    const VnResult panResult = Vn_SetVoicePan(staged, install.panning);
    if (panResult != VN_OK) {
        return rollback(panResult);
    }
    if (!harken::IsValidAuthoredSpreadPercent(install.spread)) {
        return rollback(VN_INVALID_ARG);
    }
    const VnResult widthResult = Vn_SetVoiceWidth(
        staged, harken::AuthoredSpreadPercentToRuntimeWidth(install.spread));
    if (widthResult != VN_OK) {
        return rollback(widthResult);
    }
    const VnResult scaleResult = Vn_SetVoiceDistanceScale(
        staged, install.attenuationDistanceScaling);
    if (scaleResult != VN_OK) {
        return rollback(scaleResult);
    }
    if (install.attenuationResolved) {
        const VnResult bindResult =
            Vn_BindVoiceAttenuation(staged, install.resolvedAttenuation);
        if (bindResult != VN_OK) {
            return rollback(bindResult);
        }
    }

    // The effect chain, one seam call per step in ascending slot order. Each
    // descriptor was built and range-checked at registration, so nothing is
    // converted or validated here; the seam still validates what it is handed,
    // and a refusal abandons the install rather than committing a partial chain.
    // Parameters travel inside the descriptor, so no separate parameter call
    // follows.
    for (const VoiceEffectSlot& step : install.effectSlots) {
        const VnResult effectResult = Vn_SetVoiceEffect(
            staged, step.slot, step.descriptor.type,
            step.descriptor.AsDesc());
        if (effectResult != VN_OK) {
            return rollback(effectResult);
        }
    }
    const VnResult commitResult = Vn_CommitVoiceStart(staged);
    if (commitResult != VN_OK) {
        return rollback(commitResult);
    }
    *outHandle = staged;
    return VN_OK;
}

}  // namespace harkenprogramreg
