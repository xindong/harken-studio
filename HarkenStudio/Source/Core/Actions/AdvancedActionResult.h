#pragma once

#include "../ActionManager.h"
#include "RuntimeBridge/PlaybackBridge.h"

namespace harken::studio::AdvancedActionDetail {

ActionResult fromEngineResult(
    bool engineInitialized, harken::studio::bridge::EngineResult result,
    QJsonObject successData, const QString& operation);

}
