
#include "AudioEngine.h"

namespace VNX {

namespace Bridge = HarkenEditor::Bridge;

bool AudioEngine::getDebugVoices(std::vector<DebugVoiceData>& out) const
{
    return Bridge::getDebugVoices(out);
}

bool AudioEngine::getDebugBuses(std::vector<DebugBusData>& out) const
{
    return Bridge::getDebugBuses(out);
}

bool AudioEngine::getDebugPerformance(DebugPerfData& out) const
{
    return Bridge::getDebugPerformance(out);
}

bool AudioEngine::getSessionRecap(SessionRecapData& out) const
{
    return Bridge::getSessionRecap(out);
}

bool AudioEngine::getCpuBudgetState(CpuBudgetState& out) const
{
    return Bridge::getCpuBudgetState(out);
}

bool AudioEngine::getRenderPlan(RenderPlanInfo& out) const
{
    return Bridge::getRenderPlan(out);
}

bool AudioEngine::getJobStats(JobStatsInfo& out) const
{
    return Bridge::getJobStats(out);
}

bool AudioEngine::getObjectAudioStatus(ObjectAudioStatusInfo& out) const
{
    return Bridge::getObjectAudioStatus(out);
}

bool AudioEngine::setSpatialFallbackPolicy(uint8_t policy)
{
    Q_UNUSED(policy);
    return false;
}

bool AudioEngine::getBusTopologyPlanCount(uint64_t& outCount) const
{
    return Bridge::getBusTopologyPlanCount(outCount);
}

bool AudioEngine::getVoiceMetadataChain(uint32_t voiceId,
                                        std::vector<DebugVoiceMetadataNode>& outChain) const
{
    return Bridge::getVoiceMetadataChain(voiceId, outChain);
}

}
