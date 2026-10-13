#pragma once

#include "DiagnosticsBridge.h"
#include <harken/Harken.h>

namespace HarkenEditor::Bridge {

void mapSnapshotToDebugVoices(const VnSnapshot& snapshot, std::vector<DebugVoiceData>& out);
void mapSnapshotToDebugBuses(const VnSnapshot& snapshot, std::vector<DebugBusData>& out);
bool mapSnapshotToRenderPlan(const VnSnapshot& snapshot, RenderPlanInfo& out);
void mapSnapshotToVoiceInfoList(const VnSnapshot& snapshot, std::vector<VoiceInfoData>& out);

}
