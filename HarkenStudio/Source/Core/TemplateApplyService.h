
#pragma once

#include "ProjectTypes.h"
#include "ProjectTemplateLoader.h"

#include <QJsonObject>

namespace VNX {

class ProjectSerializer;

class TemplateApplyService {
public:
    static bool seedTemplateInto(ProjectData& project,
                                 const QJsonObject& templateData,
                                 QString* error = nullptr);
    static bool seedTemplateInto(ProjectData& project,
                                 const ProjectTemplateData& templateData,
                                 QString* error = nullptr);

private:
    static bool seedBusBranch(ProjectData& project,
                              ProjectItem* parentBus,
                              const QJsonObject& nodeJson,
                              ProjectSerializer& serializer,
                              QString* error);
};

}
