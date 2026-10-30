#include "ResourceWorker.h"
#include "ResourceCompilerClient.h"
#include "ResourceCompiler/ResourceCompiler.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace Harkenizer {

int runResourceWorker()
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    const auto respond = [](const QJsonObject& response) {
        const auto bytes = QJsonDocument(response).toJson(QJsonDocument::Compact);
        std::cout.write(bytes.constData(), bytes.size());
        std::cout << '\n' << std::flush;
        return std::cout.good();
    };
    if (!respond({{QStringLiteral("protocol"), 1},
                  {QStringLiteral("compiler"), QLatin1String(resourceCompilerIdentity())}}))
        return 1;
    std::string line;
    while (std::getline(std::cin, line)) {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(QByteArray::fromStdString(line), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()
            || !document.object().value(QStringLiteral("manifest")).isString())
            return 2;
        harkenizer::resourcecompiler::Request request;
        request.manifestPaths.push_back(document.object().value(QStringLiteral("manifest")).toString().toStdString());
        const auto result = harkenizer::resourcecompiler::compile(request);
        if (!respond({{QStringLiteral("success"), result.success},
                      {QStringLiteral("error"), QString::fromStdString(result.error)},
                      {QStringLiteral("outputLog"), QString::fromStdString(result.outputLog)},
                      {QStringLiteral("diagnosticLog"), QString::fromStdString(result.diagnosticLog)}}))
            return 1;
    }
    return std::cin.eof() ? 0 : 1;
}

}
