
#include "HelpCatalog.h"

#include "AppSettings.h"
#include "ComponentCatalog.h"
#include "ItemTypeCatalog.h"

#include <QDebug>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <utility>

namespace VNX {
namespace HelpCatalog {

namespace {

constexpr int kSchemaVersion = 1;

const QString kTypePrefix = QStringLiteral("types.");
const QString kFieldPrefix = QStringLiteral("fields.");
const QString kPanelPrefix = QStringLiteral("panels.");
const QString kOwnerlessField = QStringLiteral("item");

void loadSection(const QJsonObject& root,
                 const QString& section,
                 const QString& prefix,
                 QHash<QString, HelpEntry>& out)
{
    const QJsonObject entries = root.value(section).toObject();
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it)
    {
        const QJsonObject entry = it.value().toObject();
        HelpEntry loaded{entry.value(QStringLiteral("title")).toString(),
                         entry.value(QStringLiteral("body")).toString(),
                         {}};
        const QJsonObject byType = entry.value(QStringLiteral("byType")).toObject();
        for (auto type = byType.constBegin(); type != byType.constEnd(); ++type)
            loaded.byType.insert(type.key(), type.value().toString());
        out.insert(prefix + it.key(), std::move(loaded));
    }
}

QHash<QString, HelpEntry> loadCatalog(const QString& language)
{
    QHash<QString, HelpEntry> entries;

    const QString path = QStringLiteral(":/Resources/help/help.%1.json").arg(language);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        qWarning() << "HelpCatalog:" << path << "is missing from the resource bundle";
        return entries;
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError)
    {
        qWarning() << "HelpCatalog:" << path << "is malformed:" << error.errorString();
        return entries;
    }

    const QJsonObject root = document.object();
    const int schema = root.value(QStringLiteral("schema")).toInt();
    if (schema != kSchemaVersion)
    {
        qWarning() << "HelpCatalog: unsupported schema" << schema
                   << "- expected" << kSchemaVersion;
        return entries;
    }

    loadSection(root, QStringLiteral("types"), kTypePrefix, entries);
    loadSection(root, QStringLiteral("fields"), kFieldPrefix, entries);
    loadSection(root, QStringLiteral("panels"), kPanelPrefix, entries);
    return entries;
}

const QHash<QString, HelpEntry>& catalog()
{
    static QHash<QString, HelpEntry> entries;
    static QString loadedLanguage;

    const QString language = AppSettings::instance().language();
    if (language != loadedLanguage)
    {
        entries = loadCatalog(language);
        if (entries.isEmpty() && language != QStringLiteral("en"))
            entries = loadCatalog(QStringLiteral("en"));
        loadedLanguage = language;
    }
    return entries;
}

}

QString keyFor(const ContextTopic& topic)
{
    switch (topic.kind)
    {
        case ContextTopicKind::Folder:
            return kTypePrefix + QStringLiteral("Folder");
        case ContextTopicKind::Object:
            return kTypePrefix + ItemTypeCatalog::canonicalName(topic.objectType);
        case ContextTopicKind::Field:
            return kFieldPrefix
                   + (topic.ownerComponent
                          ? ComponentCatalog::componentName(*topic.ownerComponent)
                          : kOwnerlessField)
                   + QStringLiteral(".") + topic.field;
        case ContextTopicKind::Panel:
            return kPanelPrefix + topic.panelKey;
        case ContextTopicKind::None:
            break;
    }
    return {};
}

const HelpEntry* entryFor(const ContextTopic& topic)
{
    const QString key = keyFor(topic);
    if (key.isEmpty())
    {
        return nullptr;
    }

    const QHash<QString, HelpEntry>& entries = catalog();
    const auto it = entries.constFind(key);
    return it == entries.constEnd() ? nullptr : &it.value();
}

QString bodyFor(const ContextTopic& topic)
{
    const HelpEntry* entry = entryFor(topic);
    if (!entry)
    {
        return {};
    }

    QString body = entry->body;
    const auto it = entry->byType.constFind(ItemTypeCatalog::canonicalName(topic.objectType));
    if (it != entry->byType.constEnd() && !it.value().isEmpty())
        body += QStringLiteral("\n\n") + it.value();
    for (const QString& argument : topic.helpArguments)
        body = body.arg(argument);
    return body;
}

}
}
