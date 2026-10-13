#pragma once

#include <QString>

namespace VNH {

QString updateBridgeBinaryName();

QString deployUpdateBridgePayload(const QString& sourceDir,
                                  const QString& targetRoot,
                                  const QString& buildId,
                                  const QString& rootSddl, QString* error);
void pruneUpdateBridgePayloads(const QString& targetRoot,
                              const QString& deployedExe);

}
