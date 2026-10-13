#pragma once

#include "../ActionManager.h"
#include "RuntimeBridge/PlaybackBridge.h"

namespace VNX::AdvancedActionDetail {

ActionResult fromEngineResult(
    bool engineInitialized, HarkenEditor::Bridge::EngineResult result,
    QJsonObject successData, const QString& operation);

}
