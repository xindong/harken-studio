#pragma once

#include <memory>

namespace harken::studio::project_schema {
struct CompileTargetProfileV1;
}

namespace harken::studio {

struct ProjectData;

class CompileTargetProfileCatalog final
{
public:
    static std::shared_ptr<const harken::studio::project_schema::CompileTargetProfileV1>
    resolve(const ProjectData& project) noexcept;
};

}
