#pragma once

#include <QByteArray>
#include <QLockFile>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace VNX {

using ProjectTransactionProgressCallback = std::function<void(
    int, int, qint64, qint64)>;

class ProjectTransaction
{
public:
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

    QString projectDirectory_;
    QString backupDirectory_;
    QString transactionPath_;
    QStringList writeSet_;
    QByteArray backupFingerprint_;
    QByteArray committedFingerprint_;
    std::unique_ptr<QLockFile> lock_;
    bool active_ = false;
};

}
