#pragma once

#include "Core/ProjectTypes.h"

#include <vector>

namespace VNX {

class ProjectSerializer;

namespace ProjectLoaderInternal {

struct SingleObjHierarchySpec
{
    QString folderName;
    ProjectItem::Type topType;
};

QString actionsRootDir(const QString& projectRoot);
const std::vector<SingleObjHierarchySpec>& s3Specs();
QString s3RootDir(const QString& projectRoot, const SingleObjHierarchySpec& spec);
QString s3DocumentType(const SingleObjHierarchySpec& spec);
QString busFolderAbs(const QString& projectRoot);
QString layersFolderAbs(const QString& projectRoot);
bool isLayersBucketRootType(ProjectItem::Type type);
bool loadActions(const QString& projectRoot, ProjectData& project,
                 ProjectSerializer& serializer, QString* errOut = nullptr);
bool loadS3Hierarchy(const QString& projectRoot, const SingleObjHierarchySpec& spec,
                     ProjectData& project, ProjectSerializer& serializer,
                     QString* errOut = nullptr);
bool loadBussesPacksImpl(const QString& projectRoot, ProjectData& project,
                        ProjectSerializer& serializer, QString* errOut);
bool loadLayersPacksImpl(const QString& projectRoot, ProjectData& project,
                        ProjectSerializer& serializer, QString* errOut,
                        int* failedOut = nullptr);

}

}
