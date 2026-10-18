#pragma once

#include <QByteArray>
#include <QString>

namespace VNH {

bool writeUpdateFile(const QString& path, const QByteArray& contents);

}
