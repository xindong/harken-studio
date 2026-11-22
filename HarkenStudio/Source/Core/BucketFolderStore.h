#pragma once

#include "ProjectTypes.h"

namespace harken::studio::BucketFolderStore {

std::vector<ProjectFolderBinding> scanFolderTree(const QString& bucketRootDir,
                                                 const QString& bucketName);
QString folderRelPath(const PhysicalFolder* folder);
QString pathComparisonKey(const QString& path);

}
