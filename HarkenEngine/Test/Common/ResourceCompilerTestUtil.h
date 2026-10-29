#pragma once

#include "ResourceCompiler.h"

#include <cstdio>
#include <string>
#include <vector>

namespace harkentest {

inline int CompileResources(const std::vector<std::string>& args) {
    harkenizer::resourcecompiler::Request request;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--stream") {
            request.container = harkenizer::resourcecompiler::Container::Stream;
        } else if (arg == "--no-cache") {
            request.disableCache = true;
        } else if (arg == "--report" || arg == "--cache-path" ||
                   arg == "--emit-manifest") {
            if (i + 1 == args.size()) {
                return 1;
            }
            const auto& value = args[++i];
            if (arg == "--report") {
                request.reportPath = value;
            } else if (arg == "--cache-path") {
                request.cachePath = value;
            } else {
                request.emitManifestPath = value;
            }
        } else if (arg.size() >= 2 && arg[0] == '-' && arg[1] == '-') {
            return 1;
        } else {
            request.manifestPaths.push_back(arg);
        }
    }
    const auto result = harkenizer::resourcecompiler::compile(request);
    std::fwrite(result.outputLog.data(), 1, result.outputLog.size(), stdout);
    std::fwrite(result.diagnosticLog.data(), 1, result.diagnosticLog.size(), stderr);
    return result.success ? 0 : 1;
}

}
