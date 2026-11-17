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
        ? QStringLiteral(".zh-CN.txt") : QStringLiteral(".txt");
    QFile agreement(QStringLiteral(":/Resources/legal/USER_AGREEMENT") + suffix);
    QFile privacy(QStringLiteral(":/Resources/legal/PRIVACY_POLICY") + suffix);
    if (!agreement.open(QIODevice::ReadOnly | QIODevice::Text) || !privacy.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return {QString::fromUtf8(agreement.readAll()), QString::fromUtf8(privacy.readAll())};
}

QString version() { return QStringLiteral("2026-10-28"); }

}
