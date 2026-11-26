#ifndef HARKEN_PACK_CURVE_H
#define HARKEN_PACK_CURVE_H

#include "PackFile.h"

typedef struct HkPackAttenuationCurvePoint {
    float x;
    float y;
} HkPackAttenuationCurvePoint;
HK_PACK_SIZE(HkPackAttenuationCurvePoint, 8);
HK_PACK_OFFSET(HkPackAttenuationCurvePoint, x, 0);
HK_PACK_OFFSET(HkPackAttenuationCurvePoint, y, 4);

typedef struct HkPackCurveHandles {
    float h1x;
    float h1y;
    float h2x;
    float h2y;
} HkPackCurveHandles;
HK_PACK_SIZE(HkPackCurveHandles, 16);
HK_PACK_OFFSET(HkPackCurveHandles, h1x, 0);
HK_PACK_OFFSET(HkPackCurveHandles, h1y, 4);
HK_PACK_OFFSET(HkPackCurveHandles, h2x, 8);
HK_PACK_OFFSET(HkPackCurveHandles, h2y, 12);

typedef struct HkPackAttenuationCurve {
    uint32_t enabled;
    uint32_t pointCount;
    HkPackAttenuationCurvePoint points[8];
    HkPackCurveHandles handles[7];
} HkPackAttenuationCurve;
HK_PACK_SIZE(HkPackAttenuationCurve, 184);
HK_PACK_OFFSET(HkPackAttenuationCurve, enabled, 0);
HK_PACK_OFFSET(HkPackAttenuationCurve, pointCount, 4);
HK_PACK_OFFSET(HkPackAttenuationCurve, points, 8);
HK_PACK_OFFSET(HkPackAttenuationCurve, handles, 72);

typedef struct HkPackAttenuation {
    uint32_t rolloff;
    float minDistance;
    float maxDistance;
    float airAbsorptionNearHz;
    float airAbsorptionFarHz;
    float proximityRadius;
    float proximityMaxBoostDb;
    uint32_t coneEnabled;
    float coneInnerDeg;
    float coneOuterDeg;
    float coneOuterGainDb;
    float coneOuterLowpassHz;
    HkPackAttenuationCurve volumeCurve;
    HkPackAttenuationCurve lowpassCurve;
    HkPackAttenuationCurve widthCurve;
    float proximityFrequency;
    uint32_t reserved604;
} HkPackAttenuation;
HK_PACK_SIZE(HkPackAttenuation, 608);
HK_PACK_OFFSET(HkPackAttenuation, rolloff, 0);
HK_PACK_OFFSET(HkPackAttenuation, minDistance, 4);
HK_PACK_OFFSET(HkPackAttenuation, maxDistance, 8);
HK_PACK_OFFSET(HkPackAttenuation, airAbsorptionNearHz, 12);
HK_PACK_OFFSET(HkPackAttenuation, airAbsorptionFarHz, 16);
HK_PACK_OFFSET(HkPackAttenuation, proximityRadius, 20);
HK_PACK_OFFSET(HkPackAttenuation, proximityMaxBoostDb, 24);
HK_PACK_OFFSET(HkPackAttenuation, coneEnabled, 28);
HK_PACK_OFFSET(HkPackAttenuation, coneInnerDeg, 32);
HK_PACK_OFFSET(HkPackAttenuation, coneOuterDeg, 36);
HK_PACK_OFFSET(HkPackAttenuation, coneOuterGainDb, 40);
HK_PACK_OFFSET(HkPackAttenuation, coneOuterLowpassHz, 44);
HK_PACK_OFFSET(HkPackAttenuation, volumeCurve, 48);
HK_PACK_OFFSET(HkPackAttenuation, lowpassCurve, 232);
HK_PACK_OFFSET(HkPackAttenuation, widthCurve, 416);
HK_PACK_OFFSET(HkPackAttenuation, proximityFrequency, 600);
HK_PACK_OFFSET(HkPackAttenuation, reserved604, 604);

typedef struct HkPackCurvePoint {
    float x;
    float y;
} HkPackCurvePoint;
HK_PACK_SIZE(HkPackCurvePoint, 8);
HK_PACK_OFFSET(HkPackCurvePoint, x, 0);
HK_PACK_OFFSET(HkPackCurvePoint, y, 4);

typedef struct HkPackBindingCurve {
    uint32_t pointCount;
    HkPackCurvePoint points[8];
    HkPackCurveHandles handles[7];
} HkPackBindingCurve;
HK_PACK_SIZE(HkPackBindingCurve, 180);
HK_PACK_OFFSET(HkPackBindingCurve, pointCount, 0);
HK_PACK_OFFSET(HkPackBindingCurve, points, 4);
HK_PACK_OFFSET(HkPackBindingCurve, handles, 68);

#endif
