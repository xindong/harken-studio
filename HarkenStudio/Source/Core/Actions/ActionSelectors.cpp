
#include "ActionSelectors.h"

#include "../ProjectManager.h"

#include <QLatin1Char>
#include <QStringLiteral>

namespace VNX {

ResolvedSelector resolveProjectSelector(ProjectData* project, const QString& selector,
                                         std::optional<ProjectItem::Type> type)
{
    ResolvedSelector result;
    result.name = selector;
    if (!project) {
        result.ok = false;
        result.errorCode = QStringLiteral("NO_PROJECT");
        result.errorMessage = QStringLiteral("No project open");
        return result;
    }
    const auto matches = project->findItemsBySelector(selector, type);
    if (matches.size() == 1) {
        result.item = matches.front();
        result.name = result.item->name;
        return result;
    }
    result.ok = false;
    result.errorCode = matches.empty() ? QStringLiteral("PATH_NOT_FOUND") : QStringLiteral("AMBIGUOUS_SELECTOR");
    result.errorMessage = matches.empty()
        ? QStringLiteral("Object not found: %1").arg(selector)
        : QStringLiteral("Multiple objects match '%1'; select an object by GUID.").arg(selector);
    for (const auto* item : matches)
        result.candidates.append(QJsonObject{{QStringLiteral("guid"), item->guid.toString(QUuid::WithoutBraces)},
                                            {QStringLiteral("name"), item->name},
                                            {QStringLiteral("path"), project->itemPath(item)}});
    return result;
}

ResolvedSelector resolveSelector(const QString& selector)
{
    ResolvedSelector r;
    r.name = selector;

    if (selector.isEmpty())
        return r;

    const QString kPackPrefix = QStringLiteral("Pack://");
    QString stripped = selector;
    bool hadPrefix = false;
    if (selector.startsWith(kPackPrefix)) {
        stripped = selector.mid(kPackPrefix.length());
        hadPrefix = true;
    }

    const bool isPath = hadPrefix || stripped.contains(QLatin1Char('/'))
        || stripped.startsWith(QStringLiteral("guid:")) || !QUuid::fromString(stripped).isNull();
    if (!isPath) {
        return r;
    }

    auto* project = ProjectManager::instance().currentProject();
    if (!project) {
        r.ok = false;
        r.errorCode = QStringLiteral("NO_PROJECT");
        r.errorMessage = QStringLiteral("No project open for path selector: ") + selector;
        return r;
    }

    return resolveProjectSelector(project, stripped);
}

}
