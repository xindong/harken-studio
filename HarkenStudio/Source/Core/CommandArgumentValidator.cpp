#include "CommandArgumentValidator.h"

#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <cmath>

namespace VNX {
namespace {

QString memberPath(const QString& path, const QString& key)
{
    return path + QStringLiteral(".") + key;
}

bool matchesType(const QJsonValue& value, const QString& type)
{
    if (type == "object") return value.isObject();
    if (type == "array") return value.isArray();
    if (type == "string") return value.isString();
    if (type == "boolean") return value.isBool();
    if (type == "null") return value.isNull();
    if (type == "number") return value.isDouble() && std::isfinite(value.toDouble());
    if (type == "integer") return value.isDouble() && std::isfinite(value.toDouble())
        && std::floor(value.toDouble()) == value.toDouble();
    return false;
}

QString inspectSchema(const QJsonObject& schema, const QString& path)
{
    static const QSet<QString> keywords{
        "$schema", "title", "description", "default", "examples", "deprecated",
        "type", "properties", "propertyNames", "required", "additionalProperties", "items",
        "minItems", "maxItems", "uniqueItems", "minProperties", "maxProperties",
        "enum", "const", "minimum", "maximum", "exclusiveMinimum", "exclusiveMaximum",
        "minLength", "maxLength", "pattern", "format", "anyOf", "oneOf", "allOf", "not",
        "propertyValues", "rowFields", "setFields", "fields"
    };
    static const QSet<QString> types{"object", "array", "string", "boolean", "number", "integer", "null"};
    for (auto it = schema.begin(); it != schema.end(); ++it) {
        const auto location = memberPath(path, it.key());
        if (!keywords.contains(it.key()))
            return location + ": unsupported schema keyword";
        if (it.key() == "type") {
            const auto values = it.value().isArray() ? it.value().toArray() : QJsonArray{it.value()};
            if (values.isEmpty()) return location + ": expected at least one type";
            for (const auto& value : values)
                if (!value.isString() || !types.contains(value.toString()))
                    return location + ": unsupported type";
        } else if (it.key() == "properties" || it.key() == "propertyValues") {
            if (!it.value().isObject()) return location + ": expected object";
            const auto properties = it.value().toObject();
            for (auto property = properties.begin(); property != properties.end(); ++property) {
                if (!property.value().isObject()) return memberPath(location, property.key()) + ": expected schema object";
                const auto error = inspectSchema(property.value().toObject(), memberPath(location, property.key()));
                if (!error.isEmpty()) return error;
            }
        } else if (it.key() == "items" || it.key() == "not" || it.key() == "additionalProperties"
                   || it.key() == "propertyNames") {
            if (it.key() == "additionalProperties" && it.value().isBool()) continue;
            if (!it.value().isObject()) return location + ": expected schema object";
            const auto error = inspectSchema(it.value().toObject(), location);
            if (!error.isEmpty()) return error;
        } else if (it.key() == "anyOf" || it.key() == "oneOf" || it.key() == "allOf") {
            if (!it.value().isArray() || it.value().toArray().isEmpty()) return location + ": expected schema array";
            int index = 0;
            for (const auto& branch : it.value().toArray()) {
                const auto branchPath = location + QStringLiteral("[%1]").arg(index++);
                if (!branch.isObject()) return branchPath + ": expected schema object";
                const auto error = inspectSchema(branch.toObject(), branchPath);
                if (!error.isEmpty()) return error;
            }
        } else if (it.key() == "required") {
            if (!it.value().isArray()) return location + ": expected array";
            for (const auto& key : it.value().toArray())
                if (!key.isString()) return location + ": expected property names";
        } else if (it.key() == "enum") {
            if (!it.value().isArray() || it.value().toArray().isEmpty()) return location + ": expected nonempty array";
        } else if (it.key() == "pattern") {
            if (!it.value().isString() || !QRegularExpression(it.value().toString()).isValid())
                return location + ": invalid regular expression";
        } else if (it.key() == "format") {
            if (it.value() != "uuid" && it.value() != "uint64") return location + ": unsupported format";
        } else if (it.key() == "uniqueItems") {
            if (!it.value().isBool()) return location + ": expected boolean";
        } else if (it.key().startsWith("min") || it.key().startsWith("max")
                   || it.key() == "exclusiveMinimum" || it.key() == "exclusiveMaximum") {
            if (!it.value().isDouble() || !std::isfinite(it.value().toDouble())) return location + ": expected finite number";
            if ((it.key().endsWith("Items") || it.key().endsWith("Length") || it.key().endsWith("Properties"))
                && (it.value().toDouble() < 0 || std::floor(it.value().toDouble()) != it.value().toDouble()))
                return location + ": expected nonnegative integer";
        }
    }
    return {};
}

QString validateValue(const QJsonValue& value, const QJsonObject& schema, const QString& path)
{
    const auto type = schema.value("type");
    if (!type.isUndefined()) {
        const auto types = type.isArray() ? type.toArray() : QJsonArray{type};
        bool matched = false;
        QStringList expected;
        for (const auto& item : types) {
            expected.append(item.toString());
            matched |= matchesType(value, item.toString());
        }
        if (!matched) return path + ": expected " + expected.join(" or ");
    }
    if (schema.contains("enum") && !schema.value("enum").toArray().contains(value))
        return path + ": value is not in the allowed enum";
    if (schema.contains("const") && schema.value("const") != value)
        return path + ": value does not match the required constant";
    if (schema.contains("not") && validateValue(value, schema.value("not").toObject(), path).isEmpty())
        return path + ": this argument combination is not allowed";
    for (const auto& combination : {QStringLiteral("allOf"), QStringLiteral("anyOf"), QStringLiteral("oneOf")}) {
        if (!schema.contains(combination)) continue;
        int matches = 0;
        QStringList errors;
        for (const auto& branch : schema.value(combination).toArray()) {
            const auto error = validateValue(value, branch.toObject(), path);
            if (error.isEmpty()) ++matches;
            else if (!errors.contains(error)) errors.append(error);
        }
        if (combination == "allOf" && !errors.isEmpty()) return errors.first();
        if (combination != "allOf" && matches == 0)
            return path + ": no allowed shape matched (" + errors.join("; ") + ")";
        if (combination == "oneOf" && matches > 1)
            return path + ": multiple mutually exclusive argument shapes matched";
    }
    if (value.isObject()) {
        const auto object = value.toObject();
        for (const auto& key : schema.value("required").toArray())
            if (!object.contains(key.toString())) return memberPath(path, key.toString()) + ": required property is missing";
        if (schema.contains("minProperties") && object.size() < schema.value("minProperties").toInt())
            return path + ": too few properties";
        if (schema.contains("maxProperties") && object.size() > schema.value("maxProperties").toInt())
            return path + ": too many properties";
        const auto properties = schema.value("properties").toObject();
        const auto additional = schema.value("additionalProperties");
        for (auto it = object.begin(); it != object.end(); ++it) {
            const auto location = memberPath(path, it.key());
            if (schema.contains("propertyNames")) {
                const auto error = validateValue(it.key(), schema.value("propertyNames").toObject(), location);
                if (!error.isEmpty()) return error;
            }
            if (!properties.contains(it.key()) && additional.isBool() && !additional.toBool())
                return location + ": unknown property";
            const auto rule = properties.contains(it.key()) ? properties.value(it.key()).toObject() : additional.toObject();
            const auto error = validateValue(it.value(), rule, location);
            if (!error.isEmpty()) return error;
        }
        const auto propertyValues = schema.value("propertyValues").toObject();
        if (object.contains("value") && propertyValues.contains(object.value("property").toString())) {
            const auto error = validateValue(object.value("value"),
                propertyValues.value(object.value("property").toString()).toObject(), memberPath(path, "value"));
            if (!error.isEmpty()) return error;
        }
    } else if (value.isArray()) {
        const auto array = value.toArray();
        if (schema.contains("minItems") && array.size() < schema.value("minItems").toInt())
            return path + QStringLiteral(": expected at least %1 items").arg(schema.value("minItems").toInt());
        if (schema.contains("maxItems") && array.size() > schema.value("maxItems").toInt())
            return path + QStringLiteral(": expected at most %1 items").arg(schema.value("maxItems").toInt());
        for (qsizetype i = 0; i < array.size(); ++i) {
            const auto location = path + QStringLiteral("[%1]").arg(i);
            if (schema.value("uniqueItems").toBool())
                for (qsizetype j = 0; j < i; ++j)
                    if (array[i] == array[j]) return location + QStringLiteral(": duplicates item %1").arg(j);
            const auto error = validateValue(array[i], schema.value("items").toObject(), location);
            if (!error.isEmpty()) return error;
        }
    } else if (value.isString()) {
        const auto string = value.toString();
        const auto length = string.toUcs4().size();
        if (schema.contains("minLength") && length < schema.value("minLength").toInt())
            return path + QStringLiteral(": expected at least %1 characters").arg(schema.value("minLength").toInt());
        if (schema.contains("maxLength") && length > schema.value("maxLength").toInt())
            return path + QStringLiteral(": expected at most %1 characters").arg(schema.value("maxLength").toInt());
        if (schema.contains("pattern") && !QRegularExpression(schema.value("pattern").toString()).match(string).hasMatch())
            return path + ": does not match pattern " + schema.value("pattern").toString();
        if (schema.value("format") == "uuid") {
            static const QRegularExpression uuid("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$");
            if (!uuid.match(string).hasMatch()) return path + ": expected UUID without braces";
        } else if (schema.value("format") == "uint64") {
            static const QRegularExpression decimal("^(0|[1-9][0-9]*)$");
            bool ok = false;
            string.toULongLong(&ok);
            if (!decimal.match(string).hasMatch() || !ok)
                return path + ": expected decimal uint64 string (0 to 18446744073709551615)";
        }
    } else if (value.isDouble()) {
        const double number = value.toDouble();
        if (!std::isfinite(number)) return path + ": expected finite number";
        for (const auto& bound : {QStringLiteral("minimum"), QStringLiteral("maximum"),
                                 QStringLiteral("exclusiveMinimum"), QStringLiteral("exclusiveMaximum")}) {
            if (!schema.contains(bound)) continue;
            const double limit = schema.value(bound).toDouble();
            const bool invalid = bound == "minimum" ? number < limit
                : bound == "maximum" ? number > limit
                : bound == "exclusiveMinimum" ? number <= limit : number >= limit;
            if (invalid) return path + QStringLiteral(": must satisfy %1 %2").arg(bound, QString::number(limit, 'g', 17));
        }
    }
    return {};
}

}

QString commandSchemaError(const QJsonObject& schema)
{
    return inspectSchema(schema, "schema");
}

bool validateCommandArguments(const QJsonObject& arguments, const QJsonObject& schema, QString* errorMessage)
{
    auto error = commandSchemaError(schema);
    if (error.isEmpty()) error = validateValue(arguments, schema, "args");
    if (errorMessage) *errorMessage = error;
    return error.isEmpty();
}

}
