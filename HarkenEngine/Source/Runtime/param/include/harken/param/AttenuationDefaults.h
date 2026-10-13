// Built-in physically-motivated attenuation defaults: pure functions
// mapping a voice's distance (and cone angle) to parameter contributions.
// A profile that omits an override curve for a target evaluates these
// instead. All evaluation happens on the logic thread at tick rate.
//
// Formulations:
//   - Distance rolloff models follow the OpenAL 1.1 specification's
//     distance model family (linear / inverse / exponential attenuation
//     between a reference and a maximum distance), expressed here as a
//     normalized gain g in [0, 1] converted to a dB contribution.
//   - The directional cone follows the OpenAL 1.1 inner/outer cone model:
//     full gain inside the inner angle, interpolation to an outer gain
//     across the transition zone.
//   - Air absorption as a distance-interpolated lowpass in the
//     log-frequency domain is the standard perceptual mapping (equal
//     musical intervals per distance step).
#ifndef HARKEN_PARAM_ATTENUATION_DEFAULTS_H
#define HARKEN_PARAM_ATTENUATION_DEFAULTS_H

#include <harken/Harken.h>

#include "harken/param/ParamTargets.h"

#include <cmath>
#include <cstdint>

namespace harken {

// Silence floor of the dB conversion: a rolloff gain of zero (or anything
// below this floor) contributes exactly -96 dB instead of -infinity, so
// the combine model only ever sees finite values. -96 dB is 16-bit
// quantization depth — inaudibly below any real mix bus.
inline constexpr float kAttenuationFloorDb = -96.0f;

// The cone's "unrestricted" lowpass reference: interpolation toward
// outerLowpassHz starts from this audible-band-open cutoff. 20 kHz is the
// upper edge of the audible band, so a cutoff at or above it does not
// shape anything the engine renders.
inline constexpr float kConeOpenLowpassHz = 20000.0f;

// Normalized distance position t in [0, 1] between dmin and dmax; the
// shared abscissa of the air-absorption and width defaults.
inline float DistanceParam(float d, float dmin, float dmax) {
    if (!(dmax > dmin)) {
        return 0.0f;
    }
    const float t = (d - dmin) / (dmax - dmin);
    return (t < 0.0f) ? 0.0f : (t > 1.0f) ? 1.0f : t;
}

// Distance rolloff gain g in [0, 1] for the chosen model:
//   linear:         g = 1 - (d - dmin) / (dmax - dmin)
//   inverse-square: g = (dmin / max(d, dmin))^2
//   logarithmic:    g = 1 - ln(d / dmin) / ln(dmax / dmin)
// all clamped into [0, 1].
inline float RolloffGain(std::uint32_t rolloff, float d, float dmin,
                         float dmax) {
    float g = 1.0f;
    switch (rolloff) {
    case VN_ROLLOFF_LINEAR:
        g = 1.0f - DistanceParam(d, dmin, dmax);
        break;
    case VN_ROLLOFF_INVERSE_SQUARE: {
        const float dc = (d > dmin) ? d : dmin;
        const float r = dmin / dc;
        g = r * r;
        break;
    }
    case VN_ROLLOFF_LOGARITHMIC:
        g = 1.0f - std::log(d / dmin) / std::log(dmax / dmin);
        break;
    default:
        break;
    }
    return (g < 0.0f) ? 0.0f : (g > 1.0f) ? 1.0f : g;
}

// Converts a rolloff gain to the volume target's dB contribution:
// 20*log10(g), floored at kAttenuationFloorDb (g = 0 lands exactly on the
// floor instead of -infinity).
inline float RolloffGainToDb(float g) {
    if (g <= 0.0f) {
        return kAttenuationFloorDb;
    }
    const float db = 20.0f * std::log10(g);
    return (db < kAttenuationFloorDb) ? kAttenuationFloorDb : db;
}

// Air-absorption lowpass cutoff: interpolated in the log-frequency domain
// so equal distance steps sweep equal musical intervals —
// ln fc = lerp(ln fcNear, ln fcFar, t). Returns Hz for the lowpass
// target's MinHz contribution. The endpoints (and a degenerate equal
// pair) pass through exactly: the contract says the cutoff IS fcNear at
// minDistance and fcFar at maxDistance, and an exp(log(fc)) round trip
// would drift those authored values in float.
inline float AirAbsorptionHz(float d, float dmin, float dmax, float fcNear,
                             float fcFar) {
    const float t = DistanceParam(d, dmin, dmax);
    if (t <= 0.0f || fcNear == fcFar) {
        return fcNear;
    }
    if (t >= 1.0f) {
        return fcFar;
    }
    const float lnFc =
        std::log(fcNear) + (std::log(fcFar) - std::log(fcNear)) * t;
    return std::exp(lnFc);
}

// Stereo width collapse: w = 1 - t. A near emitter fills the image
// (w = 1); a distant one collapses to a panned point (w = 0), matching
// how real sources narrow with distance.
inline float WidthCollapse(float d, float dmin, float dmax) {
    return 1.0f - DistanceParam(d, dmin, dmax);
}

// Near-field low-frequency emphasis: inside the proximity radius the
// low-shelf boost rises linearly from 0 dB at the radius to maxBoostDb at
// distance zero; outside it contributes nothing.
inline float NearFieldShelfDb(float d, float proximityRadius,
                              float maxBoostDb) {
    if (!(proximityRadius > 0.0f) || d >= proximityRadius) {
        return 0.0f;
    }
    const float dc = (d < 0.0f) ? 0.0f : d;
    return maxBoostDb * (1.0f - dc / proximityRadius);
}

// Directional cone gain: theta is the angle in degrees between the
// emitter's forward axis and the direction toward the listener.
// theta <= inner contributes 0 dB; the transition zone interpolates
// linearly in the dB domain to outerGainDb; theta >= outer holds
// outerGainDb.
inline float ConeGainDb(float thetaDeg, float innerDeg, float outerDeg,
                        float outerGainDb) {
    if (thetaDeg <= innerDeg) {
        return 0.0f;
    }
    if (thetaDeg >= outerDeg || !(outerDeg > innerDeg)) {
        return outerGainDb;
    }
    const float t = (thetaDeg - innerDeg) / (outerDeg - innerDeg);
    return outerGainDb * t;
}

// Directional cone lowpass: inside the inner angle the cone does not
// restrict the spectrum (returns the fully-open sentinel); across the
// transition zone the cutoff interpolates in the log-frequency domain
// from the audible-band-open reference (kConeOpenLowpassHz) down to
// outerLowpassHz, holding outerLowpassHz at and beyond the outer angle.
inline float ConeLowpassHz(float thetaDeg, float innerDeg, float outerDeg,
                           float outerLowpassHz) {
    if (thetaDeg <= innerDeg) {
        return kLowpassOpenHz;
    }
    if (thetaDeg >= outerDeg || !(outerDeg > innerDeg)) {
        return outerLowpassHz;
    }
    const float t = (thetaDeg - innerDeg) / (outerDeg - innerDeg);
    const float lnFc = std::log(kConeOpenLowpassHz) +
                       (std::log(outerLowpassHz) -
                        std::log(kConeOpenLowpassHz)) *
                           t;
    return std::exp(lnFc);
}

} // namespace harken

#endif // HARKEN_PARAM_ATTENUATION_DEFAULTS_H
