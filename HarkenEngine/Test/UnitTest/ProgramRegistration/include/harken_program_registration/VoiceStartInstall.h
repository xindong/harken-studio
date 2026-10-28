// The install description one occurrence contributes when its voice starts, and
// the thin primitive that applies it to the engine.
//
// A compiled occurrence carries folded gain and pitch, a loop flag, a resolved
// output bus and a merged send list. None of that is installed until a voice for
// the occurrence actually starts. VoiceStartInstall is the description a later
// executor hands the engine at that moment: it is filled at registration (where
// the schema bus ids are mapped to engine bus ids, the subject is resolved to an
// engine sound id, and the session's caller default is applied), and consumed by
// PlayVoiceStartInstall.
//
// Everything a start needs is in the one struct, including which sound plays and
// whether a voice starts at all. A caller therefore cannot pair an install with
// a sound from somewhere else, and cannot start a placeholder occurrence by
// supplying a sound the program never named.
//
// PlayVoiceStartInstall is a pure translation to the existing engine seams — it
// builds a VnPlayDesc, stages the voice, installs each send, and commits — with
// no scheduling or timeline logic of its own; that "when to start" decision
// belongs to the executor, which reuses this primitive rather than re-deriving
// "how to start". It runs on the logic thread (every seam it calls is
// logic-thread only) and adds nothing to the audio thread.
//
// The play opts into the engine's staged install, every seam call below it stores
// onto a voice that has not started, and one commit publishes the play and staged
// values as an indivisible command batch. The audio drain waits until one quantum
// can apply the complete batch, so the first audible block carries the authored
// gain, pan and chain.
//
// What the staging does settle is the rollback. A failure anywhere below the play
// cancels a voice that published nothing, so the cancel takes no queue capacity,
// cannot be refused, and needs no fade: there is no rendered level for a stop to
// step down from. That is why this primitive's rollback passes a zero-length stop
// where a stop of a committed voice takes the timeline declick — the two are
// different domains, not an inconsistency, and harken/abi/TimelineStopContract.h
// draws that line from the other side.
#ifndef HARKEN_PROGRAM_REGISTRATION_VOICE_START_INSTALL_H
#define HARKEN_PROGRAM_REGISTRATION_VOICE_START_INSTALL_H

#include <harken/ObjectId.h>
#include "harken_program_registration/EffectDescriptorWriter.h"

#include <harken/Harken.h>

#include <cstdint>
#include <vector>

namespace harkenprogramreg {

// One merged send resolved to engine terms: the target bus as a VnBusId and the
// send level the schema authored. The schema carries that level verbatim and
// interprets nothing, so the unit is fixed here, at the only place it is
// consumed: it is installed as the levelDb of Vn_SetVoiceSend, so it is
// decibels and an author who wrote a linear ratio has written a different
// level.
struct VoiceSend {
    VnBusId targetBus{};
    float level = 0.0f;
};

// One lowered chain step: the voice slot to fill and the descriptor to fill it
// with. slot comes from the lowering stage — steps run in ascending slot order,
// which is the chain's execution order — and is checked against the session's
// resolved slot count at registration, so an install never carries a slot the
// engine would refuse.
//
// The descriptor is built once at registration, where the type's captured layout
// facts and its parameters' accepted ranges are available to check first. The
// install therefore performs no conversion and no validation of its own: it hands
// the bytes to the seam. Keeping only the descriptor, rather than the descriptor
// and a parallel parameter list, means there is no second copy of the values to
// disagree with it.
struct VoiceEffectSlot {
    std::uint32_t slot = 0;
    EffectDescriptorBytes descriptor;
};

// The installable description of one occurrence's voice start. baseVolumeDb and
// basePitchRatio are the folded audible bases; loopCount is the occurrence's own value.
// outputBus is the voice's dry-path bus once resolved: outputBusResolved is true
// when a bus is known (either the reference-resolved bus or a caller default),
// and outputBusDefaulted records that the value came from the caller default
// rather than the program, so applying a default is never silent. When
// outputBusResolved is false the bus is genuinely unresolved and no default was
// supplied; the install primitive refuses rather than guessing a bus.
struct VoiceStartInstall {
    // What the voice plays, and whether there is a voice at all.
    //
    // startsAudibly is false for an occurrence that starts no voice: a clip
    // targeting a Container or a Segment is a start-only placeholder whose content
    // is expanded when that content is itself triggered, not here. For such an
    // occurrence soundId is zero and means nothing -- the absence is carried by
    // the flag, never by a reserved id value, so no consumer has to read zero as
    // a sentinel and no zero can be mistaken for a sound the engine would refuse.
    //
    // The default is the refusing one. An install has to say it starts a voice
    // before the primitive will start one, so a hand-built or partly-filled
    // install cannot reach the engine as if it named a sound.
    bool startsAudibly = false;
    VnSoundId soundId{};

    float baseVolumeDb = 0.0f;
    float outputBusVolumeDb = 0.0f;
    float basePitchRatio = 1.0f;
    std::uint32_t loopCount = 1;
    VnBusId outputBus{};
    bool outputBusResolved = false;
    bool outputBusDefaulted = false;
    std::vector<VoiceSend> sends;

    // The subject's own spatial placement. panning supplies the 2D balance;
    // spread is authoring percent in [0, 100] and is consumed only for a
    // spatialized mono source rendered to a stereo layout. The start seam maps
    // it once to Runtime Width [0, 1]. is3DEnabled selects the point-source image.
    // attenuationDistanceScaling scales distance. attenuationResolved and
    // resolvedAttenuation name the engine Attenuation profile to bind — already
    // mapped from the schema id and, for the occurrence's rolloff, to the derived
    // variant; when unresolved no profile binds and the voice gets geometric pan
    // only.
    float panning = 0.0f;
    float spread = 0.0f;
    bool is3DEnabled = false;
    float attenuationDistanceScaling = 1.0f;
    bool attenuationResolved = false;
    VnAttenuationId resolvedAttenuation{};

    // The occurrence's effect chain, already lowered onto voice slots and already
    // gated against the session's resolved slot count, registered effect types and
    // per-type pools. Empty means there is nothing to install — either the
    // occurrence selected no chain, or it selected a declared-empty one, which are
    // different authoring facts but the same install: no effect is placed. The
    // steps are in ascending slot order, which is execution order.
    std::vector<VoiceEffectSlot> effectSlots;
};

// Starts a voice for one install against the live engine and installs its sends
// and spatial placement. A pure translation to the engine seams, no scheduling:
// it maps the install to a VnPlayDesc (loopCount is passed through directly;
// is3DEnabled -> spatialized),
// stages it through the internal raw voice-start seam, installs each send
// with Vn_SetVoiceSend into ascending slots, installs the spatial placement —
// pan, width, distance scaling, and the Attenuation binding when one resolved —
// installs each effect-chain step into its slot with Vn_SetVoiceEffect, and
// publishes the lot with Vn_CommitVoiceStart. On success outHandle holds the
// voice and outStopDeferred is false.
//
// The install carries its own sound, so there is no second argument a caller
// could pair with the wrong install. Refuses with VN_INVALID_ARG, before any
// engine call, when the install says it starts no voice: a placeholder's zero
// soundId is not an id, and stopping here is what keeps it from reaching the
// engine as one. Refuses the same way when the install's output bus is
// unresolved: this primitive never guesses a bus, so an unresolved occurrence
// with no caller default must be routed by the caller before it reaches here.
// Otherwise the return is the first failing seam's result, or the commit's —
// including VN_NO_VOICE when the engine cannot admit the start's Render slot or
// its whole command batch at once.
//
// Cleanup is always the primitive's own. Both outputs are cleared before any
// work and stay cleared on every failure: nothing was published for a staged
// voice, so cancelling it cannot be refused and leaves no audible voice and no
// state to reclaim. outStopDeferred therefore never becomes true — it remains
// as the caller-visible statement that no ownership transferred.
VnResult PlayVoiceStartInstall(const VoiceStartInstall& install,
                               VnVoiceHandle* outHandle,
                               bool* outStopDeferred);

}  // namespace harkenprogramreg

#endif  // HARKEN_PROGRAM_REGISTRATION_VOICE_START_INSTALL_H
