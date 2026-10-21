#pragma once

#include "Core/BaseDialog.h"
#include "Core/ProjectPlatform.h"

namespace VNX {

class ProjectPlatformDialog : public BaseDialog
{
public:
    explicit ProjectPlatformDialog(QWidget* parent = nullptr);
    ProjectPlatform platform() const { return platform_; }

private:
    ProjectPlatform platform_;
};

}
