#include "ConversionEditing.h"

#include "FieldGroupEditing.h"
#include "FieldGroupProjectAdapter.h"
#include "ProjectManager.h"
#include "PropertyEditSession.h"
#include "UndoCommands.h"

#include <QSet>
#include <algorithm>

namespace harken::studio::ConversionEditing {
namespace {

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

class ConfigEdit final : public TransactionalUndoCommand
{
public:
    ConfigEdit(ProjectManager& manager, const QUuid& guid, ConversionConfig before,
        ConversionConfig after, const QObject* originator)
        : TransactionalUndoCommand(QStringLiteral("Edit Conversion"), nullptr, false),
          manager_(manager), project_(manager.currentProjectLease()), guid_(guid),
          before_(std::move(before)), after_(std::move(after)), originator_(originator) {}

private:
    bool apply(bool redo) override
    {
        const auto project = project_.lock();
        if (!project || manager_.currentProject() != project.get()) return false;
        auto* item = manager_.findItemByGuid(guid_);
        auto* conversion = item ? item->comp<VConversion>() : nullptr;
        const auto& expected = redo ? before_ : after_;
        const auto& replacement = redo ? after_ : before_;
        if (!conversion || conversion->config != expected || !validateConversionConfig(replacement))
            return false;
        conversion->config = replacement;
        item->markDirty();
        emit manager_.itemPropertyChanged(item, QStringLiteral("conversion"), originator_);
        originator_ = nullptr;
        return true;
    }

    ProjectManager& manager_;
    std::weak_ptr<ProjectData> project_;
    QUuid guid_;
    ConversionConfig before_;
    ConversionConfig after_;
    const QObject* originator_;
};

class ObjectEdit final : public TransactionalUndoCommand
{
public:
    ObjectEdit(ProjectManager& manager, QVector<FieldGroupDefinitionSnapshot> before,
        QVector<FieldGroupDefinitionSnapshot> after, const QObject* originator)
        : TransactionalUndoCommand(QStringLiteral("Set Object Conversion"), nullptr, false),
          manager_(manager), project_(manager.currentProjectLease()),
          before_(std::move(before)), after_(std::move(after)), originator_(originator) {}

private:
    bool apply(bool redo) override
    {
        const auto project = project_.lock();
        if (!project || manager_.currentProject() != project.get()) return false;
        const auto& expected = redo ? before_ : after_;
        const auto& replacement = redo ? after_ : before_;
        for (qsizetype index = 0; index < expected.size(); ++index) {
            auto* item = manager_.findItemByGuid(expected[index].definition);
            if (!item || item->fieldGroups != expected[index].groups) return false;
        }
        if (!validateFieldGroupProject(*project,
            std::span<const FieldGroupDefinitionSnapshot>(replacement.data(), static_cast<size_t>(replacement.size()))).ok()) return false;
        for (qsizetype index = 0; index < replacement.size(); ++index) {
            auto* item = manager_.findItemByGuid(replacement[index].definition);
            if (item && manager_.restoreFieldGroupsInternal(item, replacement[index].groups,
                expected[index].groups, originator_)) continue;
            while (index > 0) {
                --index;
                manager_.restoreFieldGroupsInternal(manager_.findItemByGuid(expected[index].definition),
                    expected[index].groups, replacement[index].groups, originator_);
            }
            return false;
        }
        originator_ = nullptr;
        return true;
    }

    ProjectManager& manager_;
    std::weak_ptr<ProjectData> project_;
    QVector<FieldGroupDefinitionSnapshot> before_;
    QVector<FieldGroupDefinitionSnapshot> after_;
    const QObject* originator_;
};

}

bool setConfig(ProjectManager& manager, const QUuid& conversion,
    const ConversionConfig& replacement, const std::optional<ConversionConfig>& expectedBefore,
    const QObject* originator, QString* error)
{
    PropertyEditSession::finishAllActive(true);
    auto* project = manager.currentProject();
    auto* item = project ? manager.findItemByGuid(conversion) : nullptr;
    const auto* component = item ? item->comp<VConversion>() : nullptr;
    if (!component) return fail(error, QStringLiteral("Conversion preset is unavailable"));
    if (expectedBefore && component->config != *expectedBefore)
        return fail(error, QStringLiteral("Conversion settings changed before commit"));
    if (!validateConversionConfig(replacement, error)) return false;
    if (component->config == replacement) return true;
    auto command = std::make_unique<ConfigEdit>(manager, conversion, component->config, replacement, originator);
    if (!command->tryReplay(true))
        return fail(error, QStringLiteral("Conversion settings changed before commit"));
    UndoRedoManager::instance().push(command.release());
    return true;
}

bool setObject(ProjectManager& manager, const QUuid& object, bool inherit,
    const std::optional<QUuid>& settings, const PlatformScope& platform,
    const QObject* originator, QString* error)
{
    return setObjects(manager, {object}, inherit, settings, platform, {}, originator, error);
}

bool setObjects(ProjectManager& manager, const QVector<QUuid>& objects,
    std::optional<bool> inherit, const std::optional<QUuid>& settings, const PlatformScope& platform,
    const QVector<FieldGroupDefinitionSnapshot>& expectedBefore,
    const QObject* originator, QString* error)
{
    PropertyEditSession::finishAllActive(true);
    auto* project = manager.currentProject();
    if (!project || objects.isEmpty()) return fail(error, QStringLiteral("Object is unavailable"));
    if (platform && !ProjectPlatformSemantics::find(project->platforms, *platform))
        return fail(error, QStringLiteral("Conversion platform does not belong to the project"));
    if (inherit.value_or(false) && settings)
        return fail(error, QStringLiteral("Inherited Conversion cannot specify local settings"));
    if (!inherit && !settings) return fail(error, QStringLiteral("No Conversion edit was specified"));
    QVector<FieldGroupDefinitionSnapshot> before;
    QVector<FieldGroupDefinitionSnapshot> after;
    QSet<QUuid> visited;
    const auto mode = inherit.value_or(false) ? FieldGroupMode::Inherit : FieldGroupMode::Local;
    for (const auto& object : objects) {
        if (visited.contains(object)) continue;
        visited.insert(object);
        auto* item = manager.findItemByGuid(object);
        if (!item) return fail(error, QStringLiteral("Object is unavailable"));
        FieldGroupEditRequest request;
        request.definition = object;
        request.group = FieldGroupId::Conversion;
        request.scopes.shared = true;
        request.operation = SetFieldGroupMode{mode};
        if (!expectedBefore.empty()) {
            const auto expected = std::find_if(expectedBefore.begin(), expectedBefore.end(),
                [&](const auto& value) { return value.definition == object; });
            if (expected == expectedBefore.end())
                return fail(error, QStringLiteral("Expected Conversion state does not include every object"));
            request.expectedBefore = expected->groups;
        }
        const bool root = !item->parent;
        const auto prepared = item->fieldGroups.conversion.shared.mode == mode
            || (root && mode == FieldGroupMode::Local)
            ? prepareFieldGroupRestore(*project, object, item->fieldGroups, request.expectedBefore)
            : prepareFieldGroupEdit(*project, request);
        if (!prepared.ok()) return fail(error, prepared.diagnostics.front().message);
        auto replacement = *prepared.after;
        if (settings) {
            auto& state = platform
                ? replacement.conversion.platforms.try_emplace(toFieldGroupGuid(*platform),
                    GroupState<ConversionFieldGroup>{FieldGroupMode::UseShared, {}}).first->second
                : replacement.conversion.shared;
            state.localHistory = ConversionFieldGroup{toFieldGroupGuid(*settings)};
            state.localFields = FieldGroupPropertyMask(FieldGroupId::Conversion);
            if (!platform) replacement.conversion.platforms.clear();
        }
        const auto validated = prepareFieldGroupRestore(*project, object, replacement, prepared.before);
        if (!validated.ok()) return fail(error, validated.diagnostics.front().message);
        if (!validated.changed) continue;
        before.push_back({object, *validated.before});
        after.push_back({object, *validated.after});
    }
    if (after.empty()) return true;
    auto command = std::make_unique<ObjectEdit>(manager, std::move(before), std::move(after), originator);
    if (!command->tryReplay(true))
        return fail(error, QStringLiteral("Object Conversion changed before commit"));
    UndoRedoManager::instance().push(command.release());
    return true;
}

}
