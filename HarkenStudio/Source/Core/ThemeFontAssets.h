#pragma once

#include <QList>
#include <QStringList>

namespace harken::studio {

struct ThemePackageCandidate;
struct ThemePackageDiagnostic;

struct ThemeFontAssetFamily
{
    QString name;
    QString renderFamily;
};

bool prepareThemeFonts(ThemePackageCandidate& candidate, QList<ThemePackageDiagnostic>& diagnostics);
QStringList installedSystemFontFamilies();

}
