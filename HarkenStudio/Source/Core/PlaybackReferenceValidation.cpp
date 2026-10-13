#include "PlaybackReferenceValidation.h"

#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "ItemTypeCatalog.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectTypes.h"

#include <QHash>
#include <QSet>

#include <vector>

namespace VNX {

const ProjectItem* findDirectPlaybackChild(const ProjectItem& owner, const QUuid& guid)
{
    for (const auto& child : owner.children)
        if (child && child->guid == guid) return child.get();
    return nullptr;
}

bool validateDirectPlaybackTarget(const ProjectItem& owner, const QUuid& guid,
                                 const ProjectItem* target, QString* error,
                                 PlaybackReferenceError* code)
{
    QString reason;
    if (guid.isNull() || !target || target->guid != guid) {
        if (code) *code = PlaybackReferenceError::MissingTarget;
        reason = QStringLiteral("Target is missing from the candidate object tree or direct children");
    } else if ((owner.type != ProjectItem::Type::Container
                && owner.type != ProjectItem::Type::Track)
               || !ItemTypeCatalog::canBeChildOf(target->type, owner.type)
               || (owner.type == ProjectItem::Type::Track
                   && !ItemTypeCatalog::isTimelineClipTarget(target->type))) {
        if (code) *code = PlaybackReferenceError::InvalidTargetType;
        reason = QStringLiteral("Target type %1 is not allowed for %2 playback references")
                     .arg(ItemTypeCatalog::canonicalName(target->type),
                          ItemTypeCatalog::canonicalName(owner.type));
    } else if (target->parent != &owner
               || findDirectPlaybackChild(owner, guid) != target) {
        if (code) *code = PlaybackReferenceError::InvalidOwnership;
        reason = QStringLiteral("Target must be a direct child; its current parent is %1 (%2)")
                     .arg(target->parent ? target->parent->name : QStringLiteral("<root>"),
                          target->parent ? target->parent->guid.toString(QUuid::WithoutBraces)
                                         : QStringLiteral("none"));
    } else {
        return true;
    }
    if (error)
        *error = QStringLiteral("Owner '%1' (%2), target %3: %4")
                     .arg(owner.name, owner.guid.toString(QUuid::WithoutBraces),
                          guid.toString(QUuid::WithoutBraces), reason);
    return false;
}

namespace {

bool validatePlaybackRoots(const std::vector<const ProjectItem*>& roots, QString* error,
                           PlaybackReferenceError* code)
{
    if (code) *code = PlaybackReferenceError::InvalidStructure;
    QHash<QUuid, const ProjectItem*> targets;
    QSet<const ProjectItem*> visited;
    std::vector<const ProjectItem*> owners;
    const auto collect = [&](auto&& self, const ProjectItem* item,
                             const ProjectItem* parent) -> bool {
        if (!item || visited.contains(item) || item->parent != parent
            || item->guid.isNull() || targets.contains(item->guid)) {
            if (error)
                *error = QStringLiteral("Invalid candidate object tree at '%1' (%2)")
                             .arg(item ? item->name : QStringLiteral("<null>"),
                                  item ? item->guid.toString(QUuid::WithoutBraces) : QString());
            return false;
        }
        visited.insert(item);
        targets.insert(item->guid, item);
        owners.push_back(item);
        for (const auto& child : item->children)
            if (!self(self, child.get(), item)) return false;
        return true;
    };
    for (const auto* root : roots)
        if (!collect(collect, root, root->parent))
            return false;

    for (const ProjectItem* owner : owners) {
        const auto check = [&](const QUuid& guid, const QString& record) {
            QString reason;
            if (validateDirectPlaybackTarget(*owner, guid, targets.value(guid), &reason, code))
                return true;
            if (error) *error = record + QStringLiteral(": ") + reason;
            return false;
        };
        QString shapeError;
        if (const auto* selection = owner->comp<VSelection>()) {
            for (const auto* table : selection->selections.tables())
                for (const auto& row : table->entries)
                    if (!check(row.childGuid, QStringLiteral("Selection member %1")
                                                 .arg(row.stableMemberOrdinal)))
                        return false;
        }
        if (!PlayablePropertyAuthoringV1::validateContainerOccurrences(*owner, &shapeError)) {
            if (error) *error = QStringLiteral("Owner '%1' (%2), Selection: %3")
                                   .arg(owner->name, owner->guid.toString(QUuid::WithoutBraces),
                                        shapeError);
            return false;
        }
        if (const auto* selection = owner->comp<VSelection>()) {
            const auto shape = selection->transitions.validate();
            if (!shape) {
                if (error) *error = QStringLiteral("Owner '%1' (%2), Transition: %3")
                                       .arg(owner->name, owner->guid.toString(QUuid::WithoutBraces),
                                            shape.message);
                return false;
            }
            for (size_t i = 0; i < selection->transitions.rules.size(); ++i) {
                const auto& rule = selection->transitions.rules[i];
                if (rule.source && !check(*rule.source, QStringLiteral("Transition rule %1 source").arg(i)))
                    return false;
                if (rule.target && !check(*rule.target, QStringLiteral("Transition rule %1 target").arg(i)))
                    return false;
            }
        }
        if (const auto* timeline = owner->comp<VTrack>()) {
            for (const auto& clip : timeline->clips.items)
                if (!check(clip.targetGuid, QStringLiteral("Clip %1")
                                               .arg(clip.clipGuid.toString(QUuid::WithoutBraces))))
                    return false;
        }
    }
    return true;
}

}

bool validatePlaybackReferences(const ProjectData& project, QString* error,
                                PlaybackReferenceError* code)
{
    std::vector<const ProjectItem*> roots;
    for (const auto* binding : project.allFileBindings())
        if (binding && binding->rootItem) roots.push_back(binding->rootItem.get());
    return validatePlaybackRoots(roots, error, code);
}

bool validatePlaybackReferences(const ProjectItem& root, QString* error)
{
    return validatePlaybackRoots({&root}, error, nullptr);
}

}
