#include "IpcBridge.h"
#include "RuntimeLink.h"

#include <QFile>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace HarkenEditor {
namespace Bridge {

namespace {

IpcQueryStatus unavailableStatus()
{
    return Detail::runtimeIsActive() ? IpcQueryStatus::Unavailable
                                     : IpcQueryStatus::NoEngine;
}

} // namespace

//==============================================================================
// Plugins.
//==============================================================================

IpcQueryStatus pluginList(std::vector<PluginDesc>& out, uint32_t& outCount)
{
    out.clear();
    outCount = 0;
    return unavailableStatus();
}

//==============================================================================
// Clock / codecs / streams / memory.
//==============================================================================

IpcQueryStatus setClockPll(bool enabled)
{
    Q_UNUSED(enabled);
    return unavailableStatus();
}

IpcQueryStatus clockSyncStatus(ClockSyncStatus& out)
{
    out = {};
    if (!Detail::runtimeIsActive())
        return IpcQueryStatus::NoEngine;
    out.actualSampleRate = static_cast<double>(Detail::runtimeActualSampleRate());
    out.driftPpm = 0.0;
    return IpcQueryStatus::Unavailable;
}

IpcQueryStatus listCodecs(std::vector<CodecDesc>& out)
{
    out.clear();
    return unavailableStatus();
}

IpcQueryStatus codecInfo(uint32_t formatId, CodecDesc& out)
{
    Q_UNUSED(formatId);
    out = {};
    return unavailableStatus();
}

IpcQueryStatus streamStatus(StreamStatusData& out)
{
    out = {};
    return unavailableStatus();
}

IpcQueryStatus streamPositionForVoice(uint32_t voiceId, StreamPositionData& out)
{
    Q_UNUSED(voiceId);
    out = {};
    return unavailableStatus();
}

IpcQueryStatus streamDeviceInfo(uint32_t index, StreamDeviceDesc& out)
{
    Q_UNUSED(index);
    out = {};
    return unavailableStatus();
}

IpcQueryStatus streamDeviceCount(uint32_t& out)
{
    out = 0;
    return unavailableStatus();
}

IpcQueryStatus streamThroughput(StreamThroughputData& out)
{
    out = {};
    return unavailableStatus();
}

IpcQueryStatus mmapStatus(MmapStatusData& out)
{
    out = {};
    return unavailableStatus();
}

IpcQueryStatus deviceFairness(DeviceFairnessData& out)
{
    out = {};
    return unavailableStatus();
}

IpcQueryStatus memoryBudgetStatus(MemoryBudgetStatusData& out)
{
    out = {};
    return unavailableStatus();
}

IpcQueryStatus listenerOutputBus(uint32_t listenerId, ListenerBusBindingData& out)
{
    Q_UNUSED(listenerId);
    out = {};
    return unavailableStatus();
}

int32_t mixerSampleRate()
{
    return Detail::runtimeIsActive() ? Detail::runtimeActualSampleRate() : 0;
}

bool engineSessionActive()
{
    return Detail::runtimeIsActive();
}

IpcQueryStatus cpuBudgetManagerStatus()
{
    return Detail::runtimeIsActive() ? IpcQueryStatus::Ok : IpcQueryStatus::NoEngine;
}

//==============================================================================
// Output device introspection.
//==============================================================================

IpcQueryStatus deviceCapabilities(int32_t deviceIndex, DeviceCapabilityData& out)
{
    Q_UNUSED(deviceIndex);
    out = {};
    return unavailableStatus();
}

IpcQueryStatus activeOutputFormat(ActiveFormatData& out)
{
    out = {};
    return unavailableStatus();
}

IpcQueryStatus hotplugCounts(uint32_t& outEventCount, uint32_t& outDeviceCount)
{
    outEventCount = 0;
    outDeviceCount = 0;
    return unavailableStatus();
}

//==============================================================================
// Monitor / live-parameter introspection.
//==============================================================================

IpcQueryStatus watchedParameters(std::vector<WatchedParamEntry>& out)
{
    out.clear();
    return unavailableStatus();
}

IpcQueryStatus liveParameters(std::vector<LiveParamSnapshotEntry>& out)
{
    out.clear();
    return unavailableStatus();
}

uint64_t monitorGlobalFilterMask()
{
    return 0;
}

//==============================================================================
// Voice pipeline diagnostics.
//==============================================================================

IpcQueryStatus voiceContribution(uint32_t voiceId, VoiceContributionData& out)
{
    Q_UNUSED(voiceId);
    out = {};
    return unavailableStatus();
}

IpcQueryStatus voicePathLatency(uint32_t voiceId, VoicePathLatencyData& out,
                                uint32_t& outRawResult)
{
    Q_UNUSED(voiceId);
    out = {};
    outRawResult = 0;
    return unavailableStatus();
}

//==============================================================================
// Pack queries.
//==============================================================================

IpcQueryStatus packMetrics(const QString& packName, PackMetricsData& out)
{
    Q_UNUSED(packName);
    out = {};
    return unavailableStatus();
}

IpcQueryStatus allPackMetrics(std::vector<PackMetricsSummary>& out)
{
    out.clear();
    return unavailableStatus();
}

IpcQueryStatus packLoadHistory(std::vector<PackLoadHistoryEntry>& out)
{
    out.clear();
    return unavailableStatus();
}

IpcQueryStatus actionRouting(uint32_t actionNameHash, std::vector<ActionRoutingEntry>& out)
{
    Q_UNUSED(actionNameHash);
    out.clear();
    return unavailableStatus();
}

IpcQueryStatus resolveActionPack(uint32_t actionNameHash, uint32_t& outPackId,
                                 QString& outPackName)
{
    Q_UNUSED(actionNameHash);
    outPackId = 0;
    outPackName.clear();
    return unavailableStatus();
}

IpcQueryStatus multiPackCollisions(std::vector<PackCollisionEntry>& out)
{
    out.clear();
    return unavailableStatus();
}

//==============================================================================
// Replay verification.
//==============================================================================

bool compareFiles(const QString& referencePath, const QString& candidatePath,
                  FileCompareResult& out)
{
    out = {};

    QFile ref(referencePath);
    QFile rep(candidatePath);
    if (!ref.open(QIODevice::ReadOnly) || !rep.open(QIODevice::ReadOnly))
        return false;

    out.lengthMatch = true;
    do {
        const QByteArray refBytes = ref.read(64 * 1024);
        const QByteArray repBytes = rep.read(64 * 1024);
        if (ref.error() != QFileDevice::NoError || rep.error() != QFileDevice::NoError)
            return false;
        const auto common = std::min(refBytes.size(), repBytes.size());
        const auto count = std::max(refBytes.size(), repBytes.size());
        out.comparedBytes += static_cast<uint64_t>(count);
        out.differentBytes += static_cast<uint64_t>(count - common);
        out.lengthMatch = out.lengthMatch && refBytes.size() == repBytes.size();
        for (qsizetype i = 0; i < common; ++i)
            if (refBytes[i] != repBytes[i]) ++out.differentBytes;
    } while (!ref.atEnd() || !rep.atEnd());
    out.identical = out.differentBytes == 0;
    return true;
}

} // namespace Bridge
} // namespace HarkenEditor
