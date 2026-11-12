#pragma once

#include <QString>
#include <cstdint>
#include <memory>
#include <vector>

namespace HarkenEditor::Bridge {

class MediaServices
{
public:
    virtual ~MediaServices() = default;
    virtual bool decodeInterleaved(const QString& path, std::vector<float>& outInterleaved,
                                   int& outChannels, int& outSampleRate) = 0;
    virtual uint64_t pcmContentRevision(const std::vector<float>& samples, uint32_t channels,
                                        uint32_t sampleRate, uint64_t loopStartFrame,
                                        uint64_t loopEndFrame) = 0;
};

void setMediaServices(std::shared_ptr<MediaServices> services);
std::shared_ptr<MediaServices> mediaServices();

}
