#pragma once

#include "ProjectDataPolicy.h"
#include "ProjectDocument.h"

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QScopeGuard>
#include <QSet>
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

    QJsonObject createDocument(const QString& filePath, const QString& type,
                               const QString& id, const QString& projectId,
                               const QJsonObject& content,
                               const QJsonObject& originalHeader = {})
    {
        const QString relative = ProjectDataPolicy::normalizeRelativePath(
            directory_.relativeFilePath(filePath));
        if (previous_) {
            const auto previous = previous_->generatedDocuments_.constFind(relative);
            if (previous != previous_->generatedDocuments_.cend()
                && previous->value(QStringLiteral("content")).toObject() == content
                && previous->value(QStringLiteral("harkenDocument")).toObject()
                    == ProjectDocument::createHeader(type, id, projectId, originalHeader)) {
                generatedDocuments_.insert(relative, *previous);
                return *previous;
            }
        }
        auto document = ProjectDocument::create(type, id, projectId, content, originalHeader);
        if (document.isEmpty()) generatedDocuments_.remove(relative);
        else generatedDocuments_.insert(relative, document);
        return document;
    }

    void prepareDirectoryLayout()
    {
        directoryEntries_.clear();
        if (QFileInfo(directory_.absolutePath()).isSymLink()) return;
        const auto capture = [&](const QString& path) {
            DirectoryEntries entries;
            entries.hidden = containsHiddenDirectory(path);
            const auto children = QDir(path).entryList(
                QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
            for (const QString& child : children) entries.names.insert(pathKey(child));
            directoryEntries_.insert(QDir::cleanPath(path), std::move(entries));
        };
        if (directory_.exists()) capture(directory_.absolutePath());
        QDirIterator directories(directory_.absolutePath(),
            QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
            QDirIterator::Subdirectories);
        while (directories.hasNext()) {
            directories.next();
            const QFileInfo directory = directories.fileInfo();
            if (!directory.isSymLink()) capture(directory.absoluteFilePath());
        }
    }

    WriteResult prepareDocument(const QString& filePath, const QJsonObject& document)
    {
        const QFileInfo target(filePath);
        const QString name = target.fileName();
#ifndef Q_OS_WIN
        if (name.startsWith(QLatin1Char('.'))) return WriteResult::Physical;
#endif
        bool regularName = !name.isEmpty();
#ifdef Q_OS_WIN
        regularName = regularName && !name.contains(QLatin1Char('~'))
            && !name.contains(QLatin1Char(':')) && !name.endsWith(QLatin1Char('.'))
            && name == name.trimmed();
#endif
        const auto prepared = directoryEntries_.constFind(QDir::cleanPath(target.absolutePath()));
        if (regularName && prepared != directoryEntries_.cend()
            && !prepared->names.contains(pathKey(name))) {
            if (prepared->hidden) return WriteResult::Physical;
            return addDocument(target, document, true, true)
                ? WriteResult::Stored : WriteResult::Failed;
        }
        if (target.exists() || containsHiddenDirectory(target.absolutePath()))
            return WriteResult::Physical;
        return addDocument(target, document, true) ? WriteResult::Stored : WriteResult::Failed;
    }

    bool containsPreparedDirectory(const QString& path) const
    {
        return directoryEntries_.contains(QDir::cleanPath(path));
    }

    bool addDocument(const QString& filePath, const QJsonObject& document)
    {
        return addDocument(QFileInfo(filePath), document, false);
    }

    void clearPrevious()
    {
        previous_ = nullptr;
        directoryEntries_.clear();
        hiddenDirectories_.clear();
        directoryPermissions_.clear();
    }
    const QMap<QString, QByteArray>& files() const { return files_; }
    const QHash<QString, QJsonObject>& documents() const { return documents_; }
    QFileDevice::Permissions permissions(const QString& relativePath) const
    {
        return permissions_.value(relativePath);
    }

private:
    static QString pathKey(const QString& path)
    {
        const QString cleaned = QDir::cleanPath(path);
#if defined(Q_OS_MACOS)
        return cleaned.toCaseFolded().normalized(QString::NormalizationForm_C);
#elif defined(Q_OS_WIN)
        return cleaned.toCaseFolded();
#else
        return cleaned;
#endif
    }

    bool addDocument(const QFileInfo& target, const QJsonObject& document, bool knownMissing,
                     bool preparedDirectory = false)
    {
        const QString relative = ProjectDataPolicy::normalizeRelativePath(
            directory_.relativeFilePath(target.filePath()));
        if (!ProjectDataPolicy::isSafeRelativePath(relative)
            || (!preparedDirectory && (!target.dir().exists()
                || (!knownMissing && target.isDir()) || target.isSymLink())))
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
    struct DirectoryEntries {
        QSet<QString> names;
        bool hidden = false;
    };
    QHash<QString, DirectoryEntries> directoryEntries_;
    const ProjectSaveSnapshot* previous_ = nullptr;
    QHash<QString, bool> hiddenDirectories_;
    QHash<QString, QFileDevice::Permissions> directoryPermissions_;
    QHash<QString, QFileDevice::Permissions> permissions_;
    QHash<QString, QJsonObject> generatedDocuments_;
    QHash<QString, QJsonObject> inputs_;
    QMap<QString, QByteArray> files_;
    QHash<QString, QJsonObject> documents_;
};

}
