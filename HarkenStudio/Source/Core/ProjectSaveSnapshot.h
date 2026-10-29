#pragma once

#include "ProjectDataPolicy.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QScopeGuard>
#include <QString>
#include <QUuid>
#include <utility>

namespace VNX {

class ProjectSaveSnapshot
{
public:
    enum class WriteResult { Stored, Physical, Failed };

    explicit ProjectSaveSnapshot(const QString& projectDirectory,
                                  const ProjectSaveSnapshot* previous = nullptr)
        : directory_(projectDirectory), previous_(previous)
    {
    }

    WriteResult prepareDocument(const QString& filePath, const QJsonObject& document)
    {
        const QFileInfo target(filePath);
#ifndef Q_OS_WIN
        if (target.fileName().startsWith(QLatin1Char('.'))) return WriteResult::Physical;
#endif
        if (target.exists() || containsHiddenDirectory(target.absolutePath()))
            return WriteResult::Physical;
        return addDocument(target, document, true) ? WriteResult::Stored : WriteResult::Failed;
    }

    bool addDocument(const QString& filePath, const QJsonObject& document)
    {
        return addDocument(QFileInfo(filePath), document, false);
    }

    void clearPrevious() { previous_ = nullptr; }
    const QMap<QString, QByteArray>& files() const { return files_; }
    const QHash<QString, QJsonObject>& documents() const { return documents_; }
    QFileDevice::Permissions permissions(const QString& relativePath) const
    {
        return permissions_.value(relativePath);
    }

private:
    bool addDocument(const QFileInfo& target, const QJsonObject& document, bool knownMissing)
    {
        const QString relative = ProjectDataPolicy::normalizeRelativePath(
            directory_.relativeFilePath(target.filePath()));
        if (!ProjectDataPolicy::isSafeRelativePath(relative)
            || !target.dir().exists() || (!knownMissing && target.isDir()) || target.isSymLink())
            return false;
#ifndef Q_OS_WIN
        const QString parent = target.absolutePath();
        if (!directoryPermissions_.contains(parent)) {
            QFile witness(QDir(parent).filePath(QStringLiteral(".harken-save-permissions-")
                + QUuid::createUuid().toString(QUuid::WithoutBraces)));
            if (!witness.open(QIODevice::WriteOnly | QIODevice::NewOnly)) return false;
            auto cleanup = qScopeGuard([&] { witness.remove(); });
            const auto permissions = witness.permissions();
            if (!witness.remove()) return false;
            cleanup.dismiss();
            directoryPermissions_.insert(parent, permissions);
        }
#endif
        const auto previous = previous_ ? previous_->inputs_.constFind(relative)
                                        : inputs_.cend();
        if (previous_ && previous != previous_->inputs_.cend() && previous.value() == document) {
            inputs_.insert(relative, previous.value());
            files_.insert(relative, previous_->files_.value(relative));
            documents_.insert(relative, previous_->documents_.value(relative));
        } else {
            QByteArray bytes = QJsonDocument(document).toJson(QJsonDocument::Indented);
#ifdef Q_OS_WIN
            bytes.replace("\n", "\r\n");
#endif
            QJsonParseError error;
            const QJsonDocument parsed = QJsonDocument::fromJson(bytes, &error);
            if (error.error != QJsonParseError::NoError || !parsed.isObject()) return false;
            inputs_.insert(relative, document);
            files_.insert(relative, std::move(bytes));
            documents_.insert(relative, parsed.object());
        }
#ifndef Q_OS_WIN
        permissions_.insert(relative, directoryPermissions_.value(parent));
#endif
        return true;
    }

    bool containsHiddenDirectory(const QString& path)
    {
        if (path == directory_.absolutePath()) return false;
        const auto known = hiddenDirectories_.constFind(path);
        if (known != hiddenDirectories_.cend()) return known.value();
        const QFileInfo directory(path);
        const QString parent = directory.absolutePath();
        const bool hidden = directory.isHidden()
            || (parent != path && containsHiddenDirectory(parent));
        hiddenDirectories_.insert(path, hidden);
        return hidden;
    }

    QDir directory_;
    const ProjectSaveSnapshot* previous_ = nullptr;
    QHash<QString, bool> hiddenDirectories_;
    QHash<QString, QFileDevice::Permissions> directoryPermissions_;
    QHash<QString, QFileDevice::Permissions> permissions_;
    QHash<QString, QJsonObject> inputs_;
    QMap<QString, QByteArray> files_;
    QHash<QString, QJsonObject> documents_;
};

}
