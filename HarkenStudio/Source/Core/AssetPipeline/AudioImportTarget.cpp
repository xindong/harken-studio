
#include "AudioImportTarget.h"
#include "Core/AuthoringPolicy.h"
#include "Core/Components/VSound.h"
#include "Core/LanguageContextService.h"
#include "Core/ProjectLanguageSemantics.h"
#include "Core/ProjectManager.h"


#include <algorithm>

namespace VNX {

AudioImportTarget resolveAudioImportTarget(ProjectItem* target)
{
    auto& manager = ProjectManager::instance();
    auto* project = manager.currentProject();
    const auto reject = [](const QString& error) {
        return AudioImportTarget{AudioImportKind::Rejected, nullptr, {}, error};
    };
    if (!project)
        return reject(QObject::tr("No project is currently open"));
    if (!target)
        return {AudioImportKind::Root, nullptr, {}, {}};
    if (manager.findItemByObjectID(target->objectID) != target)
        return reject(QObject::tr("The drop target no longer exists"));
    if (target->type == ProjectItem::Type::Bus)
        return {AudioImportKind::RootOutput, target, {}, {}};
    if (!findFileBindingForItem(*project, target))
        return reject(QObject::tr("The drop target has no owning Pack"));
    if (target->type == ProjectItem::Type::Sound) {
        const auto* sound = target->comp<VSound>();
        if (!sound)
            return reject(QObject::tr("The drop target has no Sound sources"));
        QString language;
        if (sound->distinguishLanguage) {
            language = LanguageContextService::instance().language();
            const auto languages = ProjectLanguageSemantics::buildLanguages(*project);
            if (!languages.contains(language)
                || std::any_of(sound->sources.localized.keyBegin(), sound->sources.localized.keyEnd(),
                    [&languages](const QString& key) { return !languages.contains(key); }))
                return reject(QObject::tr("Per-language sources must use project languages before importing."));
        }
        return {AudioImportKind::Sources, target, language, {}};
    }
    if (!ProjectManager::canAcceptImportedAudioParent(target))
        return reject(QObject::tr("The drop target cannot contain imported audio"));
    ProjectItem candidate(QStringLiteral("Imported Sound"), ProjectItem::Type::Sound);
    candidate.attachDefaultComponents();
    const auto authoring = AuthoringPolicy::validateNewSubtree(candidate, target,
        [&manager](const QUuid& guid) { return manager.findItemByGuid(guid); },
        [&manager](uint32_t id) { return manager.findItemByObjectID(id); });
    if (!authoring)
        return reject(authoring.message);
    return {AudioImportKind::Parent, target, {}, {}};
}

}
