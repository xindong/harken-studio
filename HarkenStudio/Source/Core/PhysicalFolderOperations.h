#pragma once

#include <QString>

#include <memory>
#include <vector>

namespace VNX {

struct PhysicalFolder;
struct ProjectItem;
class TransactionalUndoCommand;

struct PhysicalItemMove {
    ProjectItem* item = nullptr;
    PhysicalFolder* destination = nullptr;
};

struct PhysicalFolderMove {
    PhysicalFolder* folder = nullptr;
    PhysicalFolder* destination = nullptr;
    QString bucket;
};

bool createPhysicalFolder(PhysicalFolder* parent,
                          const std::shared_ptr<PhysicalFolder>& folder,
                          const QString& bucket);
bool renamePhysicalFolder(PhysicalFolder* folder, const QString& name);
bool executePhysicalMoves(const std::vector<PhysicalItemMove>& items,
                          const std::vector<PhysicalFolderMove>& folders,
                          std::unique_ptr<TransactionalUndoCommand> business = {});

}
