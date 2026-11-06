#pragma once

#include <QJsonObject>
#include <QString>
#include <optional>

class QWidget;

namespace VNX {

std::optional<QString> chooseVariableRangePolicy(QWidget* parent, const QJsonObject& change);

}
