#pragma once

#include <harken/Harken.h>
#include "harken/core/CurrentWorld.h"
#include "harken/param/VariableService.h"
#include <initializer_list>

namespace harkentest {

struct DirectSoundActionWorld {
    harken::VariableDefinitionV1 variable{};
    harken::PlayablePropertyProgramViewV1 property{};
    harken::ActionStepProgramViewV1 step{};
    harken::ActionProgramViewV1 action{};
    harken::ContainerWorldInstallViewV1 world{};

    DirectSoundActionWorld(std::uint64_t contentRevision, VnObjectId soundId,
        VnObjectId actionId, const harken::VariableDefinitionV1& definition,
        const harken::PlayablePropertyPayloadViewV1& payload) {
        variable = definition;
        property.contentPropertyKind =
            harken::playableproperties::ContentPropertyKindV1::Sound;
        property.stableId = soundId;
        property.contentRevision = contentRevision;
        property.routeStableId = VN_BUS_MASTER;
        property.compatibilityFingerprint = 1;
        property.payload = payload;
        property.programSemanticFingerprint =
            harken::ComputePlayablePropertyFingerprintV1(property);

        step.opcode = harken::ActionOpcodeV1::Play;
        step.target.kind = harken::ActionTargetKindV1::Sound;
        step.target.stableId = soundId;
        step.target.expectedProgramSemanticFingerprint =
            property.programSemanticFingerprint;
        for (auto* time : {&step.delay, &step.fade, &step.cooldown}) {
            time->domain = harken::AuthoredTimeDomainV1::AbsoluteNanoseconds;
            time->denominator = 1;
        }

        action.actionStableId = actionId;
        action.steps = &step;
        action.stepCount = 1;
        action.expandedStepOccurrenceCount = 1;
        action.expandedFiringCount = 1;
        action.programSemanticFingerprint =
            harken::ComputeActionProgramFingerprintV1(action);

        world.variables = &variable;
        world.variableCount = 1;
        world.playableProperties = &property;
        world.playablePropertyCount = 1;
        world.actions = &action;
        world.actionCount = 1;
        world.identity.capabilityMask =
            harken::kWorldCapabilityVariableDefinitions |
            harken::kWorldCapabilityPlayableProperties |
            harken::kWorldCapabilityActionDefinitions;
        world.identity.runtimeSemanticFingerprint =
            harken::ComputeCurrentWorldFingerprintV4(world);
    }
};

} // namespace harkentest
