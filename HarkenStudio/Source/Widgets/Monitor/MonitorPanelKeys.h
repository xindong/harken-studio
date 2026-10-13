#pragma once

#include <QStringList>

#include <cstdint>

namespace VNX {

inline constexpr std::uint64_t kMonitorCursorContributorVoices = 2;
inline constexpr std::uint64_t kMonitorCursorContributorPerformance = 3;
inline constexpr std::uint64_t kMonitorCursorContributorPlayables = 4;
inline constexpr std::uint64_t kMonitorCursorContributorEffects = 5;
inline constexpr std::uint64_t kMonitorCursorContributorBuses = 6;
inline constexpr std::uint64_t kMonitorCursorContributorDspTimeline = 7;
inline constexpr std::uint64_t kMonitorCursorContributorLineage = 8;
inline constexpr std::uint64_t kMonitorCursorContributorTapSelect = 10;
inline constexpr std::uint64_t kMonitorCursorContributorWaveform = 11;
inline constexpr std::uint64_t kMonitorCursorContributorScope = 12;
inline constexpr std::uint64_t kMonitorCursorContributorSpectrum = 13;
inline constexpr std::uint64_t kMonitorCursorContributorRecording = 15;
inline constexpr std::uint64_t kMonitorCursorContributorSessionCompare = 16;
inline constexpr std::uint64_t kMonitorCursorContributorStreaming = 17;
inline constexpr std::uint64_t kMonitorCursorContributorMemory = 18;
inline constexpr std::uint64_t kMonitorCursorContributorGameModulate = 19;
inline constexpr std::uint64_t kMonitorCursorContributorEmitters = 20;

QStringList monitorPanelTypeIds();
QStringList monitorPanelTitles();
QStringList monitorPanelDockObjectNames();
QStringList monitorViewSortKeys();

} // namespace VNX
