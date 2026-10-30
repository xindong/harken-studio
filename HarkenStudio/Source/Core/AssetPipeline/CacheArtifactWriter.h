#pragma once

#include <QString>
#include <functional>

class QIODevice;

namespace VNX {

bool writeCacheArtifact(const QString& path, const std::function<bool(QIODevice&)>& write);

}
