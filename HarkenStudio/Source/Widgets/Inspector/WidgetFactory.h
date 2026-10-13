
#pragma once

#include <QWidget>
#include <QJsonObject>
#include <QString>
#include <QMap>
#include <functional>

namespace VNX {

class WidgetFactory
{
public:
    using WidgetCreator = std::function<QWidget*(const QJsonObject& config, QWidget* parent)>;

    static WidgetFactory& instance();

    void registerType(const QString& typeName, WidgetCreator creator);

    QWidget* create(const QString& typeName, const QJsonObject& config, QWidget* parent = nullptr) const;

    QStringList registeredTypes() const;

    bool hasType(const QString& typeName) const;

    using CustomWidgetCreator = std::function<QWidget*(const QJsonObject& config, QWidget* parent)>;
    void registerCustomWidget(const QString& name, CustomWidgetCreator creator);
    bool hasCustomWidget(const QString& name) const;

private:
    WidgetFactory();
    void registerBuiltins();

    QMap<QString, WidgetCreator>       creators_;
    QMap<QString, CustomWidgetCreator> customWidgets_;
};

}
