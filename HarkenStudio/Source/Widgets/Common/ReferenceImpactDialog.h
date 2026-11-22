#pragma once

#include <QString>

class QWidget;

namespace harken::studio {

bool confirmReferenceImpact(QWidget* parent, const QString& title,
                            const QString& summary, const QString& details,
                            const QString& confirmText, bool destructive,
                            const QString& objectName);

}
