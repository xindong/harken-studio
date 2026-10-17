#include "ProjectLanguageSemantics.h"

#include "ProjectTypes.h"

#include <QRegularExpression>

namespace VNX {
namespace ProjectLanguageSemantics {
namespace {

constexpr int kMaxNameLength = 32;

}

bool isSfxName(const QString& name)
{
    return name.compare(QStringLiteral("SFX"), Qt::CaseInsensitive) == 0;
}

QStringList dropSfxNames(const QStringList& names)
{
    QStringList result;
    for (const QString& raw : names) {
        const QString name = raw.trimmed();
        if (name.isEmpty() || isSfxName(name)) continue;
        result.append(name);
    }
    return result;
}

QStringList buildLanguages(const ProjectData& project)
{
    QStringList result;
    for (const QString& name : dropSfxNames(project.projectLanguages)) {
        if (!result.contains(name)) result.append(name);
    }
    return result;
}

Normalization normalize(const QStringList& requested)
{
    static const QRegularExpression allowed(QStringLiteral("^[A-Za-z0-9_]+$"));

    Normalization out;
    for (const QString& raw : requested) {
        const QString name = raw.trimmed();
        if (name.isEmpty()) continue;
        if (!allowed.match(name).hasMatch())
            return {{}, QStringLiteral(
                            "Language name \"%1\" may only contain letters, digits and underscores")
                            .arg(name)};
        if (name.size() > kMaxNameLength)
            return {{}, QStringLiteral("Language name \"%1\" is longer than %2 characters")
                            .arg(name)
                            .arg(kMaxNameLength)};
        if (isSfxName(name))
            return {{}, QStringLiteral("\"%1\" is not a language").arg(name)};
        for (const QString& kept : out.value) {
            if (kept.compare(name, Qt::CaseInsensitive) != 0) continue;
            return {{}, kept == name
                            ? QStringLiteral("Language name \"%1\" is listed more than once")
                                  .arg(name)
                            : QStringLiteral(
                                  "Language names \"%1\" and \"%2\" differ only in case")
                                  .arg(kept, name)};
        }
        out.value.append(name);
    }
    if (out.value.isEmpty())
        return {{}, QStringLiteral("At least one project language is required")};
    return out;
}

}
}
