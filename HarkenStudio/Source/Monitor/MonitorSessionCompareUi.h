#pragma once

#include "MonitorSessionFile.h"

#include "Monitor/MonitorTypes.h"

#include <cstdint>
#include <string>

namespace VNX {
namespace Monitor {

enum class MonitorComparePicker
{
    NotWired = 0,
    MissingSecondFile,
    Loaded,
};

struct MonitorCompareSummary
{
    uint32_t result = kMonitorCompareMissingData;
    const char* resultText = "MissingData";
    std::string identity;
    std::string timestamp;
    std::string reason;
    std::string capability;
};

struct MonitorCompareUiState
{
    MonitorComparePicker picker = MonitorComparePicker::NotWired;
    bool pickerWired = false;
    MonitorResult callResult = kNotInitialized;
    uint32_t compareResult = kMonitorCompareMissingData;
    MonitorCompareSummary summary {};
};

struct MonitorCompareUiInput
{
    bool pickerWired = false;
    bool leftPresent = false;
    bool rightPresent = false;
    bool readInterrupted = false;
    MonitorSessionCompareView left {};
    MonitorSessionCompareView right {};
    std::string identity;
    std::string timestamp;
};

MonitorSessionCompareView projectCompareView(const MonitorSessionFileResult& file);
MonitorCompareUiState projectCompareUi(const MonitorCompareUiInput& input);
std::string compareExportText(const MonitorCompareSummary& summary);
const char* compareResultText(uint32_t result);

} // namespace Monitor
} // namespace VNX
