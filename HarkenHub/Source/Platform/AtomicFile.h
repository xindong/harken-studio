#pragma once

#include <QByteArray>
#include <QString>

namespace harken::hub {

bool writeAtomicFile(const QString& path, const QByteArray& contents,
                     QString* error = nullptr);

}
