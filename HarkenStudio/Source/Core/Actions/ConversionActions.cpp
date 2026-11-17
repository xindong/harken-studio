#include "../ActionManager.h"
#include "../CommandArgumentValidator.h"
#include "../CommandSchemas.h"
#include "../ConversionEditing.h"
#include "../ConversionResolution.h"
#include "../ConversionResults.h"
#include "../LanguageContextService.h"
#include "../NameValidator.h"
#include "../PlatformContextService.h"
#include "../PlayablePropertyAuthoringV1.h"
#include "../ProjectManager.h"
#include "../ReferenceSlotResolution.h"
#include "../SoundSourceEditing.h"
#include "../AssetPipeline/AssetRegistry.h"
#include "ActionSelectors.h"

#include <QDir>
#include <cmath>

namespace VNX {
namespace {

ActionResult success(const QJsonObject& data)
{
    ActionResult result{};
    result.success = true;
    result.data = data;
    return result;
}

ActionResult failure(const QString& code, const QString& message, const QJsonObject& data = {})
{
    ActionResult result{};
    result.data = data;
    result.errorCode = code;
    result.errorMessage = message;
    return result;
}

ActionResult invalid(const QString& message)
{
    return failure(QStringLiteral("INVALID_ARGUMENTS"), message);
}

QJsonObject describePreset(const ProjectData& project, const ProjectItem& item)
{
    QJsonArray platforms;
    for (const auto& config : item.comp<VConversion>()->platforms) {
        QJsonArray tiers;
        for (const auto& tier : config.tiers)
            tiers.append(QJsonObject{{"max_bytes", tier.maxBytes
                ? QJsonValue(static_cast<qint64>(*tier.maxBytes)) : QJsonValue(QJsonValue::Null)},
                {"sample_rate", static_cast<qint64>(tier.sampleRate)}});
        platforms.append(QJsonObject{{"platform_id", ProjectPlatformSemantics::idString(config.platformId)},
            {"format", config.format == ConversionFormat::Vorbis ? "Vorbis" : "PCM"},
            {"quality", std::round(static_cast<double>(config.vorbisQuality) * 1000000.0) / 10000.0},
            {"sample_rate_tiers", tiers}});
    }
    return {{"guid", item.guid.toString(QUuid::WithoutBraces)}, {"name", item.name},
        {"path", project.itemPath(&item)}, {"platforms", platforms}};
}

std::optional<ProjectPlatformId> platformFor(const ProjectData& project, const QJsonObject& args)
{
    const auto id = args.contains(QStringLiteral("platform_id"))
        ? ProjectPlatformSemantics::parseId(args.value(QStringLiteral("platform_id")).toString())
        : std::optional<ProjectPlatformId>{PlatformContextService::instance().context().primary};
    return id && ProjectPlatformSemantics::find(project.platforms, *id) ? id : std::nullopt;
}

ActionResult execute(ProjectManager& manager, const QString& name, const QJsonObject& args)
{
    QString error;
    if (!validateCommandArguments(args, commandInputSchema(name), &error)) return invalid(error);
    auto* project = manager.currentProject();
    if (!project) return failure(QStringLiteral("NO_PROJECT_OPEN"), QStringLiteral("No project is currently open"));
    const auto* originator = ActionManager::instance().executeOriginator();
    if (name == QStringLiteral("create_conversion")) {
        const auto requestedName = args.value(QStringLiteral("name")).toString();
        NameValidator::Result validation;
        if (!NameValidator::validateItemName(requestedName, ProjectItem::Type::Conversion, &validation))
            return invalid(NameValidator::resultMessage(validation));
        auto folder = QDir::fromNativeSeparators(args.value(QStringLiteral("folder")).toString());
        if (folder == QStringLiteral("Conversions")) folder.clear();
        else if (folder.startsWith(QStringLiteral("Conversions/"))) folder.remove(0, 12);
        if (!folder.isEmpty() && !project->findFolderByPath(QStringLiteral("Conversions"), folder))
            return invalid(QStringLiteral("folder must identify an existing Conversions directory"));
        auto* item = manager.createItem(nullptr, requestedName, ProjectItem::Type::Conversion, folder);
        if (!item) return failure(QStringLiteral("CREATE_CONVERSION_FAILED"), QStringLiteral("Cannot create Conversion preset"));
        return success(describePreset(*project, *item));
    }
    if (name == QStringLiteral("set_default_conversion") && args.value(QStringLiteral("conversion")).isNull()) {
        if (!manager.setDefaultConversion({})) return invalid(QStringLiteral("Cannot set default Conversion"));
        return success({{"conversion", QJsonValue(QJsonValue::Null)}});
    }
    const bool objectCommand = name == QStringLiteral("set_object_conversion")
        || name == QStringLiteral("get_effective_conversion");
    const auto selected = resolveProjectSelector(project,
        args.value(objectCommand ? QStringLiteral("object") : QStringLiteral("conversion")).toString(),
        objectCommand ? std::nullopt : std::optional{ProjectItem::Type::Conversion});
    if (!selected.ok)
        return failure(selected.errorCode, selected.errorMessage, {{"candidates", selected.candidates}});
    auto* item = selected.item;
    if (objectCommand && !PlayablePropertyAuthoringV1::isFieldGroupDefinitionType(item->type))
        return invalid(QStringLiteral("Object does not support Conversion settings"));
    if (name == QStringLiteral("set_default_conversion")) {
        if (!manager.setDefaultConversion(item->guid)) return invalid(QStringLiteral("Cannot set default Conversion"));
        return success({{"conversion", item->guid.toString(QUuid::WithoutBraces)}});
    }
    if (name == QStringLiteral("set_object_conversion")) {
        std::optional<QUuid> settings;
        if (args.contains(QStringLiteral("settings"))) {
            settings = QUuid{};
            if (!args.value(QStringLiteral("settings")).isNull()) {
                const auto preset = resolveProjectSelector(project, args.value(QStringLiteral("settings")).toString(),
                    ProjectItem::Type::Conversion);
                if (!preset.ok)
                    return failure(preset.errorCode, preset.errorMessage, {{"candidates", preset.candidates}});
                settings = preset.item->guid;
            }
        }
        if (!ConversionEditing::setObject(manager, item->guid, args.value(QStringLiteral("inherit")).toBool(),
            settings, originator, &error)) return invalid(error);
        const auto& group = item->fieldGroups.conversion.shared;
        const auto local = group.localHistory
            ? group.localHistory->settingsRef : harkenprojectschema::AuthoringGuid{};
        return success({{"object", item->guid.toString(QUuid::WithoutBraces)},
            {"inherit", group.mode == FieldGroupMode::Inherit},
            {"settings", harkenprojectschema::IsNull(local) ? QJsonValue(QJsonValue::Null)
                : QJsonValue(fromFieldGroupGuid(local).toString(QUuid::WithoutBraces))}});
    }
    if (name == QStringLiteral("set_conversion_fields")) {
        QSet<ProjectPlatformId> platforms;
        if (args.value(QStringLiteral("all_platforms")).toBool()) {
            for (const auto& platform : project->platforms) platforms.insert(platform.id);
        } else {
            for (const auto& value : args.value(QStringLiteral("platform_ids")).toArray()) {
                const auto platform = ProjectPlatformSemantics::parseId(value.toString());
                if (!platform || !ProjectPlatformSemantics::find(project->platforms, *platform))
                    return invalid(QStringLiteral("Unknown project platform ID"));
                platforms.insert(*platform);
            }
        }
        const auto before = item->comp<VConversion>()->platforms;
        auto replacement = before;
        for (auto& config : replacement) {
            if (!platforms.contains(config.platformId)) continue;
            auto json = config.toJson();
            if (args.contains(QStringLiteral("format"))) json.insert(QStringLiteral("format"), args.value(QStringLiteral("format")));
            if (args.contains(QStringLiteral("quality")))
                json.insert(QStringLiteral("vorbisQuality"), args.value(QStringLiteral("quality")).toDouble() / 100.0);
            if (args.contains(QStringLiteral("sample_rate_tiers"))) {
                QJsonArray tiers;
                for (const auto& value : args.value(QStringLiteral("sample_rate_tiers")).toArray()) {
                    const auto tier = value.toObject();
                    tiers.append(QJsonObject{{"maxBytes", tier.value(QStringLiteral("max_bytes"))},
                        {"sampleRate", tier.value(QStringLiteral("sample_rate"))}});
                }
                json.insert(QStringLiteral("sampleRateTiers"), tiers);
            }
            const auto parsed = ConversionPlatformConfig::fromJson(json, &error);
            if (!parsed) return invalid(error);
            config = *parsed;
        }
        if (!ConversionEditing::setPlatforms(manager, item->guid, replacement, before, originator, &error))
            return invalid(error);
        return success(describePreset(*project, *item));
    }
    const auto platform = platformFor(*project, args);
    if (!platform) return invalid(QStringLiteral("Unknown project platform ID"));
    if (name == QStringLiteral("get_effective_conversion")) {
        const auto resolved = ReferenceSlotResolution::resolveEffectiveConversion(
            ReferenceSlotResolution::buildResolutionScope(*project, item, toFieldGroupGuid(*platform)));
        if (!resolved.valid) return invalid(resolved.error);
        QJsonObject result{{"object", item->guid.toString(QUuid::WithoutBraces)},
            {"platform_id", ProjectPlatformSemantics::idString(*platform)},
            {"conversion", resolved.target ? QJsonValue(resolved.target->guid.toString(QUuid::WithoutBraces))
                : QJsonValue(QJsonValue::Null)},
            {"owner", resolved.owner ? QJsonValue(resolved.owner->guid.toString(QUuid::WithoutBraces))
                : QJsonValue(QJsonValue::Null)}, {"sample_rate", QJsonValue(QJsonValue::Null)}};
        if (resolved.target) result.insert(QStringLiteral("preset"), describePreset(*project, *resolved.target));
        if (item->type == ProjectItem::Type::Sound) {
            const auto* source = SoundSourceEditing::activeSource(*item, *platform,
                SoundSourceEditing::previewLanguage(*item, LanguageContextService::instance().language()));
            const auto* asset = source ? AssetRegistry::instance().getAsset(source->sourceId) : nullptr;
            if (asset) {
                const auto conversion = resolveConversion(*project, *item, *platform, *asset, &error);
                if (!error.isEmpty()) return invalid(error);
                result.insert(QStringLiteral("sample_rate"), static_cast<qint64>(
                    conversion && conversion->sampleRate != 0 ? conversion->sampleRate : asset->metadata.sampleRate));
                result.insert(QStringLiteral("source_id"), static_cast<qint64>(source->sourceId));
                result.insert(QStringLiteral("source_entry_id"), source->entryId.toString(QUuid::WithoutBraces));
            }
        }
        return success(result);
    }
    QJsonObject result{{"conversion", item->guid.toString(QUuid::WithoutBraces)},
        {"platform_id", ProjectPlatformSemantics::idString(*platform)}};
    if (name == QStringLiteral("convert")) {
        const auto batch = ConversionResults::Batch::create(*project, item->guid, *platform, &error);
        if (!batch) return invalid(error);
        const auto converted = batch->run();
        result.insert(QStringLiteral("conversion_result"), converted.toJson());
        const auto rows = ConversionResults::query(*project, item->guid, *platform, &error);
        result.insert(QStringLiteral("sources"), ConversionResults::toJson(rows));
        if (!converted.errors.isEmpty() || !error.isEmpty())
            return failure(QStringLiteral("CONVERSION_FAILED"),
                error.isEmpty() ? converted.errors.join(QLatin1Char('\n')) : error, result);
        return success(result);
    }
    const auto rows = ConversionResults::query(*project, item->guid, *platform, &error);
    if (!error.isEmpty()) return invalid(error);
    result.insert(QStringLiteral("sources"), ConversionResults::toJson(rows));
    return success(result);
}

}

void registerConversionActions(ActionManager& manager, ProjectManager* projectManager)
{
    for (const auto* name : {"create_conversion", "set_conversion_fields", "set_object_conversion",
        "set_default_conversion", "get_effective_conversion", "get_conversion_results", "convert"}) {
        ActionDef action;
        action.id = QLatin1String(name);
        action.displayName = action.id;
        action.category = QStringLiteral("asset");
        action.undoable = action.id == QStringLiteral("create_conversion")
            || action.id == QStringLiteral("set_conversion_fields")
            || action.id == QStringLiteral("set_object_conversion")
            || action.id == QStringLiteral("set_default_conversion");
        action.paramSchema = commandInputSchema(action.id);
        action.handler = [projectManager, id = action.id](const QJsonObject& args) {
            if (!projectManager)
                return failure(QStringLiteral("NO_PROJECT_OPEN"), QStringLiteral("No project is currently open"));
            return execute(*projectManager, id, args);
        };
        manager.registerAction(action);
    }
}

}
