#include "FontManager.h"
#include "BuiltinTheme.h"
#include "ThemeFontAssets.h"

#include <QFontDatabase>
#include <QGuiApplication>

#include <utility>

namespace harken::studio {

FontManager& FontManager::instance()
{
    static FontManager inst;
    return inst;
}

FontManager::FontManager(QObject* parent)
    : QObject(parent)
{
    systemSansFamily_ = QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
    systemMonoFamily_ = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
    QFont sans;
    sans.setStyleHint(QFont::SansSerif);
    defaultSansFamily_ = sans.defaultFamily();
    refreshSystemFamilies();
    connect(qGuiApp, &QGuiApplication::fontDatabaseChanged, this, &FontManager::refreshSystemFamilies);
    sansFamily_ = familyFor({}, FontRole::Body).family;
    monoFamily_ = familyFor({}, FontRole::Mono).family;
}

void FontManager::refreshSystemFamilies()
{
    QHash<QString, QString> names;
    const auto families = installedSystemFontFamilies();
    for (const auto& name : families)
        names.insert(name.toCaseFolded(), name);
    QWriteLocker lock(&familyLock_);
    installedNames_.swap(names);
}

QFont FontManager::getFont(FontRole role, FontEmphasis emphasis) const
{
    return getFont(snapshot_.lock(), role, emphasis);
}

QFont FontManager::getFont(FontRole role, int sizePtOverride) const
{
    QFont font = getFont(role);
    if (sizePtOverride > 0)
        font.setPointSizeF(qMax(6, sizePtOverride) * scaleFactor());
    return font;
}

QStringList FontManager::themeFamilies(const ThemeSnapshotPtr& snapshot) const
{
    QStringList names;
    if (snapshot)
        for (const auto& asset : snapshot->fontAssets)
            names.append(asset->name);
    return names;
}

QStringList FontManager::systemFamilies() const
{
    return installedSystemFontFamilies();
}

ResolvedFontFamily FontManager::familyFor(const ThemeSnapshotPtr& snapshot, FontRole role) const
{
    const auto& typography = snapshot ? snapshot->ui.typography : builtinThemeTypography();
    const auto& style = typography.*themeFontToken(role).member;
    if (!style.source && style.family.isEmpty() && (role == FontRole::Title || role == FontRole::Caption))
        return familyFor(snapshot, FontRole::Body);
    if (style.source == ThemeFontSource::Theme) {
        for (const auto& asset : snapshot->fontAssets)
            if (asset->name.compare(style.family, Qt::CaseInsensitive) == 0)
                return {asset->name, ThemeFontSource::Theme};
    } else if (!style.family.isEmpty()) {
        QReadLocker lock(&familyLock_);
        auto found = installedNames_.constFind(style.family.toCaseFolded());
        if (found != installedNames_.cend()) return {found.value(), ThemeFontSource::System};
        for (const auto& candidate : style.fallbackFamilies) {
            found = installedNames_.constFind(candidate.toCaseFolded());
            if (found != installedNames_.cend()) return {found.value(), ThemeFontSource::System};
        }
        return {role == FontRole::Mono ? systemMonoFamily_ : defaultSansFamily_, ThemeFontSource::System};
    }
    return {role == FontRole::Mono ? systemMonoFamily_ : systemSansFamily_, ThemeFontSource::System};
}

QFont FontManager::getFont(const ThemeSnapshotPtr& snapshot, FontRole role,
                         FontEmphasis emphasis) const
{
    const auto& typography = snapshot ? snapshot->ui.typography : builtinThemeTypography();
    const auto& style = typography.*themeFontToken(role).member;
    const bool mono = role == FontRole::Mono;
    auto family = familyFor(snapshot, role);
    if (family.source == ThemeFontSource::Theme)
        for (const auto& asset : snapshot->fontAssets)
            if (asset->name == family.family) {
                family.family = asset->renderFamily;
                break;
            }
    QFont font(family.family);
    const int size = style.pointSize > 0 ? style.pointSize : typography.body.pointSize;
    font.setPointSizeF(qMax(6, size) * (snapshot ? snapshot->uiScale : 1.0));
    font.setWeight(static_cast<QFont::Weight>(emphasis == FontEmphasis::Strong
        ? typography.title.weight : style.weight > 0 ? style.weight : typography.body.weight));
    font.setStyleHint(mono ? QFont::Monospace : QFont::SansSerif);
    font.setStretch(QFont::Unstretched);
    font.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    font.setWordSpacing(0);
    font.setStyle(QFont::StyleNormal);
    font.setCapitalization(QFont::MixedCase);
    font.setKerning(true);
    return font;
}

QString FontManager::styleSheet(const ThemeSnapshotPtr& snapshot, FontRole role,
                               FontEmphasis emphasis) const
{
    const QFont font = getFont(snapshot, role, emphasis);
    QString family = font.family();
    family.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    family.replace(QLatin1Char('\''), QStringLiteral("\\'"));
    return QStringLiteral("font-family: '%1'; font-size: %2pt; font-weight: %3; letter-spacing: 0;")
        .arg(family).arg(font.pointSizeF()).arg(int(font.weight()));
}

void FontManager::applyTheme(ThemeSnapshotPtr snapshot)
{
    const auto previous = snapshot_.lock();
    if (previous == snapshot)
        return;
    const bool changed = !previous || !snapshot
        || previous->ui.typography != snapshot->ui.typography
        || previous->uiScale != snapshot->uiScale
        || previous->fontAssets != snapshot->fontAssets;
    snapshot_ = snapshot;
    if (!changed)
        return;
    sansFamily_ = getFont(FontRole::Body).family();
    monoFamily_ = getFont(FontRole::Mono).family();
    emit fontChanged();
}

}
