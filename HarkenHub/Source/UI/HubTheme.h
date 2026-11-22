#pragma once

#include <QColor>
#include <QString>
#include <functional>

class QWidget;

namespace harken::hub::HubTheme {

enum class Tone { Text, Muted, Accent, Warning, Error, Success, Info };

void initialize();
void applyPreference();
QColor color(Tone tone);
QString html(QString text);
void setTone(QWidget* widget, Tone tone);
void observe(QWidget* widget, std::function<void()> callback);

}
