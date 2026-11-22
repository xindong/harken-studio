
#include "AudioEngine.h"

namespace harken::studio {


bool AudioEngine::getDebugVoices(std::vector<DebugVoiceData>& out) const
{
    return bridge::getDebugVoices(out);
}

bool AudioEngine::getDebugBuses(std::vector<DebugBusData>& out) const
{
    return bridge::getDebugBuses(out);
}

bool AudioEngine::getDebugPerformance(DebugPerfData& out) const
{
    return bridge::getDebugPerformance(out);
}

bool AudioEngine::getSessionRecap(SessionRecapData& out) const
{
    return bridge::getSessionRecap(out);
}

bool AudioEngine::getCpuBudgetState(CpuBudgetState& out) const
{
    return bridge::getCpuBudgetState(out);
}

bool AudioEngine::getRenderPlan(RenderPlanInfo& out) const
{
    return bridge::getRenderPlan(out);
}

bool AudioEngine::getJobStats(JobStatsInfo& out) const
{
    return bridge::getJobStats(out);
}

bool AudioEngine::getObjectAudioStatus(ObjectAudioStatusInfo& out) const
{
    return bridge::getObjectAudioStatus(out);
}

bool AudioEngine::setSpatialFallbackPolicy(uint8_t policy)
{
    Q_UNUSED(policy);
    return false;
}

bool AudioEngine::getBusTopologyPlanCount(uint64_t& outCount) const
{
    return bridge::getBusTopologyPlanCount(outCount);
}

bool AudioEngine::getVoiceMetadataChain(uint32_t voiceId,
                                        std::vector<DebugVoiceMetadataNode>& outChain) const
{
    return bridge::getVoiceMetadataChain(voiceId, outChain);
}

}
