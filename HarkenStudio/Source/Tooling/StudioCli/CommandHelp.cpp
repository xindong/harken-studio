#include "CommandHelp.h"

#include <QJsonDocument>
#include <QMap>
#include <QStringList>

namespace HarkenStudioCli {
namespace {

QString jsonValueText(const QJsonValue& value)
{
    const auto bytes = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(bytes.mid(1, bytes.size() - 2));
}

QString schemaSummary(const QJsonObject& schema)
{
    QStringList parts;
    const auto type = schema.value(QStringLiteral("type"));
    if (type.isString()) parts.append(type.toString());
    else if (type.isArray()) {
        QStringList types;
        for (const auto& value : type.toArray()) types.append(value.toString());
        parts.append(types.join(QStringLiteral(" | ")));
    }
    for (const auto& keyword : {QStringLiteral("anyOf"), QStringLiteral("oneOf")}) {
        QStringList options;
        for (const auto& value : schema.value(keyword).toArray())
            options.append(schemaSummary(value.toObject()));
        if (!options.isEmpty()) parts.append(options.join(QStringLiteral(" | ")));
    }
    if (parts.isEmpty()) parts.append(QStringLiteral("value"));
    if (schema.contains(QStringLiteral("items")))
        parts.append(QStringLiteral("of %1").arg(schemaSummary(schema.value(QStringLiteral("items")).toObject())));
    for (const auto& keyword : {QStringLiteral("enum"), QStringLiteral("const"),
            QStringLiteral("default"), QStringLiteral("format"), QStringLiteral("minimum"),
            QStringLiteral("maximum"), QStringLiteral("exclusiveMinimum"), QStringLiteral("exclusiveMaximum"),
            QStringLiteral("minLength"), QStringLiteral("maxLength"), QStringLiteral("pattern"),
            QStringLiteral("minItems"), QStringLiteral("maxItems"), QStringLiteral("uniqueItems"),
            QStringLiteral("minProperties"), QStringLiteral("maxProperties")})
        if (schema.contains(keyword)) parts.append(keyword + QLatin1Char('=') + jsonValueText(schema.value(keyword)));
    return parts.join(QStringLiteral(", "));
}

void appendProperties(QString& text, const QJsonObject& schema, const QString& prefix = {})
{
    const auto properties = schema.value(QStringLiteral("properties")).toObject();
    const auto required = schema.value(QStringLiteral("required")).toArray();
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        const auto field = it.value().toObject();
        const auto path = prefix + it.key();
        text += QStringLiteral("  %1  [%2]  %3\n").arg(path,
            required.contains(it.key()) ? QStringLiteral("required") : QStringLiteral("optional"),
            schemaSummary(field));
        if (!field.value(QStringLiteral("description")).toString().isEmpty())
            text += QStringLiteral("    ") + field.value(QStringLiteral("description")).toString() + QLatin1Char('\n');
        appendProperties(text, field, path + QLatin1Char('.'));
        appendProperties(text, field.value(QStringLiteral("items")).toObject(), path + QStringLiteral("[]."));
    }
}

QString conditionText(const QJsonObject& schema)
{
    QStringList conditions;
    QStringList required;
    for (const auto& field : schema.value(QStringLiteral("required")).toArray()) required.append(field.toString());
    if (!required.isEmpty()) conditions.append(QStringLiteral("provide %1").arg(required.join(QStringLiteral(" + "))));
    const auto properties = schema.value(QStringLiteral("properties")).toObject();
    for (auto it = properties.begin(); it != properties.end(); ++it)
        conditions.append(it.key() + QStringLiteral(": ") + schemaSummary(it.value().toObject()));
    if (schema.contains(QStringLiteral("not")))
        conditions.append(QStringLiteral("not (%1)").arg(conditionText(schema.value(QStringLiteral("not")).toObject())));
    for (const auto& keyword : {QStringLiteral("allOf"), QStringLiteral("anyOf"), QStringLiteral("oneOf")}) {
        QStringList branches;
        for (const auto& branch : schema.value(keyword).toArray()) branches.append(conditionText(branch.toObject()));
        if (!branches.isEmpty()) conditions.append(branches.join(keyword == QStringLiteral("allOf")
            ? QStringLiteral(" and ") : QStringLiteral(" or ")));
    }
    return conditions.join(QStringLiteral("; "));
}

void appendConstraints(QString& text, const QJsonObject& schema)
{
    for (const auto& keyword : {QStringLiteral("anyOf"), QStringLiteral("oneOf")}) {
        const auto branches = schema.value(keyword).toArray();
        if (branches.isEmpty()) continue;
        QStringList alternatives;
        bool onlyRequired = true;
        for (const auto& value : branches) {
            const auto branch = value.toObject();
            onlyRequired &= branch.size() == 1 && branch.contains(QStringLiteral("required"));
            QStringList required;
            for (const auto& field : branch.value(QStringLiteral("required")).toArray()) required.append(field.toString());
            alternatives.append(required.join(QStringLiteral(" + ")));
        }
        const auto quantifier = keyword == QStringLiteral("oneOf") ? QStringLiteral("exactly one") : QStringLiteral("at least one");
        if (onlyRequired) {
            text += QStringLiteral("  Provide %1 of: %2.\n").arg(quantifier, alternatives.join(QStringLiteral(" | ")));
        } else {
            text += QStringLiteral("  %1 condition must hold:\n").arg(quantifier);
            for (const auto& branch : branches)
                text += QStringLiteral("    - ") + conditionText(branch.toObject()) + QLatin1Char('\n');
        }
    }
    if (schema.contains(QStringLiteral("not"))) {
        const auto forbidden = schema.value(QStringLiteral("not")).toObject();
        if (forbidden.size() == 1 && forbidden.contains(QStringLiteral("required"))) {
            QStringList fields;
            for (const auto& field : forbidden.value(QStringLiteral("required")).toArray()) fields.append(field.toString());
            text += QStringLiteral("  Do not combine: ") + fields.join(QStringLiteral(" + ")) + QStringLiteral(".\n");
        } else {
            text += QStringLiteral("  Disallowed: ") + conditionText(forbidden) + QStringLiteral(".\n");
        }
    }
    for (const auto& branch : schema.value(QStringLiteral("allOf")).toArray()) appendConstraints(text, branch.toObject());
}

}

QString commandDescriptionText(const QJsonObject& entry, bool shell)
{
    const auto name = entry.value(QStringLiteral("command")).toString();
    QString text = name + QStringLiteral(" - ") + entry.value(QStringLiteral("description")).toString() + QLatin1Char('\n');
    text += QStringLiteral("Category: %1 | %2\n\n").arg(entry.value(QStringLiteral("category")).toString(),
        entry.value(QStringLiteral("readOnly")).toBool() ? QStringLiteral("read only") : QStringLiteral("changes state"));
    const auto schema = entry.value(QStringLiteral("inputSchema")).toObject();
    const bool hasArguments = !schema.value(QStringLiteral("properties")).toObject().isEmpty();
    if (!hasArguments) {
        text += QStringLiteral("Arguments: none.\n");
    } else {
        text += QStringLiteral("Arguments (JSON object):\n");
        appendProperties(text, schema);
    }
    const auto propertyValues = schema.value(QStringLiteral("propertyValues")).toObject();
    if (!propertyValues.isEmpty()) {
        text += QStringLiteral("\nValue types by property:\n");
        for (auto it = propertyValues.begin(); it != propertyValues.end(); ++it) {
            const auto field = it.value().toObject();
            text += QStringLiteral("  %1: %2\n").arg(it.key(), schemaSummary(field));
            if (!field.value(QStringLiteral("description")).toString().isEmpty())
                text += QStringLiteral("    ") + field.value(QStringLiteral("description")).toString() + QLatin1Char('\n');
            appendProperties(text, field.value(QStringLiteral("items")).toObject(), QStringLiteral("value[]."));
        }
    }
    appendConstraints(text, schema);
    if (shell) {
        text += QStringLiteral("\nRun: %1%2\n").arg(name, hasArguments ? QStringLiteral(" { ... }") : QString());
        text += QStringLiteral("Enter the command directly in this shell; arguments are one JSON object.\n");
    } else {
        text += QStringLiteral("\nRun: HarkenStudioCLI exec %1").arg(name)
            + (hasArguments ? QStringLiteral(" --input arguments.json") : QString())
            + QStringLiteral(" [--project <path> | --instance <id>] [--save]\n")
            + QStringLiteral("Use --input - to read UTF-8 JSON from stdin; --json returns the full command contract.\n");
    }
    return text;
}

QString commandDirectoryText(const QJsonArray& entries, bool shell)
{
    QMap<QString, QMap<QString, QJsonObject>> categories;
    for (const auto& value : entries) {
        const auto entry = value.toObject();
        categories[entry.value(QStringLiteral("category")).toString()]
            .insert(entry.value(QStringLiteral("command")).toString(), entry);
    }
    QString text;
    if (entries.isEmpty()) text = QStringLiteral("No commands match these filters.\n");
    for (auto category = categories.cbegin(); category != categories.cend(); ++category) {
        if (!text.isEmpty()) text += QLatin1Char('\n');
        text += category.key() + QLatin1Char('\n');
        for (const auto& entry : category.value())
            text += QStringLiteral("  %1  [%2]  %3\n").arg(entry.value(QStringLiteral("command")).toString(),
                entry.value(QStringLiteral("readOnly")).toBool() ? QStringLiteral("read only") : QStringLiteral("changes state"),
                entry.value(QStringLiteral("description")).toString());
    }
    text += QStringLiteral("\n%1 commands. Use %2 for parameters.\n").arg(entries.size()).arg(
        shell ? QStringLiteral("help <name>") : QStringLiteral("HarkenStudioCLI commands <name>"));
    text += shell
        ? QStringLiteral("Filter: commands --search <text> | --category <name> | --read-only\n")
        : QStringLiteral("Filter: HarkenStudioCLI commands --search <text> | --category <name> | --read-only\n");
    return text;
}

QJsonObject filterCommandDirectory(QJsonObject response, const QString& search,
                                   const QString& category, bool readOnly)
{
    if (response.value(QStringLiteral("status")) != QStringLiteral("ok")) return response;
    auto result = response.value(QStringLiteral("result")).toObject();
    QJsonArray entries;
    for (const auto& value : result.value(QStringLiteral("commands")).toArray()) {
        const auto entry = value.toObject();
        const auto entryCategory = entry.value(QStringLiteral("category")).toString();
        const auto searchable = entry.value(QStringLiteral("command")).toString() + QLatin1Char(' ')
            + entry.value(QStringLiteral("description")).toString() + QLatin1Char(' ') + entryCategory;
        if (!search.isEmpty() && !searchable.contains(search, Qt::CaseInsensitive)) continue;
        if (!category.isEmpty() && entryCategory.compare(category, Qt::CaseInsensitive) != 0) continue;
        if (readOnly && !entry.value(QStringLiteral("readOnly")).toBool()) continue;
        entries.append(entry);
    }
    result.insert(QStringLiteral("commands"), entries);
    response.insert(QStringLiteral("result"), result);
    return response;
}

}
