#pragma once

#include <QJsonObject>
#include <QString>
#include <optional>

class QWidget;

namespace harken::studio {

std::optional<QString> chooseVariableRangePolicy(QWidget* parent, const QJsonObject& change);

}
