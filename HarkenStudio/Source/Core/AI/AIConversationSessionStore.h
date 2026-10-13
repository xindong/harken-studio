#pragma once

#include <QList>
#include <QString>
#include <QtGlobal>

#include <functional>

namespace VNX {

struct AIConversationSessionRecord
{
    QString id;
    QString title;
    qint64 createdAtMs = 0;
    qint64 updatedAtMs = 0;
};

struct AIConversationSessionLimits
{
    static constexpr int kSchemaVersion = 1;
    static constexpr int kMaximumSessions = 64;
    static constexpr int kMaximumTitleBytes = 256;
    static constexpr int kMaximumDocumentBytes = 65536;
};

struct AIConversationSessionCatalog
{
    QString activeSessionId;
    QList<AIConversationSessionRecord> sessions;
};

struct AIConversationSessionLoadResult
{
    enum class Status
    {
        Missing,
        Loaded,
        Invalid,
        IoError
    };

    Status status = Status::Missing;
    AIConversationSessionCatalog catalog;
    QString code;
    QString message;
};

struct AIConversationSessionOperationResult
{
    bool success = false;
    QString code;
    QString message;
};

class IAIConversationSessionStore
{
public:
    using Mutation = std::function<AIConversationSessionOperationResult(
        const AIConversationSessionLoadResult& current,
        AIConversationSessionCatalog* replacement,
        bool* shouldPersist)>;

    virtual ~IAIConversationSessionStore() = default;
    virtual AIConversationSessionLoadResult load(
        const QString& scope) const = 0;
    virtual AIConversationSessionOperationResult mutate(
        const QString& scope,
        const Mutation& mutation) const = 0;
    virtual AIConversationSessionOperationResult save(
        const QString& scope,
        const AIConversationSessionCatalog& catalog) const = 0;
};

class LocalAIConversationSessionStore final
    : public IAIConversationSessionStore
{
public:
    LocalAIConversationSessionStore();
    explicit LocalAIConversationSessionStore(QString rootPath);

    AIConversationSessionLoadResult load(
        const QString& scope) const override;
    AIConversationSessionOperationResult mutate(
        const QString& scope,
        const Mutation& mutation) const override;
    AIConversationSessionOperationResult save(
        const QString& scope,
        const AIConversationSessionCatalog& catalog) const override;

    QString storagePathForScope(const QString& scope) const;
    static QString defaultRootPath();
    static bool normalizeCatalog(
        const AIConversationSessionCatalog& input,
        AIConversationSessionCatalog* output,
        QString* errorMessage = nullptr);

private:
    static QString scopeHash(const QString& scope);
    AIConversationSessionOperationResult saveUnlocked(
        const QString& scope,
        const QString& path,
        const AIConversationSessionCatalog& catalog) const;

    QString rootPath_;
};

}
