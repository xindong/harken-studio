#pragma once

#include <harken_studio/AudioValues.h>

namespace harkenstudiobuild {

struct ParameterCurvePoint { float x = 0; float y = 0; };
struct ParameterCurveHandles { float h1x = 0; float h1y = 0; float h2x = 0; float h2y = 0; };

struct BindingCurve {
    std::uint32_t pointCount = 0;
    ParameterCurvePoint points[harkenstudio::kCurveMaxPoints]{};
    ParameterCurveHandles handles[harkenstudio::kCurveMaxPoints - 1]{};
};

struct AttenuationCurve {
    std::uint32_t enabled = 0;
    std::uint32_t pointCount = 0;
    ParameterCurvePoint points[harkenstudio::kAttenuationMaxCurvePoints]{};
    ParameterCurveHandles handles[harkenstudio::kAttenuationMaxCurvePoints - 1]{};
};

struct AttenuationDefinition {
    std::uint32_t rolloff = 0;
    float minDistance = 0;
    float maxDistance = 0;
    float airAbsorptionNearHz = 0;
    float airAbsorptionFarHz = 0;
    float proximityRadius = 0;
    float proximityMaxBoostDb = 0;
    float proximityFrequency = 250;
    std::uint32_t coneEnabled = 0;
    float coneInnerDeg = 0;
    float coneOuterDeg = 0;
    float coneOuterGainDb = 0;
    float coneOuterLowpassHz = 0;
    AttenuationCurve volumeCurve{};
    AttenuationCurve lowpassCurve{};
    AttenuationCurve widthCurve{};
};

struct BindingDefinition {
    harkenstudio::ObjectId variableId{};
    std::uint32_t targetScopeKind = 0;
    std::uint32_t targetId = 0;
    BindingCurve curve{};
    std::uint32_t transitionMs = 0;
};

enum PackResult : std::int32_t {
    kPackOk = 0,
    kPackUnknownId = -200,
    kPackInvalid = -401,
    kPackIncompatible = -410,
    kPackHashMismatch = -412,
    kPackCatalogCapacity = -414,
    kPackResidentCapacity = -415,
};

}
