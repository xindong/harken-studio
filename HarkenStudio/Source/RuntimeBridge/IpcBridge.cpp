#include "IpcBridge.h"
#include "RuntimeLink.h"

#include <QFile>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

namespace harken::studio {
namespace bridge {

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

uint64_t monitorGlobalFilterMask()
{
    return 0;
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

} // namespace bridge
} // namespace harken::studio
