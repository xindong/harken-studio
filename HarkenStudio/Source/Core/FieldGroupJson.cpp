#include "FieldGroupJson.h"

#include "JsonReader.h"
#include "JsonWriter.h"

#include <QJsonDocument>
#include <QJsonParseError>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <set>
#include <utility>

namespace VNX {
namespace {

using harkenprojectschema::AuthoringGuid;

using harkenstudiojson::JsonValue;
using harkenstudiojson::JsonWriter;

bool Fail(std::string& error, const std::string& path,
          const std::string& message)
{
    error = path + ": " + message;
    return false;
}

bool CheckMembers(const JsonValue& value, const std::string& path,
                  std::initializer_list<std::string_view> allowed,
                  std::initializer_list<std::string_view> required,
                  std::string& error)
{
    if (value.kind != JsonValue::Kind::Object)
        return Fail(error, path, "expected an object");
    for (const auto& member : value.members)
        if (std::find(allowed.begin(), allowed.end(), member.first) == allowed.end())
            return Fail(error, path + "." + member.first, "unknown member");
    for (const auto key : required)
        if (!value.Find(std::string(key)))
            return Fail(error, path + "." + std::string(key), "missing member");
    return true;
}

int HexDigit(char character)
{
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

bool ReadGuid(const JsonValue& value, AuthoringGuid& guid,
              const std::string& path, std::string& error)
{
    if (value.kind != JsonValue::Kind::String || value.text.size() != 36)
        return Fail(error, path, "expected a 128-bit UUID string");
    AuthoringGuid parsed{};
    std::size_t digits = 0;
    for (std::size_t index = 0; index < value.text.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            if (value.text[index] != '-')
                return Fail(error, path, "invalid UUID separator");
            continue;
        }
        const int digit = HexDigit(value.text[index]);
        if (digit < 0) return Fail(error, path, "invalid UUID digit");
        auto& part = digits < 16 ? parsed.high : parsed.low;
        part = (part << 4) | static_cast<std::uint64_t>(digit);
        ++digits;
    }
    guid = parsed;
    return true;
}

std::string WriteGuid(AuthoringGuid guid)
{
    const std::string digits = JsonWriter::HexU64(guid.high)
        + JsonWriter::HexU64(guid.low);
    return digits.substr(0, 8) + "-" + digits.substr(8, 4)
        + "-" + digits.substr(12, 4) + "-" + digits.substr(16, 4)
        + "-" + digits.substr(20, 12);
}

bool ReadFloat(const JsonValue& value, float& number,
               const std::string& path, std::string& error)
{
    if (value.kind != JsonValue::Kind::Number)
        return Fail(error, path, "expected a finite float");
    float parsed = 0.0f;
    const char* end = value.text.data() + value.text.size();
    const auto result = std::from_chars(value.text.data(), end, parsed);
    if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(parsed))
        return Fail(error, path, "expected a finite float");
    number = parsed;
    return true;
}

void WriteFloat(JsonWriter& writer, float value)
{
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(),
                                     value, std::chars_format::general,
                                     std::numeric_limits<float>::max_digits10);
    writer.ValueRawNumber(std::string(buffer.data(), result.ptr));
}

bool ReadFieldValue(const JsonValue& value, float& field,
                     const std::string& path, std::string& error)
{
    return ReadFloat(value, field, path, error);
}

bool ReadFieldValue(const JsonValue& value, AuthoringGuid& field,
                     const std::string& path, std::string& error)
{
    return ReadGuid(value, field, path, error);
}

bool ReadFieldValue(const JsonValue& value, bool& field,
                     const std::string& path, std::string& error)
{
    if (value.kind != JsonValue::Kind::Bool)
        return Fail(error, path, "expected a boolean");
    field = value.boolean;
    return true;
}

bool ReadFieldValue(const JsonValue& value, std::vector<SendFieldGroupRow>& rows,
                     const std::string& path, std::string& error)
{
    if (value.kind != JsonValue::Kind::Array)
        return Fail(error, path, "expected an array");
    for (std::size_t index = 0; index < value.items.size(); ++index) {
        const auto& entry = value.items[index];
        const auto rowPath = path + "[" + std::to_string(index) + "]";
        if (!CheckMembers(entry, rowPath, {"targetBusID", "sendLevel", "mode", "modeVolumes"},
                          {"targetBusID", "sendLevel"}, error)) return false;
        SendFieldGroupRow row;
        if (!ReadGuid(*entry.Find("targetBusID"), row.targetBusID, rowPath + ".targetBusID", error)
            || !ReadFloat(*entry.Find("sendLevel"), row.sendLevel, rowPath + ".sendLevel", error))
            return false;
        if (const auto* mode = entry.Find("mode")) {
            unsigned parsed = 0;
            const char* end = mode->text.data() + mode->text.size();
            const auto converted = std::from_chars(mode->text.data(), end, parsed);
            if (mode->kind != JsonValue::Kind::Number || converted.ec != std::errc{}
                || converted.ptr != end
                || (parsed != static_cast<unsigned>(harkenprojectschema::SendMode::Mix)
                    && parsed != static_cast<unsigned>(harkenprojectschema::SendMode::Ducking)
                    && parsed != static_cast<unsigned>(harkenprojectschema::SendMode::Sidechain)))
                return Fail(error, rowPath + ".mode", "expected a send mode");
            row.mode = static_cast<harkenprojectschema::SendMode>(parsed);
        }
        if (const auto* volumes = entry.Find("modeVolumes")) {
            if (!CheckMembers(*volumes, rowPath + ".modeVolumes", {"0", "1", "2"}, {}, error))
                return false;
            for (const auto& [key, value] : volumes->members) {
                const auto mode = static_cast<harkenprojectschema::SendMode>(key.front() - '0');
                float volume = 0.0f;
                if (mode == row.mode)
                    return Fail(error, rowPath + ".modeVolumes." + key, "active mode uses sendLevel");
                if (!ReadFloat(value, volume, rowPath + ".modeVolumes." + key, error)) return false;
                if (volume < -96.0f || volume > 24.0f)
                    return Fail(error, rowPath + ".modeVolumes." + key, "volume is outside [-96, 24]");
                row.modeVolumesDb.emplace(mode, volume);
            }
        }
        rows.push_back(row);
    }
    return true;
}

bool ReadEffectJson(const JsonValue& value, QJsonValue& result,
                    const std::string& path, std::string& error)
{
    switch (value.kind) {
    case JsonValue::Kind::Null: result = QJsonValue::Null; return true;
    case JsonValue::Kind::Bool: result = value.boolean; return true;
    case JsonValue::Kind::String: result = QString::fromStdString(value.text); return true;
    case JsonValue::Kind::Number: {
        QJsonParseError detail;
        const auto number = QJsonDocument::fromJson(
            QByteArray::fromStdString("[" + value.text + "]"), &detail);
        if (detail.error != QJsonParseError::NoError)
            return Fail(error, path, detail.errorString().toStdString());
        result = number.array().at(0);
        return true;
    }
    case JsonValue::Kind::Array: {
        QJsonArray array;
        for (std::size_t index = 0; index < value.items.size(); ++index) {
            QJsonValue entry;
            if (!ReadEffectJson(value.items[index], entry,
                    path + "[" + std::to_string(index) + "]", error)) return false;
            array.append(entry);
        }
        result = array;
        return true;
    }
    case JsonValue::Kind::Object: {
        QJsonObject object;
        for (const auto& [name, value] : value.members) {
            QJsonValue entry;
            if (!ReadEffectJson(value, entry, path + "." + name, error)) return false;
            object.insert(QString::fromStdString(name), entry);
        }
        result = object;
        return true;
    }
    }
    return false;
}

bool ReadFieldValue(const JsonValue& value, EffectChainLedger& effects,
                    const std::string& path, std::string& error)
{
    if (value.kind != JsonValue::Kind::Array)
        return Fail(error, path, "expected an array");
    QJsonValue converted;
    if (!ReadEffectJson(value, converted, path, error)) return false;
    auto array = converted.toArray();
    for (std::size_t index = 0; index < value.items.size(); ++index) {
        const auto& entry = value.items[index];
        const auto* reference = entry.Find("effectRef");
        if (!reference) continue;
        const auto rowPath = path + "[" + std::to_string(index) + "]";
        if (entry.Find("effectGuid")
            || !CheckMembers(*reference, rowPath + ".effectRef", {"id", "documentId"},
                {"id", "documentId"}, error))
            return Fail(error, rowPath, "expected an exclusive effect reference");
        AuthoringGuid document;
        AuthoringGuid guid;
        if (!ReadGuid(*reference->Find("documentId"), document,
                rowPath + ".effectRef.documentId", error) || harkenstudio::IsNull(document))
            return Fail(error, rowPath + ".effectRef.documentId", "expected a non-null document UUID");
        if (!ReadGuid(*reference->Find("id"), guid, rowPath + ".effectRef.id", error)) return false;
        auto object = array.at(static_cast<qsizetype>(index)).toObject();
        object.remove(QStringLiteral("effectRef"));
        object.insert(QStringLiteral("effectGuid"), QString::fromStdString(WriteGuid(guid)));
        array[static_cast<qsizetype>(index)] = object;
    }
    const auto parsed = EffectChainLedger::fromJson(array);
    if (!effects.replace(parsed.entries())) return Fail(error, path, "invalid effect chain");
    return true;
}

void WriteFieldValue(JsonWriter& writer, float value) { WriteFloat(writer, value); }
void WriteFieldValue(JsonWriter& writer, bool value) { writer.ValueBool(value); }
void WriteFieldValue(JsonWriter& writer, AuthoringGuid value) { writer.ValueString(WriteGuid(value)); }

void WriteFieldValue(JsonWriter& writer, const std::vector<SendFieldGroupRow>& rows)
{
    writer.BeginArray();
    for (const auto& row : rows) {
        writer.BeginObject();
        writer.Key("targetBusID");
        writer.ValueString(WriteGuid(row.targetBusID));
        writer.Key("sendLevel");
        WriteFloat(writer, row.sendLevel);
        writer.Key("mode");
        writer.ValueRawNumber(std::to_string(static_cast<unsigned>(row.mode)));
        if (!row.modeVolumesDb.empty()) {
            writer.Key("modeVolumes");
            writer.BeginObject();
            for (const auto& [mode, volume] : row.modeVolumesDb) {
                writer.Key(std::to_string(static_cast<unsigned>(mode)).c_str());
                WriteFloat(writer, volume);
            }
            writer.EndObject();
        }
        writer.EndObject();
    }
    writer.EndArray();
}

void WriteFieldValue(JsonWriter& writer, const EffectChainLedger& effects)
{
    writer.ValueRawJson(QJsonDocument(effects.toJson()).toJson(QJsonDocument::Compact).toStdString());
}

template <typename Payload>
bool ReadState(const JsonValue& value, GroupState<Payload>& state,
               const std::string& path, std::string& error, bool shared = false)
{
    if (!CheckMembers(value, path, {"inherit", "values"}, {}, error)) return false;
    state.mode = FieldGroupMode::UseShared;
    if (const auto* inherit = value.Find("inherit")) {
        bool enabled = false;
        if (!ReadFieldValue(*inherit, enabled, path + ".inherit", error)) return false;
        state.mode = enabled ? FieldGroupMode::Inherit : FieldGroupMode::Local;
    } else if (shared) {
        return Fail(error, path + ".inherit", "missing shared inheritance flag");
    }
    if (const auto* values = value.Find("values")) {
        if (values->kind != JsonValue::Kind::Object || values->members.empty())
            return Fail(error, path + ".values", "expected a nonempty object");
        Payload payload;
        std::uint64_t fields = 0;
        bool valid = true;
        std::set<std::string> known;
        VisitFieldGroupFields(payload, [&](auto property, const char* name, auto& field) {
            known.insert(name);
            if (!valid) return;
            if (const auto* source = values->Find(name)) {
                valid = ReadFieldValue(*source, field, path + ".values." + name, error);
                fields |= FieldGroupFieldMask(property);
            } else if (shared) {
                valid = Fail(error, path + ".values." + name, "missing shared value");
            }
        });
        if (!valid) return false;
        for (const auto& [name, field] : values->members)
            if (!known.contains(name)) return Fail(error, path + ".values." + name, "unknown member");
        state.localHistory = std::move(payload);
        state.localFields = fields;
    }
    return true;
}

template <typename Payload>
void WriteState(JsonWriter& writer, const GroupState<Payload>& state)
{
    writer.BeginObject();
    if (state.mode != FieldGroupMode::UseShared) {
        writer.Key("inherit");
        writer.ValueBool(state.mode == FieldGroupMode::Inherit);
    }
    if (state.localHistory && state.localFields != 0) {
        writer.Key("values");
        writer.BeginObject();
        VisitFieldGroupFields(*state.localHistory, [&](auto property, const char* name, const auto& value) {
            if ((state.localFields & (FieldGroupFieldMask(property))) == 0) return;
            writer.Key(name);
            WriteFieldValue(writer, value);
        });
        writer.EndObject();
    }
    writer.EndObject();
}


template <typename Payload>
bool ReadPlatformState(const JsonValue& value, const char* name,
                       AuthoringGuid platform, AuthoredGroup<Payload>& group,
                       const std::string& path, std::string& error)
{
    if (const auto* entry = value.Find(name)) {
        GroupState<Payload> state;
        if (!ReadState(*entry, state, path + "." + name, error)) return false;
        group.platforms.emplace(platform, std::move(state));
    }
    return true;
}

template <typename Payload>
void WritePlatformState(JsonWriter& writer, const char* name,
                        AuthoringGuid platform, const AuthoredGroup<Payload>& group)
{
    if (const auto found = group.platforms.find(platform); found != group.platforms.end()) {
        writer.Key(name);
        WriteState(writer, found->second);
    }
}

bool ValidateGroups(const DefinitionFieldGroups& groups, std::string& error)
{
    const auto diagnostics = ValidateDefinitionFieldGroups(groups);
    if (diagnostics.empty()) return true;
    const auto& diagnostic = diagnostics.front();
    std::string path = "fieldGroups";
    if (diagnostic.scope)
        path += ".platforms." + WriteGuid(*diagnostic.scope);
    else path += ".shared";
    if (diagnostic.group)
        path += "." + std::string(FieldGroupName(*diagnostic.group));
    if (!diagnostic.field.empty()) path += "." + std::string(diagnostic.field);
    if (diagnostic.row) path += "[" + std::to_string(*diagnostic.row) + "]";
    return Fail(error, path, "invalid field group state ("
                + std::to_string(static_cast<unsigned>(diagnostic.code)) + ")");
}

bool CheckOccurrence(const JsonValue& value, const std::string& path,
                     std::string& error);

bool CheckSpatial(const JsonValue& value, const std::string& path,
                  std::string& error)
{
    if (value.kind != JsonValue::Kind::Object)
        return Fail(error, path, "expected an object");
    for (const auto& [name, member] : value.members) {
        if (name == "panning" || name == "is3DEnabled"
            || name == "attenuationDistanceScaling" || name == "attenuationRef"
            || name == "attenuation" || name == "attenuationGuid")
            return Fail(error, path + "." + name, "occurrences cannot author field groups");
    }
    return CheckOccurrence(value, path, error);
}

bool IsFieldGroupPropertyJsonKey(std::string_view key)
{
    if (FieldGroupForField(key)) return true;
    for (const auto& property : harkenplayableproperties::kPlayablePropertyRegistryV1)
        if (key == property.jsonKey && FieldGroupForProperty(property.id))
            return true;
    return false;
}

bool CheckPresence(const JsonValue& value, const std::string& path,
                   std::string& error)
{
    if (value.kind != JsonValue::Kind::String || value.text.size() != 16)
        return Fail(error, path, "expected a 16-digit presence mask");
    std::uint64_t bits = 0;
    for (const char character : value.text) {
        const int digit = HexDigit(character);
        if (digit < 0) return Fail(error, path, "invalid presence mask");
        bits = (bits << 4) | static_cast<std::uint64_t>(digit);
    }
    if ((bits & kFieldGroupPropertyMask) != 0)
        return Fail(error, path, "occurrences cannot author field group properties");
    return true;
}

bool CheckComponents(const JsonValue& value, const std::string& path,
                     std::string& error)
{
    if (!CheckOccurrence(value, path, error)) return false;
    if (const auto* mixnode = value.Find("mixnode"))
        if (!CheckOccurrence(*mixnode, path + ".mixnode", error)) return false;
    if (const auto* audio = value.Find("audio"))
        if (!CheckOccurrence(*audio, path + ".audio", error)) return false;
    return true;
}

bool CheckOccurrence(const JsonValue& value, const std::string& path,
                     std::string& error)
{
    if (value.kind != JsonValue::Kind::Object)
        return Fail(error, path, "expected an object");
    for (const auto& [name, member] : value.members) {
        const std::string memberPath = path + "." + name;
        if (name == "fieldGroups" || name == "sectionGroups" || name == "output" || name == "send"
            || name == "spatialRating" || name == "effect" || name == "effects"
            || name == "outputBusId" || name == "outputBusRef"
            || name == "outputBusOverride" || name == "overrideBits"
            || IsFieldGroupPropertyJsonKey(name))
            return Fail(error, memberPath, "occurrences cannot author field groups");
        if (name == "spatial") {
            if (!CheckSpatial(member, memberPath, error)) return false;
        } else if (name == "presence") {
            if (!CheckPresence(member, memberPath, error)) return false;
        } else if (name == "playableProperties" || name == "properties") {
            if (!CheckOccurrence(member, memberPath, error)) return false;
        } else if (name == "components") {
            if (!CheckComponents(member, memberPath, error)) return false;
        } else if (name == "platformOverrides") {
            if (member.kind != JsonValue::Kind::Array)
                return Fail(error, memberPath, "expected an array");
            for (std::size_t index = 0; index < member.items.size(); ++index)
                if (!CheckOccurrence(member.items[index],
                                     memberPath + "[" + std::to_string(index) + "]", error))
                    return false;
        }
    }
    return true;
}

}

bool ReadFieldGroupsJson(const std::string& text,
                           DefinitionFieldGroups& groups,
                           std::string& error)
{
    JsonValue root;
    if (!harkenstudiojson::ParseJson(text, &root, &error, true)) return false;
    if (!CheckMembers(root, "fieldGroups", {"shared", "platforms"}, {"shared"}, error))
        return false;
    const auto& shared = *root.Find("shared");
    if (!CheckMembers(shared, "fieldGroups.shared", {"output", "send", "spatialRating", "effect"},
                      {"output", "send", "spatialRating", "effect"}, error))
        return false;
    DefinitionFieldGroups parsed;
    if (!ReadState(*shared.Find("output"), parsed.output.shared,
                   "fieldGroups.shared.output", error, true)
        || !ReadState(*shared.Find("send"), parsed.send.shared,
                      "fieldGroups.shared.send", error, true)
        || !ReadState(*shared.Find("spatialRating"), parsed.spatialRating.shared,
                      "fieldGroups.shared.spatialRating", error, true)
        || !ReadState(*shared.Find("effect"), parsed.effects.shared,
                      "fieldGroups.shared.effect", error, true))
        return false;
    if (const auto* platforms = root.Find("platforms")) {
        if (platforms->kind != JsonValue::Kind::Object)
            return Fail(error, "fieldGroups.platforms", "expected an object");
        for (const auto& [name, value] : platforms->members) {
            const auto path = "fieldGroups.platforms." + name;
            JsonValue identity;
            identity.kind = JsonValue::Kind::String;
            identity.text = name;
            AuthoringGuid platform;
            if (!ReadGuid(identity, platform, path, error)) return false;
            if (harkenprojectschema::IsNull(platform) || WriteGuid(platform) != name)
                return Fail(error, path, "expected a canonical non-null platform UUID");
            if (!CheckMembers(value, path, {"output", "send", "spatialRating", "effect"}, {}, error)
                || !ReadPlatformState(value, "output", platform, parsed.output, path, error)
                || !ReadPlatformState(value, "send", platform, parsed.send, path, error)
                || !ReadPlatformState(value, "spatialRating", platform, parsed.spatialRating, path, error)
                || !ReadPlatformState(value, "effect", platform, parsed.effects, path, error))
                return false;
        }
    }
    if (!ValidateGroups(parsed, error)) return false;
    groups = std::move(parsed);
    error.clear();
    return true;
}

bool WriteFieldGroupsJson(const DefinitionFieldGroups& groups,
                            std::string& text,
                            std::string& error)
{
    if (!ValidateGroups(groups, error)) return false;
    JsonWriter writer;
    writer.BeginObject();
    writer.Key("shared");
    writer.BeginObject();
    writer.Key("output");
    WriteState(writer, groups.output.shared);
    writer.Key("send");
    WriteState(writer, groups.send.shared);
    writer.Key("spatialRating");
    WriteState(writer, groups.spatialRating.shared);
    writer.Key("effect");
    WriteState(writer, groups.effects.shared);
    writer.EndObject();
    if (!groups.output.platforms.empty() || !groups.send.platforms.empty()
        || !groups.spatialRating.platforms.empty() || !groups.effects.platforms.empty()) {
        writer.Key("platforms");
        writer.BeginObject();
        std::set<AuthoringGuid> platforms;
        for (const auto& [platform, state] : groups.output.platforms) platforms.insert(platform);
        for (const auto& [platform, state] : groups.send.platforms) platforms.insert(platform);
        for (const auto& [platform, state] : groups.spatialRating.platforms) platforms.insert(platform);
        for (const auto& [platform, state] : groups.effects.platforms) platforms.insert(platform);
        for (const auto platform : platforms) {
            writer.Key(WriteGuid(platform).c_str());
            writer.BeginObject();
            WritePlatformState(writer, "output", platform, groups.output);
            WritePlatformState(writer, "send", platform, groups.send);
            WritePlatformState(writer, "spatialRating", platform, groups.spatialRating);
            WritePlatformState(writer, "effect", platform, groups.effects);
            writer.EndObject();
        }
        writer.EndObject();
    }
    writer.EndObject();
    text = writer.Finish();
    error.clear();
    return true;
}

bool ValidateOccurrenceFieldGroupExclusionJson(const std::string& text,
                                                std::string& error)
{
    JsonValue root;
    if (!harkenstudiojson::ParseJson(text, &root, &error)) return false;
    if (!CheckOccurrence(root, "occurrence", error)) return false;
    error.clear();
    return true;
}

}
