#pragma once

#include <QJsonArray>
#include <QString>

namespace VNX {

struct AIConversationLimits
{
    static constexpr int kSchemaVersion = 1;
    static constexpr int kMaximumHistoryBytes = 65536;
    static constexpr int kMaximumMessageBytes = 24576;
    static constexpr int kMaximumDocumentBytes = 98304;
    static constexpr int kMaximumMessages = 256;
};

class IAIConversationStore
{
public:
    struct LoadResult
    {
        enum class Status { Missing, Loaded, Invalid, IoError };

        Status status = Status::Missing;
        QJsonArray messages;
        QString code;
        QString message;
    };

    struct OperationResult
    {
        bool success = false;
        QString code;
        QString message;
    };

    virtual ~IAIConversationStore() = default;
    virtual LoadResult load(const QString& scope) const = 0;
    virtual OperationResult save(const QString& scope,
                                 const QJsonArray& messages) const = 0;
    virtual OperationResult clear(const QString& scope) const = 0;
};

class LocalAIConversationStore final : public IAIConversationStore
{
public:
    LocalAIConversationStore();
    explicit LocalAIConversationStore(QString rootPath);

    LoadResult load(const QString& scope) const override;
    OperationResult save(const QString& scope,
                         const QJsonArray& messages) const override;
    OperationResult clear(const QString& scope) const override;

    QString storagePathForScope(const QString& scope) const;
    static QString defaultRootPath();
    static bool normalizeMessages(const QJsonArray& input,
                                  QJsonArray* output,
                                  QString* errorMessage = nullptr);

private:
    static QString scopeHash(const QString& scope);
    static OperationResult success();
    static OperationResult failure(const QString& code,
                                   const QString& message);

    QString rootPath_;
};

}
