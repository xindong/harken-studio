#include "Monitor/MonitorPanelKeys.h"

namespace VNX {

QStringList monitorPanelTypeIds()
{
    return {
        QStringLiteral("MonitorPerformance"),
        QStringLiteral("Monitor"),
        QStringLiteral("MonitorVoices"),
        QStringLiteral("MonitorPlayables"),
        QStringLiteral("MonitorEffects"),
        QStringLiteral("MonitorBuses"),
        QStringLiteral("MonitorDspTimeline"),
        QStringLiteral("MonitorLineage"),
        QStringLiteral("MonitorTapSelect"),
        QStringLiteral("MonitorWaveform"),
        QStringLiteral("MonitorScope"),
        QStringLiteral("MonitorSpectrum"),
        QStringLiteral("MonitorRecording"),
        QStringLiteral("MonitorSessionCompare"),
        QStringLiteral("MonitorStreaming"),
        QStringLiteral("MonitorMemory"),
        QStringLiteral("MonitorGameModulate"),
        QStringLiteral("MonitorEmitters"),
    };
}

QStringList monitorPanelTitles()
{
    return {
        QStringLiteral("Performance Monitor"),
        QStringLiteral("Monitor"),
        QStringLiteral("Voices"),
        QStringLiteral("Playables & Actions"),
        QStringLiteral("Effects"),
        QStringLiteral("Buses"),
        QStringLiteral("DSP Timeline"),
        QStringLiteral("Lineage"),
        QStringLiteral("Tap Select"),
        QStringLiteral("Waveform"),
        QStringLiteral("Scope"),
        QStringLiteral("Spectrum"),
        QStringLiteral("Recording"),
        QStringLiteral("Session Compare"),
        QStringLiteral("Streaming"),
        QStringLiteral("Memory"),
        QStringLiteral("Game Modulate"),
        QStringLiteral("Emitters"),
    };
}

QStringList monitorPanelDockObjectNames()
{
    return {
        QStringLiteral("MonitorPerformanceDock"),
        QStringLiteral("MonitorDock"),
        QStringLiteral("MonitorVoicesDock"),
        QStringLiteral("MonitorPlayablesDock"),
        QStringLiteral("MonitorEffectsDock"),
        QStringLiteral("MonitorBusesDock"),
        QStringLiteral("MonitorDspTimelineDock"),
        QStringLiteral("MonitorLineageDock"),
        QStringLiteral("MonitorTapSelectDock"),
        QStringLiteral("MonitorWaveformDock"),
        QStringLiteral("MonitorScopeDock"),
        QStringLiteral("MonitorSpectrumDock"),
        QStringLiteral("MonitorRecordingDock"),
        QStringLiteral("MonitorSessionCompareDock"),
        QStringLiteral("MonitorStreamingDock"),
        QStringLiteral("MonitorMemoryDock"),
        QStringLiteral("MonitorGameModulateDock"),
        QStringLiteral("MonitorEmittersDock"),
    };
}

} // namespace VNX
