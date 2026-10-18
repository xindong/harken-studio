#pragma once

#include <QByteArray>
#include <QString>

namespace VNH {

bool writeAtomicFile(const QString& path, const QByteArray& contents,
                     QString* error = nullptr);

}
