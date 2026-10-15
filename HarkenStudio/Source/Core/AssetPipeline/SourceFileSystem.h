#pragma once

#include <QString>
#include <QStringList>
#include <QMap>
#include <QList>
#include <cstdint>
#include <memory>

namespace VNX { class TransactionalUndoCommand; struct AssetInfo; }

namespace VNX::SourceFileSystem {

QString absolutePath(const QString& relativePath);
bool validName(const QString& name);
bool createFolder(const QString& parentPath, const QString& name, QString* error = nullptr,
                  QString* createdName = nullptr);
bool rename(const QString& path, const QString& name, QString* error = nullptr,
            QString* renamedName = nullptr);
bool restoreByRename(uint32_t sourceID, uint32_t missingID, QString* error = nullptr);
bool validateRename(const QString& path, const QString& name, QString* error = nullptr);
bool canMove(const QStringList& paths, const QString& parentPath);
bool move(const QStringList& paths, const QString& parentPath, QString* error = nullptr);
bool removeEmptyFolder(const QString& path, QString* error = nullptr);
std::unique_ptr<TransactionalUndoCommand> replaceFiles(
    const QMap<QString, QString>& replacements, QString* error = nullptr);
std::unique_ptr<TransactionalUndoCommand> registerImport(
    const AssetInfo& asset, bool copiedFile, QString* error = nullptr);
bool replayFiles(const QList<std::shared_ptr<TransactionalUndoCommand>>& commands,
                 bool redo, bool* rollbackFailed);
bool copyFile(const QString& source, const QString& destination);
void commitChanges();
void discardChanges();

}
