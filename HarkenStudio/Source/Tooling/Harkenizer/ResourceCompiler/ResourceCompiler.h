#pragma once

#include <string>
#include <vector>

namespace harkenizer::resourcecompiler {

enum class Container { Pack, Stream };

struct Request {
    std::vector<std::string> manifestPaths;
    Container container = Container::Pack;
    bool disableCache = false;
    std::string reportPath;
    std::string cachePath;
    std::string emitManifestPath;
};

struct Result {
    bool success = false;
    std::string error;
    std::string reportPath;
    std::string outputLog;
    std::string diagnosticLog;
};

Result compile(const Request& request);

}
