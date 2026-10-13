#include "UnitTest/StudioObjectIdTestSupport.h"
#include "RuntimeBridge/IpcBridge.h"
#include "RuntimeBridge/NativeDiagnostics.h"

#include <gtest/gtest.h>

namespace Bridge = HarkenEditor::Bridge;

TEST(EditorCapabilityMap, UnavailableStatusIsPartOfQueryVocabulary)
{
    EXPECT_EQ(static_cast<int>(Bridge::IpcQueryStatus::Unavailable), 5);
}

TEST(EditorCapabilityMap, UnavailableSurfacesDoNotReportSuccessWithoutSession)
{
    std::vector<Bridge::PluginDesc> plugins;
    uint32_t pluginCount = 99;
    EXPECT_EQ(Bridge::IpcQueryStatus::NoEngine,
              Bridge::pluginList(plugins, pluginCount));
    EXPECT_TRUE(plugins.empty());
    EXPECT_EQ(pluginCount, 0u);

    std::vector<Bridge::CodecDesc> codecs;
    EXPECT_EQ(Bridge::IpcQueryStatus::NoEngine,
              Bridge::listCodecs(codecs));
    EXPECT_TRUE(codecs.empty());

    EXPECT_EQ(Bridge::monitorGlobalFilterMask(), 0u);
}

TEST(EditorCapabilityMap, DiagnosticsSnapshotSurfacesNeedRuntimeSession)
{
    std::vector<Bridge::DebugVoiceData> voices;
    EXPECT_FALSE(Bridge::getDebugVoices(voices));
    EXPECT_TRUE(voices.empty());

    std::vector<Bridge::DebugBusData> buses;
    EXPECT_FALSE(Bridge::getDebugBuses(buses));
    EXPECT_TRUE(buses.empty());

    Bridge::DebugPerfData perf;
    EXPECT_FALSE(Bridge::getDebugPerformance(perf));

    Bridge::SessionRecapData recap;
    EXPECT_FALSE(Bridge::getSessionRecap(recap));

    Bridge::CpuBudgetState budget;
    EXPECT_FALSE(Bridge::getCpuBudgetState(budget));

    Bridge::JobStatsInfo jobs;
    EXPECT_FALSE(Bridge::getJobStats(jobs));

    Bridge::ObjectAudioStatusInfo objectStatus;
    EXPECT_FALSE(Bridge::getObjectAudioStatus(objectStatus));

    Bridge::VoiceStateBreakdown breakdown;
    EXPECT_FALSE(Bridge::getVoiceStateBreakdown(breakdown));

    uint64_t topologyPlanCount = 1;
    EXPECT_FALSE(Bridge::getBusTopologyPlanCount(topologyPlanCount));
    EXPECT_EQ(topologyPlanCount, 0u);
}

TEST(EditorCapabilityMap, ModulatorDiagnosticsReportNoEngineBeforeUnavailable)
{
    std::vector<Bridge::ActiveModulatorInfo> modulators;
    EXPECT_EQ(Bridge::IpcQueryStatus::NoEngine,
              Bridge::listActiveModulators(modulators));
    EXPECT_TRUE(modulators.empty());

    std::vector<Bridge::ActiveModulatorChainInfo> chains;
    EXPECT_EQ(Bridge::IpcQueryStatus::NoEngine,
              Bridge::listActiveModulatorChains(chains));
    EXPECT_TRUE(chains.empty());
}
