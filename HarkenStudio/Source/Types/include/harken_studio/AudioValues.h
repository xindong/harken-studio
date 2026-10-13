#pragma once

#include "ObjectId.h"

namespace harkenstudio {

using ChannelLayout = std::uint32_t;
inline constexpr ChannelLayout kChannelLayoutDefault = 0;
inline constexpr ChannelLayout kChannelLayoutMono = 1;
inline constexpr ChannelLayout kChannelLayoutStereo = 2;
inline constexpr ChannelLayout kChannelLayoutQuad = 3;
inline constexpr ChannelLayout kChannelLayout5_1 = 4;
inline constexpr ChannelLayout kChannelLayout7_1 = 5;

inline constexpr std::uint32_t kEffectFeedbackDelay = 0;
inline constexpr std::uint32_t kEffectThreeBandTone = 1;
inline constexpr std::uint32_t kEffectGain = 2;
inline constexpr std::uint32_t kEffectLimiter = 3;
inline constexpr std::uint32_t kEffectCompressor = 4;
inline constexpr std::uint32_t kDynamicSignalRawAudio = 2;
inline constexpr std::uint32_t kDynamicSourceBus = 2;
inline constexpr std::uint32_t kDynamicTapBusPostEffects = 0;
inline constexpr std::uint32_t kDynamicTapBusPreFader = 1;
inline constexpr std::uint32_t kDynamicTapBusPostFader = 2;
inline constexpr std::uint32_t kRolloffLinear = 0;
inline constexpr std::uint32_t kRolloffInverseSquare = 1;
inline constexpr std::uint32_t kRolloffLogarithmic = 2;
inline constexpr std::uint32_t kTargetScopeVoice = 0;
inline constexpr std::uint32_t kTargetScopeBus = 1;
inline constexpr std::uint32_t kTargetScopeVoiceEffectParam = 2;
inline constexpr std::uint32_t kTargetScopeBusEffectParam = 3;
inline constexpr std::uint32_t kCurveMaxPoints = 8;
inline constexpr std::uint32_t kAttenuationMaxCurvePoints = 8;

}
