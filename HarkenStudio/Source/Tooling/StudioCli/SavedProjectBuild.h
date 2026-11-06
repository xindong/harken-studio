#pragma once

#include <QJsonObject>
#include <functional>

class QCommandLineParser;

namespace HarkenStudioCli {

QJsonObject runSavedProjectBuild(const QCommandLineParser& parser,
                                const std::function<bool()>& canceled);

}
