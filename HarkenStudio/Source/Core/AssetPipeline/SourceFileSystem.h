#pragma once

#include <QString>
#include <QStringList>

namespace VNX::SourceFileSystem {

QString absolutePath(const QString& relativePath);
bool validName(const QString& name);
bool createFolder(const QString& parentPath, const QString& name, QString* error = nullptr,
                  QString* createdName = nullptr);
bool rename(const QString& path, const QString& name, QString* error = nullptr,
            QString* renamedName = nullptr);
bool validateRename(const QString& path, const QString& name, QString* error = nullptr);
bool canMove(const QStringList& paths, const QString& parentPath);
bool move(const QStringList& paths, const QString& parentPath, QString* error = nullptr);
bool removeEmptyFolder(const QString& path, QString* error = nullptr);
void commitChanges();
void discardChanges();

}
