#pragma once

#include <QJsonObject>
#include <functional>

class QCommandLineParser;

namespace harken::studio::cli {

QJsonObject runSavedProjectBuild(const QCommandLineParser& parser,
                                const std::function<bool()>& canceled);

}
