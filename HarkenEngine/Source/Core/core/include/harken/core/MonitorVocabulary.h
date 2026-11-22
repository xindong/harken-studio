#ifndef HARKEN_CORE_MONITOR_VOCABULARY_H
#define HARKEN_CORE_MONITOR_VOCABULARY_H

#include <cstdint>

namespace harken::monitor {

inline constexpr std::uint32_t kContentAction = 5;
inline constexpr std::uint32_t kContentMedia = 6;
inline constexpr std::uint32_t kContentSound = 1;
inline constexpr std::uint32_t kEntityAction = 3;
inline constexpr std::uint32_t kEntityNone = 0;
inline constexpr std::uint32_t kEntityPlayable = 2;
inline constexpr std::uint32_t kEntityVoice = 4;
inline constexpr std::uint32_t kOperationLoadPack = 10;
inline constexpr std::uint32_t kOperationPlayPreview = 3;
inline constexpr std::uint32_t kOperationPostAction = 1;
inline constexpr std::uint32_t kOperationResetValues = 19;
inline constexpr std::uint32_t kOperationSetEmitterValue = 9;
inline constexpr std::uint32_t kOperationSetEmitterValues = 13;
inline constexpr std::uint32_t kOperationSetValue = 8;
inline constexpr std::uint32_t kOperationSetValues = 12;
inline constexpr std::uint32_t kOperationStopAction = 6;
inline constexpr std::uint32_t kOperationStopAll = 7;
inline constexpr std::uint32_t kOperationStopEmitter = 16;
inline constexpr std::uint32_t kOperationStopPreview = 4;
inline constexpr std::uint32_t kOperationStopVoice = 5;
inline constexpr std::uint32_t kOperationUnloadPack = 11;
inline constexpr std::uint32_t kPlayableProvenanceKnown = 1;
inline constexpr std::uint32_t kSubjectAction = 2;
inline constexpr std::uint32_t kSubjectLogicalVoice = 3;
inline constexpr std::uint32_t kSubjectPlayable = 1;
inline constexpr std::uint32_t kTargetAction = 2;
inline constexpr std::uint32_t kTargetEmitter = 5;
inline constexpr std::uint32_t kTargetLogicalVoice = 3;

} // namespace harken::monitor

#endif
