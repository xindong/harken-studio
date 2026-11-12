
#pragma once

#include "Core/AssetPipeline/AssetTypes.h"

#include <QString>
#include <QStringList>

namespace VNX {

QString assetDisplayName(const AssetInfo& asset);
QString assetDisplayPath(const AssetInfo& asset);
QStringList assetFolderSegments(const AssetInfo& asset);
QString assetTooltip(const AssetInfo& asset);

}
