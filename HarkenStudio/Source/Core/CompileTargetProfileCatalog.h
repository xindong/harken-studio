#pragma once

#include <memory>

namespace harkenprojectschema {
struct CompileTargetProfileV1;
}

namespace VNX {

struct ProjectData;

class CompileTargetProfileCatalog final
{
public:
    static std::shared_ptr<const harkenprojectschema::CompileTargetProfileV1>
    resolve(const ProjectData& project) noexcept;
};

}
