#pragma once

#include <QByteArray>
#include <QFileDevice>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QLockFile>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include <functional>
#include <memory>

namespace VNX {

class ProjectSaveSnapshot;

using ProjectTransactionProgressCallback = std::function<void(
    int, int, qint64, qint64)>;

class ProjectTransaction
{
public:
    struct FileWrite {
        QString path;
        QByteArray bytes;
        QFileDevice::Permissions permissions{};
    };
    struct FileRename {
        QString source;
        QString target;
    };
    struct DirectoryChange {
        QString path;
        bool present = true;
    };
    struct ReadDependency {
        QString path;
        QByteArray hash;
        bool exists = true;
    };
    struct DeltaPlan {
        QList<FileWrite> writes;
        QStringList deletes;
        QList<FileRename> renames;
        QList<DirectoryChange> directories;
        QList<ReadDependency> readSet;
    };

    static std::unique_ptr<ProjectTransaction> beginDelta(
        const QString& projectFilePath,
        const DeltaPlan& plan,
        QString* error = nullptr);
    static bool verifyReadSet(const QString& projectFilePath,
                              const QList<ReadDependency>& readSet,
                              QString* error = nullptr);
    static std::unique_ptr<ProjectTransaction> begin(
        const QString& projectFilePath,
        QString* error = nullptr);
    static std::unique_ptr<ProjectTransaction> begin(
        const QString& projectFilePath,
        const QStringList& writeSet,
        QString* error = nullptr);
    static std::unique_ptr<ProjectTransaction> begin(
        const QString& projectFilePath,
        const QStringList& writeSet,
        const QString& recoveryRoot,
        QString* error = nullptr,
        ProjectTransactionProgressCallback progress = {});
    static std::unique_ptr<ProjectTransaction> beginReplacement(
        const QString& projectFilePath,
        const QStringList& writeSet,
        const QString& stagedDirectory,
        QString* error = nullptr);
    static std::unique_ptr<ProjectTransaction> beginReplacement(
        const QString& projectFilePath,
        const QStringList& writeSet,
        const QString& stagedDirectory,
        const ProjectSaveSnapshot& snapshot,
        QString* error = nullptr);
    static bool recover(const QString& projectFilePath,
                        QString* error = nullptr,
                        ProjectTransactionProgressCallback progress = {});
    static bool recoveryRequired(const QString& projectFilePath);
    static bool rollbackJournal(const QString& transactionPath, QString* error = nullptr);
    static bool finalizeJournal(const QString& transactionPath,
                                const QString& receiptPath,
                                QString* error = nullptr);
    static bool discardReceipt(const QString& receiptPath, QString* error = nullptr);

    ~ProjectTransaction();

    const QString& projectDirectory() const { return projectDirectory_; }
    const QString& backupDirectory() const { return backupDirectory_; }
    const QString& transactionPath() const { return transactionPath_; }
    const QByteArray& backupFingerprint() const { return backupFingerprint_; }
    const QByteArray& committedFingerprint() const { return committedFingerprint_; }
    const QStringList& writeSet() const { return writeSet_; }
    QStringList changedPaths() const;

    bool applyDelta(QString* error = nullptr,
                    ProjectTransactionProgressCallback progress = {});
    bool replaceFromDirectory(const QString& stagedDirectory,
                              QString* error = nullptr,
                              ProjectTransactionProgressCallback progress = {});
    bool replaceFromDirectory(const QString& stagedDirectory,
                              const QStringList& directoryRoots,
                              QString* error = nullptr,
                              ProjectTransactionProgressCallback progress = {});
    bool commit(bool keepJournal = false, QString* error = nullptr);
    bool finalizeToReceipt(const QString& receiptPath, QString* error = nullptr);
    bool rollback(QString* error = nullptr,
                  ProjectTransactionProgressCallback progress = {});

private:
    ProjectTransaction() = default;
    static std::unique_ptr<ProjectTransaction> beginImpl(
        const QString& projectFilePath,
        const QStringList& writeSet,
        const QString& recoveryRoot,
        const QString& stagedDirectory,
        const ProjectSaveSnapshot* snapshot,
        QString* error,
        ProjectTransactionProgressCallback progress,
        const DeltaPlan* delta = nullptr);
    bool install(QJsonObject journal, const QString& stagedDirectory,
                 QString* error, ProjectTransactionProgressCallback progress);
    bool verifyReadSet(QString* error) const;

    struct StagedFile {
        QByteArray bytes;
        QByteArray hash;
        QFileDevice::Permissions permissions;
    };

    QString projectDirectory_;
    QString backupDirectory_;
    QString transactionPath_;
    QStringList writeSet_;
    QHash<QString, StagedFile> stagedFiles_;
    QHash<QString, QByteArray> retainedFiles_;
    QHash<QString, QString> renamedSources_;
    QList<ReadDependency> readSet_;
    QJsonArray deltaDirectories_;
    QByteArray backupFingerprint_;
    QByteArray committedFingerprint_;
    std::unique_ptr<QLockFile> lock_;
    QThreadPool* verificationPool_ = nullptr;
    bool active_ = false;
    bool delta_ = false;
    bool parallelFileOperations_ = false;
};

}
