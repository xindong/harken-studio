#ifndef HARKEN_PROJECT_SCHEMA_RESOURCE_INPUT_H
#define HARKEN_PROJECT_SCHEMA_RESOURCE_INPUT_H

#include "harken_project_schema/Model.h"

#include <map>

namespace harkenprojectschema {

struct ResourceFacts {
    bool valid = false;
    std::uint32_t sampleRate = 0;
    std::uint32_t channelCount = 0;
    std::uint64_t frameCount = 0;
    std::uint64_t contentRevision = 0;
};

struct ResourceInput {
    ResourceFacts facts;
    bool seekable = false;
};

// The resources a build is given, keyed by the stable id of the Sound they
// describe. A clip whose resolved Sound target is absent here is a missing
// input, which the build refuses. The caller resolves authoring identity to the
// stable id (through the compiled project) before keying this table.
using ResourceTable = std::map<StableId, ResourceInput>;

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_RESOURCE_INPUT_H
