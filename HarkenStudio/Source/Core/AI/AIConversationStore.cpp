#include "AIConversationStore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <utility>

namespace VNX {
namespace {

constexpr auto kInvalidCode = "AI_MEMORY_INVALID";
constexpr auto kIoCode = "AI_MEMORY_IO_ERROR";

IAIConversationStore::LoadResult invalidLoad(const QString& message)
{
    IAIConversationStore::LoadResult result;
    result.status = IAIConversationStore::LoadResult::Status::Invalid;
    result.code = QString::fromLatin1(kInvalidCode);
    result.message = message;
    return result;
}

IAIConversationStore::LoadResult ioLoad(const QString& message)
{
    IAIConversationStore::LoadResult result;
    result.status = IAIConversationStore::LoadResult::Status::IoError;
    result.code = QString::fromLatin1(kIoCode);
    result.message = message;
    return result;
}

}

LocalAIConversationStore::LocalAIConversationStore()
    : LocalAIConversationStore(defaultRootPath())
{
}

LocalAIConversationStore::LocalAIConversationStore(QString rootPath)
    : rootPath_(rootPath.trimmed().isEmpty()
                    ? QString()
                    : QDir::cleanPath(std::move(rootPath)))
{
}

QString LocalAIConversationStore::defaultRootPath()
{
    const QString appData = QStandardPaths::writableLocation(
        QStandardPaths::AppDataLocation);
    if (appData.isEmpty())
        return {};
    return QDir(appData).filePath(QStringLiteral("AI/Conversations"));
}

QString LocalAIConversationStore::scopeHash(const QString& scope)
{
    const QByteArray digest = QCryptographicHash::hash(
        scope.trimmed().toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(digest.toHex());
}

QString LocalAIConversationStore::storagePathForScope(
    const QString& scope) const
{
    if (rootPath_.isEmpty() || scope.trimmed().isEmpty())
        return {};
    return QDir(rootPath_).filePath(
        scopeHash(scope) + QStringLiteral(".json"));
}

bool LocalAIConversationStore::normalizeMessages(
    const QJsonArray& input, QJsonArray* output, QString* errorMessage)
{
    auto fail = [errorMessage](const QString& message) {
        if (errorMessage)
            *errorMessage = message;
        return false;
    };
    if (!output)
        return fail(QStringLiteral("Conversation output is unavailable."));
    if (input.size() % 2 != 0)
        return fail(QStringLiteral("Conversation messages must form complete turns."));
    if (input.size() > AIConversationLimits::kMaximumMessages)
        return fail(QStringLiteral("Conversation contains too many messages."));
    if (QJsonDocument(input).toJson(QJsonDocument::Compact).size()
        > AIConversationLimits::kMaximumHistoryBytes)
    {
        return fail(QStringLiteral("Conversation history exceeds the storage limit."));
    }

    QJsonArray normalized;
    for (int index = 0; index < input.size(); ++index)
    {
        if (!input[index].isObject())
            return fail(QStringLiteral("Conversation contains a malformed message."));
        const QJsonObject message = input[index].toObject();
        const QJsonValue roleValue = message.value(QStringLiteral("role"));
        const QJsonValue contentValue = message.value(QStringLiteral("content"));
        if (!roleValue.isString() || !contentValue.isString())
            return fail(QStringLiteral("Conversation message fields are invalid."));

        const QString expectedRole =
            index % 2 == 0 ? QStringLiteral("user")
                           : QStringLiteral("assistant");
        if (roleValue.toString() != expectedRole)
            return fail(QStringLiteral("Conversation message roles are invalid."));
        const QString content = contentValue.toString();
        if (content.toUtf8().size()
            > AIConversationLimits::kMaximumMessageBytes)
            return fail(QStringLiteral("Conversation message exceeds the storage limit."));

        normalized.append(QJsonObject{
            {QStringLiteral("role"), expectedRole},
            {QStringLiteral("content"), content}
        });
    }
    *output = normalized;
    if (errorMessage)
        errorMessage->clear();
    return true;
}

IAIConversationStore::LoadResult LocalAIConversationStore::load(
    const QString& scope) const
{
    const QString path = storagePathForScope(scope);
    if (path.isEmpty())
        return invalidLoad(QStringLiteral("Conversation storage scope is invalid."));

    QFile file(path);
    if (!file.exists())
        return {};
    if (file.size() > AIConversationLimits::kMaximumDocumentBytes)
        return invalidLoad(QStringLiteral("Stored conversation exceeds the file limit."));
    if (!file.open(QIODevice::ReadOnly))
        return ioLoad(QStringLiteral("Stored conversation could not be opened."));

    const QByteArray bytes =
        file.read(AIConversationLimits::kMaximumDocumentBytes + 1);
    if (bytes.size() > AIConversationLimits::kMaximumDocumentBytes)
        return invalidLoad(QStringLiteral("Stored conversation exceeds the file limit."));

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return invalidLoad(QStringLiteral("Stored conversation is not valid JSON."));

    const QJsonObject root = document.object();
    const QJsonValue schemaVersion =
        root.value(QStringLiteral("schema_version"));
    if (!schemaVersion.isDouble()
        || schemaVersion.toDouble()
            != static_cast<double>(AIConversationLimits::kSchemaVersion))
    {
        return invalidLoad(
            QStringLiteral("Stored conversation schema is unsupported."));
    }
    if (root.value(QStringLiteral("scope_id")).toString()
        != scopeHash(scope))
    {
        return invalidLoad(QStringLiteral("Stored conversation scope is invalid."));
    }
    if (!root.value(QStringLiteral("messages")).isArray())
        return invalidLoad(QStringLiteral("Stored conversation messages are invalid."));

    QJsonArray normalized;
    QString validationError;
    if (!normalizeMessages(root.value(QStringLiteral("messages")).toArray(),
                           &normalized, &validationError))
    {
        return invalidLoad(validationError);
    }

    LoadResult result;
    result.status = LoadResult::Status::Loaded;
    result.messages = normalized;
    return result;
}

IAIConversationStore::OperationResult LocalAIConversationStore::save(
    const QString& scope, const QJsonArray& messages) const
{
    const QString path = storagePathForScope(scope);
    if (path.isEmpty())
        return failure(QString::fromLatin1(kInvalidCode),
                       QStringLiteral("Conversation storage scope is invalid."));

    QJsonArray normalized;
    QString validationError;
    if (!normalizeMessages(messages, &normalized, &validationError))
        return failure(QString::fromLatin1(kInvalidCode), validationError);

    const QJsonObject root{
        {QStringLiteral("schema_version"),
         AIConversationLimits::kSchemaVersion},
        {QStringLiteral("scope_id"), scopeHash(scope)},
        {QStringLiteral("messages"), normalized}
    };
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (bytes.size() > AIConversationLimits::kMaximumDocumentBytes)
    {
        return failure(QString::fromLatin1(kInvalidCode),
                       QStringLiteral("Conversation document exceeds the file limit."));
    }

    if (!QDir().mkpath(rootPath_))
    {
        return failure(QString::fromLatin1(kIoCode),
                       QStringLiteral("Conversation storage directory could not be created."));
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
    {
        return failure(QString::fromLatin1(kIoCode),
                       QStringLiteral("Conversation storage could not be opened."));
    }
    if (file.write(bytes) != bytes.size() || !file.commit())
    {
        return failure(QString::fromLatin1(kIoCode),
                       QStringLiteral("Conversation storage could not be committed."));
    }
    return success();
}

IAIConversationStore::OperationResult LocalAIConversationStore::clear(
    const QString& scope) const
{
    const QString path = storagePathForScope(scope);
    if (path.isEmpty())
        return failure(QString::fromLatin1(kInvalidCode),
                       QStringLiteral("Conversation storage scope is invalid."));
    if (!QFile::exists(path) || QFile::remove(path))
        return success();
    return failure(QString::fromLatin1(kIoCode),
                   QStringLiteral("Stored conversation could not be removed."));
}

IAIConversationStore::OperationResult LocalAIConversationStore::success()
{
    OperationResult result;
    result.success = true;
    return result;
}

IAIConversationStore::OperationResult LocalAIConversationStore::failure(
    const QString& code, const QString& message)
{
    OperationResult result;
    result.code = code;
    result.message = message;
    return result;
}

}
