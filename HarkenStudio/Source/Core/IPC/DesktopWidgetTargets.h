#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <functional>

class QWidget;

namespace VNX {

QString desktopWidgetId(QWidget* widget);
QList<QWidget*> desktopWidgets();
QWidget* resolveDesktopWidget(const QJsonObject& args, QJsonObject* error,
                              bool interaction = false, QWidget* defaultTarget = nullptr);
QJsonObject desktopWidgetInteractionError(QWidget* widget);
QJsonObject queueDesktopWidgetOperation(QWidget* widget,
    std::function<QJsonObject(QWidget*)> operation, QJsonObject details = {},
    std::function<QJsonObject()> canExecute = {});
QJsonObject desktopUiOperation(const QString& operationId);

}
