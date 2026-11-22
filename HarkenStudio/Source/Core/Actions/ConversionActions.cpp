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
#include <algorithm>

namespace harken::studio {
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

QJsonObject describeConfig(const ConversionConfig& config)
{
    QJsonArray rules;
    QJsonArray limits;
    for (const auto& rule : config.rules) {
        QJsonObject row{{"max_bytes", rule.maxBytes ? QJsonValue(static_cast<qint64>(*rule.maxBytes)) : QJsonValue::Null},
                        {"sample_rate", static_cast<qint64>(rule.sampleRate)}};
        limits.append(row);
        row.insert("format", rule.format == ConversionFormat::Vorbis ? "Vorbis" : "PCM");
        if (rule.vorbisQuality) row.insert("quality", *rule.vorbisQuality);
        rules.append(row);
    }
    QJsonObject result{{"rules", rules}};
    const auto& first = config.rules.front();
    if (std::all_of(config.rules.begin(), config.rules.end(), [&](const ConversionRule& rule) {
            return rule.format == first.format && rule.vorbisQuality == first.vorbisQuality;
        })) {
        result.insert("sample_rate_tiers", limits);
        result.insert("format", rules.first().toObject().value("format"));
        if (first.vorbisQuality) result.insert("quality", *first.vorbisQuality);
    }
    return result;
}

QJsonObject describePreset(const ProjectData& project, const ProjectItem& item)
{
    auto result = describeConfig(item.comp<VConversion>()->config);
    result.insert("guid", item.guid.toString(QUuid::WithoutBraces));
    result.insert("name", item.name);
    result.insert("path", project.itemPath(&item));
    return result;
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
        const auto platform = platformFor(*project, args);
        if (!platform) return invalid(QStringLiteral("Unknown project platform ID"));
        if (!manager.setDefaultConversion(*platform, {})) return invalid(QStringLiteral("Cannot set default Conversion"));
        return success({{"platform_id", ProjectPlatformSemantics::idString(*platform)},
            {"conversion", QJsonValue(QJsonValue::Null)}});
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
        const auto platform = platformFor(*project, args);
        if (!platform) return invalid(QStringLiteral("Unknown project platform ID"));
        if (!manager.setDefaultConversion(*platform, item->guid)) return invalid(QStringLiteral("Cannot set default Conversion"));
        return success({{"platform_id", ProjectPlatformSemantics::idString(*platform)},
            {"conversion", item->guid.toString(QUuid::WithoutBraces)}});
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
        PlatformScope platform;
        if (args.contains(QStringLiteral("platform_id"))) {
            platform = platformFor(*project, args);
            if (!platform) return invalid(QStringLiteral("Unknown project platform ID"));
        }
        if (!ConversionEditing::setObject(manager, item->guid, args.value(QStringLiteral("inherit")).toBool(),
            settings, platform, originator, &error)) return invalid(error);
        const auto& groups = item->fieldGroups.conversion;
        const auto found = platform ? groups.platforms.find(toFieldGroupGuid(*platform)) : groups.platforms.end();
        const auto* group = platform ? (found == groups.platforms.end() ? nullptr : &found->second) : &groups.shared;
        const auto local = group && group->localHistory
            ? group->localHistory->settingsRef : harken::studio::project_schema::AuthoringGuid{};
        QJsonObject result{{"object", item->guid.toString(QUuid::WithoutBraces)},
            {"inherit", group && group->mode == FieldGroupMode::Inherit},
            {"settings", harken::studio::project_schema::IsNull(local) ? QJsonValue(QJsonValue::Null)
                : QJsonValue(fromFieldGroupGuid(local).toString(QUuid::WithoutBraces))}};
        if (platform) result.insert(QStringLiteral("platform_id"), ProjectPlatformSemantics::idString(*platform));
        return success(result);
    }
    if (name == QStringLiteral("set_conversion_fields")) {
        const auto before = item->comp<VConversion>()->config;
        auto requested = args.value("rules").toArray();
        if (!args.contains("rules")) {
            const auto previous = describeConfig(before);
            if (!previous.contains("format")) return invalid(QStringLiteral("Specify rules to edit a mixed-format or mixed-quality preset"));
            const auto format = (args.contains("format") ? args : previous).value("format").toString();
            const auto quality = args.value("quality").toInt(before.rules.front().vorbisQuality.value_or(kDefaultVorbisQuality));
            for (const auto& value : (args.contains("sample_rate_tiers") ? args : previous).value("sample_rate_tiers").toArray()) {
                auto row = value.toObject();
                row.insert("format", format);
                if (format == QLatin1String("Vorbis")) row.insert("quality", quality);
                requested.append(row);
            }
        }
        QJsonArray rules;
        for (const auto& entry : requested) {
            auto rule = entry.toObject();
            rule.insert("maxBytes", rule.take("max_bytes"));
            rule.insert("sampleRate", rule.take("sample_rate"));
            rules.append(rule);
        }
        const auto parsed = ConversionConfig::fromJson({{"rules", rules}}, &error);
        if (!parsed) return invalid(error);
        if (!ConversionEditing::setConfig(manager, item->guid, *parsed, before, originator, &error))
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
                result.insert(QStringLiteral("format"), conversion && conversion->format == ConversionFormat::Vorbis ? "Vorbis" : "PCM");
                if (conversion && conversion->vorbisQuality)
                    result.insert(QStringLiteral("quality"), *conversion->vorbisQuality);
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
