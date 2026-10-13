#pragma once

#include "PlayablePropertyAuthoringV1.h"
#include "harken_playable_property_studio.g.h"

#include <QJsonArray>
#include <QJsonObject>

namespace VNX {

inline QJsonObject setItemPropertySchema()
{
    QJsonArray propertyNames{
        QStringLiteral("volume"), QStringLiteral("behavior"), QStringLiteral("selections"),
        QStringLiteral("loopCount"), QStringLiteral("sources"),
        QStringLiteral("scheduleOffsetSec"), QStringLiteral("distinguishLanguage"),
        QStringLiteral("clips"), QStringLiteral("pieceTime"),
        QStringLiteral("dimensions"), QStringLiteral("maxDistance"),
        QStringLiteral("transitions")};
    for (const auto& field : VNX::PlayablePropertyAuthoringV1::fields()) {
        if (!PlayablePropertyAuthoringV1::isEditorExposed(field.id)) continue;
        const QString name = QString::fromLatin1(field.fieldName);
        if (!propertyNames.contains(name)) propertyNames.append(name);
    }
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("properties"), QJsonObject{
            {QStringLiteral("path"), QJsonObject{{QStringLiteral("type"), QStringLiteral("string")}, {QStringLiteral("minLength"), 1}}},
            {QStringLiteral("object_id"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 1}}},
            {QStringLiteral("authoring_handle"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}, {QStringLiteral("minimum"), 1}}},
            {QStringLiteral("property"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), propertyNames}}},
            {QStringLiteral("override_operation"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("string")},
                {QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("set_override"),
                    QStringLiteral("clear_override"),
                    QStringLiteral("clear_platform_override")}}}},
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
                {QStringLiteral("minimum"), 0}}},
            {QStringLiteral("value"), QJsonObject{}}
        }},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("property")}},
        {QStringLiteral("anyOf"), QJsonArray{
            QJsonObject{{QStringLiteral("required"), QJsonArray{QStringLiteral("path")}}},
            QJsonObject{{QStringLiteral("required"), QJsonArray{QStringLiteral("object_id")}}},
            QJsonObject{{QStringLiteral("required"), QJsonArray{QStringLiteral("authoring_handle")}}}}},
        {QStringLiteral("additionalProperties"), false},
        {QStringLiteral("propertyValues"), QJsonObject{
            {QStringLiteral("dimensions"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("array")},
                {QStringLiteral("rowFields"), QJsonArray{
                    QStringLiteral("type"), QStringLiteral("points"), QStringLiteral("handles")}}}},
            {QStringLiteral("maxDistance"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("number")}, {QStringLiteral("minimum"), 0}}},
            {QStringLiteral("clips"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("description"), QStringLiteral("Track ClipLedger with rows and occurrenceOrdinalHighWater. Start positions use absolute nanoseconds. Positive duration is the authored playback boundary. Targets must support absolute-time authoring.")},
                {QStringLiteral("required"), QJsonArray{
                    QStringLiteral("rows"), QStringLiteral("occurrenceOrdinalHighWater")}}}},
            {QStringLiteral("pieceTime"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("description"), QStringLiteral("Only domain Absolute is supported. Musical, tempoMap, and meterMap authoring is unavailable. Existing musical arrangements are preserved; this command does not migrate them.")},
                {QStringLiteral("properties"), QJsonObject{
                    {QStringLiteral("domain"), QJsonObject{
                        {QStringLiteral("const"), QStringLiteral("Absolute")}}}}}}},
            {QStringLiteral("transitions"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("object")},
                {QStringLiteral("description"), QStringLiteral("Container transition table. Authoring exit timings: Immediate, NaturalEnd, NextMarker, ExitMarker. NextBeat and NextBar cannot be introduced or modified. Existing unsupported rules may be deleted or replaced with supported timings. Runtime support is assessed separately for each operation and target.")}}},
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
                    QStringLiteral("loopRegion"), QStringLiteral("loopProvenance"),
                    QStringLiteral("rangeContentHash")}}}}
        }}
    };
}

}
