
#include "BlueprintImportUtils.h"
#include "Core/AuthoringPolicy.h"
#include "Core/Components/VSound.h"
#include "Core/LanguageContextService.h"
#include "Core/ProjectLanguageSemantics.h"
#include "Core/ProjectManager.h"

#include <QFileInfo>

#include <algorithm>
#include <cmath>

namespace VNX {
namespace Blueprint {

AudioDropTarget resolveAudioDropTarget(ProjectItem* target)
{
    auto& manager = ProjectManager::instance();
    auto* project = manager.currentProject();
    const auto reject = [](const QString& error) {
        return AudioDropTarget{AudioDropKind::Rejected, nullptr, {}, error};
    };
    if (!project)
        return reject(QObject::tr("No project is currently open"));
    if (!target)
        return {AudioDropKind::Root, nullptr, {}, {}};
    if (manager.findItemByObjectID(target->objectID) != target)
        return reject(QObject::tr("The drop target no longer exists"));
    if (target->type == ProjectItem::Type::Bus)
        return {AudioDropKind::RootOutput, target, {}, {}};
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
        return {AudioDropKind::Sources, target, language, {}};
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
    return {AudioDropKind::Parent, target, {}, {}};
}

QString commonPrefix(const QStringList& filePaths)
{
    if (filePaths.size() < 2) {
        return QString();
    }

    QStringList basenames;
    basenames.reserve(filePaths.size());
    for (const QString& p : filePaths) {
        if (p.isEmpty()) {
            return QString();
        }
        basenames.push_back(QFileInfo(p).baseName());
    }

    for (const QString& b : basenames) {
        if (b.isEmpty()) {
            return QString();
        }
    }

    qsizetype minLen = basenames.front().size();
    for (const QString& b : basenames) {
        minLen = std::min(minLen, b.size());
    }

    qsizetype prefixLen = 0;
    for (qsizetype i = 0; i < minLen; ++i) {
        const QChar c = basenames.front().at(i);
        bool allMatch = true;
        for (qsizetype k = 1; k < basenames.size(); ++k) {
            if (basenames[k].at(i) != c) {
                allMatch = false;
                break;
            }
        }
        if (!allMatch) break;
        ++prefixLen;
    }

    if (prefixLen <= 0) {
        return QString();
    }

    QString shared = basenames.front().left(prefixLen);

    while (!shared.isEmpty() && shared.back().isDigit()) {
        shared.chop(1);
    }
    while (!shared.isEmpty() && !shared.back().isLetterOrNumber()) {
        shared.chop(1);
    }

    if (shared.size() < 2) {
        return QString();
    }
    return shared;
}

QPointF snapToGrid(QPointF scenePoint, double cellPx)
{
    if (cellPx <= 0.0) {
        return scenePoint;
    }
    const double sx = std::round(scenePoint.x() / cellPx) * cellPx;
    const double sy = std::round(scenePoint.y() / cellPx) * cellPx;
    return QPointF(sx, sy);
}

}
}
