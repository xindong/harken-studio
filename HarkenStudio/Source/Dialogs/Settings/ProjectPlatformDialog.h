#pragma once

#include "Core/BaseDialog.h"
#include "Core/ProjectPlatform.h"

namespace VNX {

class ProjectPlatformDialog : public BaseDialog
{
public:
    ProjectPlatformDialog(const ProjectPlatform& platform, bool create, QWidget* parent = nullptr);
    ProjectPlatform platform() const { return platform_; }

private:
    ProjectPlatform platform_;
    ProjectPlatformProfile profile_;
};

}
