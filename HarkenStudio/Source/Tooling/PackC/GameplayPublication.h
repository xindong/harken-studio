#pragma once

#include "PackSidecar.h"
#include <harken_studio_build/PackImage.h>
#include <harken_studio_build/PackFormatV2.h>

namespace harkenstudiobuild::tooling {

inline bool EncodeGameplayPublication(const harkenstudiojson::JsonValue& root,
                                      std::vector<std::uint8_t>* bytes) {
    using harkenstudiojson::JsonValue;
    using harkenstudiojson::PackSidecarText;
    const auto* json = root.Find("gameplayCatalog");
    if (!json || !harkenstudiojson::PackSidecarKeys(*json, {"schema", "projectId", "initPackStableId",
            "initContentRevision", "digest", "actions", "retired"})) return false;
    const auto number = [](const JsonValue& row, const char* key, std::uint64_t* out) {
        const auto* value = row.Find(key);
        return value && (value->kind == JsonValue::Kind::Number ? value->AsU64(out)
            : value->kind == JsonValue::Kind::String && harkenstudiojson::PackSidecarU64(value->text, out));
    };
    std::uint64_t schema = 0;
    harkenstudio::ObjectId project{};
    PackGameplayCatalogHeader header;
    if (!number(*json, "schema", &schema) || schema != 2 ||
        !ParseObjectId(PackSidecarText(*json, "projectId"), &project) || project == harkenstudio::ObjectId{} ||
        !number(*json, "initPackStableId", &header.initPackStableId) ||
        !number(*json, "initContentRevision", &header.initContentRevision)) return false;
    StoreObjectId(header.projectGuid.data(), project);
    const auto* actions = json->Find("actions");
    const auto* history = json->Find("retired");
    if (!actions || actions->kind != JsonValue::Kind::Array ||
        !history || history->kind != JsonValue::Kind::Array) return false;
    std::vector<PackGameplayCatalogActiveDraft> active;
    std::vector<PackGameplayCatalogRetiredDraft> retired;
    for (const auto& item : actions->items) {
        if (!harkenstudiojson::PackSidecarKeys(item, {"name", "actionStableId", "nameBindingRevision",
                "callableFingerprint", "ownerPackStableId", "providerClosureFingerprint"})) return false;
        PackGameplayCatalogActiveDraft row;
        const auto* name = item.Find("name");
        if (!name || name->kind != JsonValue::Kind::String ||
            !ParseObjectId(PackSidecarText(item, "actionStableId"), &row.actionStableId) ||
            !number(item, "nameBindingRevision", &row.nameBindingRevision) ||
            !number(item, "callableFingerprint", &row.callableFingerprint) ||
            !number(item, "ownerPackStableId", &row.ownerPackStableId) ||
            !number(item, "providerClosureFingerprint", &row.providerClosureFingerprint)) return false;
        row.name = name->text;
        if (GameplayCallableFingerprint(row.actionStableId, row.nameBindingRevision, row.name) != row.callableFingerprint)
            return false;
        active.push_back(row);
    }
    for (const auto& item : history->items) {
        if (!harkenstudiojson::PackSidecarKeys(item, {"name", "lastActionStableId", "lastNameBindingRevision", "reason"})) return false;
        PackGameplayCatalogRetiredDraft row;
        const auto* name = item.Find("name");
        if (!name || name->kind != JsonValue::Kind::String ||
            !ParseObjectId(PackSidecarText(item, "lastActionStableId"), &row.lastActionStableId) ||
            !number(item, "lastNameBindingRevision", &row.lastNameBindingRevision)) return false;
        row.name = name->text;
        const auto reason = PackSidecarText(item, "reason");
        row.reason = reason == "renamed" ? 1 : reason == "deleted" ? 2 : reason == "reused" ? 3 : 0;
        retired.push_back(row);
    }
    if (!EncodeGameplayCatalog(header, active, retired, bytes)) return false;
    GameplayCatalogImage decoded;
    return ParseGameplayCatalog(bytes->data(), bytes->size(), &decoded) == kPackOk;
}

}
