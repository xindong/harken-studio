#pragma once

#include <QString>

namespace harken::studio {

// Internal deploy seam: atomically publish a staged bundle directory.
// Not a public Studio API; tests inject a native sharing conflict here.
bool activateRuntimeContentBundleDirectory(
    const QString& source, const QString& target, QString* error);

} // namespace harken::studio
