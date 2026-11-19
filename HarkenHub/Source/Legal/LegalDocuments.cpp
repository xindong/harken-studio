#include "Legal/LegalDocuments.h"
#include "Localization/HubLocalization.h"

#include <QFile>

namespace VNH::LegalDocuments {

QStringList texts()
{
    const QString overridePath = qEnvironmentVariable("HARKEN_HUB_EULA_FILE");
    if (!overridePath.isEmpty()) {
        QFile file(overridePath);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
            return {QString::fromUtf8(file.readAll())};
    }
    const QString suffix = HubLocalization::effectiveLanguage() == HubLocalization::simplifiedChinesePreference()
        ? QStringLiteral(".zh-CN.md") : QStringLiteral(".md");
    QFile terms(QStringLiteral(":/Resources/legal/USER_TERMS") + suffix);
    if (!terms.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    const QStringList sections = QString::fromUtf8(terms.readAll()).split(QStringLiteral("\n---\n\n"));
    if (sections.size() != 2)
        return {};
    return {sections.at(0).mid(2), sections.at(1).mid(2)};
}

QString version() { return QStringLiteral("2026-10-28"); }

}
