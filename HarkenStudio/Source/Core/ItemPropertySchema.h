#pragma once

#include "PlayablePropertyAuthoringV1.h"
#include "Components/VEffect.h"
#include "harken_playable_properties/PlayablePropertyStudio.h"

#include <QJsonArray>
#include <QJsonObject>

namespace harken::studio {

inline QJsonObject setItemPropertySchema()
{
    QJsonArray propertyNames{
        QStringLiteral("notes"), QStringLiteral("volume"), QStringLiteral("mode"), QStringLiteral("playType"),
        QStringLiteral("variableResponse"),
        QStringLiteral("behavior"), QStringLiteral("selections"),
        QStringLiteral("loopCount"), QStringLiteral("sources"),
        QStringLiteral("scheduleOffsetSec"), QStringLiteral("distinguishLanguage"),
        QStringLiteral("clips"),
        QStringLiteral("dimensions"), QStringLiteral("maxDistance"),
        QStringLiteral("kind"), QStringLiteral("range"), QStringLiteral("rangeMin"),
        QStringLiteral("rangeMax"), QStringLiteral("defaultValue"), QStringLiteral("labels"),
        QStringLiteral("steps"),
        QStringLiteral("effects")};
    for (const auto& field : VEffect::schema())
        propertyNames.append(field.field);
    for (const auto& field : harken::studio::PlayablePropertyAuthoringV1::fields()) {
        if (!PlayablePropertyAuthoringV1::isEditorExposed(field.id)) continue;
        const QString name = QString::fromLatin1(field.fieldName);
        if (!propertyNames.contains(name)) propertyNames.append(name);
    }
    QJsonObject schema{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("path"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("minLength"), 1}}},
            {QStringLiteral("object_id"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 1}, {QStringLiteral("maximum"), 4294967295.0}}},
            {QStringLiteral("authoring_handle"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 1}, {QStringLiteral("maximum"), 4294967295.0}}},
            {QStringLiteral("property"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), propertyNames}}},
            {QStringLiteral("override_operation"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("set_override"),
                    QStringLiteral("clear_override"),
                    QStringLiteral("clear_platform_override")}}}},
            {QStringLiteral("range_policy"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{QStringLiteral("stretch"), QStringLiteral("fix")}},
                {QStringLiteral("description"), QStringLiteral("Required for Variable range changes. Consumers are inspected after a policy is supplied. Stretch scales X coordinates and values proportionally; Fix clips curves at their existing X coordinates and clamps values.")}}},
            {QStringLiteral("allow_invalid_references"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("boolean")},
                {QStringLiteral("description"), QStringLiteral("Explicitly retain Match conditions made unreachable by a Variable range change.")}}},
            {QStringLiteral("platform_ids"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("array")},
                {QStringLiteral("items"), QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("string")}}}}},
            {QStringLiteral("occurrence_kind"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("track_clip"),
                    QStringLiteral("container_child")}}}},
            {QStringLiteral("occurrence_guid"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}}},
            {QStringLiteral("occurrence_target_guid"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")}}},
            {QStringLiteral("occurrence_ordinal"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("minimum"), 0}, {QStringLiteral("maximum"), 4294967295.0}}},
            {QStringLiteral("value"), QJsonObject{}}
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("property")}},
        {QStringLiteral("anyOf"), QJsonArray{
            QJsonObject{{QStringLiteral("required"), QJsonArray{QStringLiteral("path")}}},
            QJsonObject{{QStringLiteral("required"), QJsonArray{QStringLiteral("object_id")}}},
            QJsonObject{{QStringLiteral("required"), QJsonArray{QStringLiteral("authoring_handle")}}}}},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("propertyValues"), QJsonObject{
            {QStringLiteral("notes"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
            {QStringLiteral("variableResponse"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("enum"), QJsonArray{0, 1}},
                {QStringLiteral("description"), QStringLiteral("Response of the current Match or Blend mode to variable changes: 0 Fixed, 1 Live.")}}},
            {QStringLiteral("mode"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("enum"), QJsonArray{0, 2, 4, 5}},
                {QStringLiteral("description"), QStringLiteral("Container selection mode: 0 Random, 2 Sequence, 4 Match, 5 Blend.")}}},
            {QStringLiteral("playType"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("integer")},
                {QStringLiteral("enum"), QJsonArray{0, 1}},
                {QStringLiteral("description"), QStringLiteral("Container play type: 0 Single, 1 Queue. Queue applies to Random and Sequence modes.")}}},
            {QStringLiteral("steps"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("array")},
                {QStringLiteral("description"), QStringLiteral("Ordered Action steps. Use targetObjectID (authoring handle) or an unambiguous targetName. get_action_definition.steps returns the editable array.")},
                {QStringLiteral("items"), QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("object")},
                    {QStringLiteral("required"), QJsonArray{QStringLiteral("type")}},
                    {QStringLiteral("properties"), QJsonObject{
                        {QStringLiteral("type"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
                        {QStringLiteral("targetObjectID"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 0}}},
                        {QStringLiteral("targetName"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}}},
                        {QStringLiteral("delay"), QJsonObject{{"type", "number"}, {"minimum", 0}, {"description", "Delay in milliseconds."}}},
                        {QStringLiteral("fadeTime"), QJsonObject{{"type", "number"}, {"description", "Stop fade time in milliseconds."}}},
                        {QStringLiteral("value"), QJsonObject{{"type", "number"}, {"description", "SetVariable value."}}},
                        {QStringLiteral("scope"), QJsonObject{{"enum", QJsonArray{"Emitter", "Global"}}, {"description", "SetVariable scope."}}},
                        {QStringLiteral("resetScope"), QJsonObject{{"enum", QJsonArray{"Emitter", "Global"}}, {"description", "Reset scope."}}}
                    }}
                }}
            }},
            {QStringLiteral("dimensions"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("array")},
                {QStringLiteral("rowFields"), QJsonArray{
                    QStringLiteral("type"), QStringLiteral("points"), QStringLiteral("handles")}}}},
            {QStringLiteral("maxDistance"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")}, {QStringLiteral("minimum"), 0}}},
            {QStringLiteral("clips"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("description"), QStringLiteral("Track ClipLedger with rows and occurrenceOrdinalHighWater. Start and duration are in nanoseconds. Positive duration is the authored playback boundary.")},
                {QStringLiteral("required"), QJsonArray{
                    QStringLiteral("rows"), QStringLiteral("occurrenceOrdinalHighWater")}}}},
            {QStringLiteral("volume"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}}},
            {QStringLiteral("loopCount"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 0}}},
            {QStringLiteral("scheduleOffsetSec"), QJsonObject{{QStringLiteral("type"), QStringLiteral("number")}, {QStringLiteral("minimum"), 0}}},
            {QStringLiteral("distinguishLanguage"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
            {QStringLiteral("behavior"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("fields"), QJsonArray{
                    QStringLiteral("discipline"),
                    QStringLiteral("avoidRepeat"), QStringLiteral("memoryScope"),
                    QStringLiteral("resumeFromLast")}}}},
            {QStringLiteral("selections"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("description"), QStringLiteral("Complete selection tables for shared and each platform override. Every table has entries, matchAxes, matchRows, blendAxes, occurrenceOrdinalHighWater and ruleOrdinalHighWater. matchRows are ordered Match rules {stableRuleOrdinal, target, cells}: stableRuleOrdinal is unique and below ruleOrdinalHighWater; target is the childGuid of an entry, or null for a rule without a child; cells has one value per matchAxes Variable, where null matches any value, an integer is a Variable value and {labelId} is a Variable label. Rules without a target are kept in the project and skipped at playback.")},
                {QStringLiteral("required"), QJsonArray{QStringLiteral("shared"), QStringLiteral("platforms")}}}},
            {QStringLiteral("sources"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("required"), QJsonArray{
                    QStringLiteral("plain"), QStringLiteral("localized")}},
                {QStringLiteral("setFields"), QJsonArray{
                    QStringLiteral("entries"), QStringLiteral("active"),
                    QStringLiteral("platforms")}},
                {QStringLiteral("rowFields"), QJsonArray{
                    QStringLiteral("entryId"), QStringLiteral("sourceId"),
                    QStringLiteral("trimStartFrame"), QStringLiteral("trimEndFrame"),
                    QStringLiteral("fadeIn"), QStringLiteral("fadeOut"),
                    QStringLiteral("loopRegion"), QStringLiteral("loopProvenance"),
                    QStringLiteral("rangeContentHash"), QStringLiteral("name")}}}}
        }}
    };
    auto values = schema.value(QStringLiteral("propertyValues")).toObject();
    for (const auto& field : VEffect::schema())
        values.insert(field.field, QJsonObject{
            {QStringLiteral("type"), QStringLiteral("number")},
            {QStringLiteral("minimum"), field.min.toDouble()},
            {QStringLiteral("maximum"), field.max.toDouble()}});
    schema.insert(QStringLiteral("propertyValues"), values);
    return schema;
}

}
