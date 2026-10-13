
#include "FontManager.h"
#include <QFontDatabase>

#include <utility>

namespace VNX {

FontManager& FontManager::instance()
{
    static FontManager inst;
    return inst;
}

FontManager::FontManager(QObject* parent)
    : QObject(parent)
{
    resolveFontFamilies();
}

void FontManager::resolveFontFamilies()
{
    const QStringList sansCandidates = { "Inter", "Segoe UI" };
    const QStringList availableFamilies = QFontDatabase::families();

    sansFamily_.clear();
    for (const auto& candidate : sansCandidates) {
        if (availableFamilies.contains(candidate)) {
            sansFamily_ = candidate;
            break;
        }
    }
    if (sansFamily_.isEmpty()) {
        QFont defaultFont;
        defaultFont.setStyleHint(QFont::SansSerif);
        sansFamily_ = defaultFont.defaultFamily();
    }
    systemSansFamily_ = sansFamily_;

    const QStringList monoCandidates = { "JetBrains Mono", "Consolas" };

    monoFamily_.clear();
    for (const auto& candidate : monoCandidates) {
        if (availableFamilies.contains(candidate)) {
            monoFamily_ = candidate;
            break;
        }
    }
    if (monoFamily_.isEmpty()) {
        QFont monoFont;
        monoFont.setStyleHint(QFont::Monospace);
        monoFamily_ = monoFont.defaultFamily();
    }
    systemMonoFamily_ = monoFamily_;
}

QFont FontManager::getFont(FontRole role) const
{
    const ThemeFontStyle* style = themeStyle(role);
    return getFont(
        role,
        style && style->pointSize > 0 ? style->pointSize : defaultSize(role),
        style && style->weight > 0
            ? static_cast<QFont::Weight>(style->weight)
            : defaultWeight(role));
}

QFont FontManager::getFont(FontRole role, int sizePtOverride) const
{
    const ThemeFontStyle* style = themeStyle(role);
    return getFont(
        role, sizePtOverride,
        style && style->weight > 0
            ? static_cast<QFont::Weight>(style->weight)
            : defaultWeight(role));
}

QFont FontManager::getFont(FontRole role, int sizePtOverride, QFont::Weight weightOverride) const
{
    const ThemeFontStyle* style = themeStyle(role);
    const bool isMono = role == FontRole::Mono;
    const QString family = style && !style->family.isEmpty()
        ? style->family
        : (isMono ? monoFamily_ : sansFamily_);

    QFont font(family);
    font.setPointSize(scaledSize(sizePtOverride));
    font.setWeight(weightOverride);

    if (isMono) {
        font.setStyleHint(QFont::Monospace);
    } else {
        font.setStyleHint(QFont::SansSerif);
    }

    return font;
}

QFont FontManager::getFont(const ThemeSnapshotPtr& snapshot, FontRole role) const
{
    const ThemeFontStyle* style = themeStyle(snapshot, role);
    const bool isMono = role == FontRole::Mono;
    const QString family = style && !style->family.isEmpty()
        ? style->family
        : (isMono ? systemMonoFamily_ : systemSansFamily_);
    QFont font(family);
    font.setPointSize(scaledSize(
        style && style->pointSize > 0 ? style->pointSize : defaultSize(role)));
    font.setWeight(style && style->weight > 0
        ? static_cast<QFont::Weight>(style->weight)
        : defaultWeight(role));
    font.setStyleHint(isMono ? QFont::Monospace : QFont::SansSerif);
    return font;
}

void FontManager::applyTheme(ThemeSnapshotPtr snapshot)
{
    if (snapshot_ == snapshot)
        return;
    snapshot_ = std::move(snapshot);
    sansFamily_ = snapshot_ && !snapshot_->ui.typography.body.family.isEmpty()
        ? snapshot_->ui.typography.body.family
        : systemSansFamily_;
    monoFamily_ = snapshot_ && !snapshot_->ui.typography.mono.family.isEmpty()
        ? snapshot_->ui.typography.mono.family
        : systemMonoFamily_;
    emit fontChanged();
}

void FontManager::setScaleFactor(float factor)
{
    if (factor < 0.5f) factor = 0.5f;
    if (factor > 3.0f) factor = 3.0f;

    if (qFuzzyCompare(scaleFactor_, factor))
        return;

    scaleFactor_ = factor;
    emit fontChanged();
}

int FontManager::scaledSize(int basePt) const
{
    int scaled = static_cast<int>(basePt * scaleFactor_ + 0.5f);
    return qMax(scaled, 6);
}

int FontManager::defaultSize(FontRole role)
{
    switch (role) {
        case FontRole::Heading: return 14;
        case FontRole::Title:   return 12;
        case FontRole::Body:    return 10;
        case FontRole::Caption: return 9;
        case FontRole::Axis:    return 8;
        case FontRole::Mono:    return 10;
        case FontRole::Label:   return 9;
    }
    return 10;
}

const ThemeFontStyle* FontManager::themeStyle(FontRole role) const
{
    return themeStyle(snapshot_, role);
}

const ThemeFontStyle* FontManager::themeStyle(
    const ThemeSnapshotPtr& snapshot,
    FontRole role)
{
    if (!snapshot)
        return nullptr;
    const auto& typography = snapshot->ui.typography;
    switch (role) {
        case FontRole::Heading: return &typography.heading;
        case FontRole::Title:   return &typography.title;
        case FontRole::Body:    return &typography.body;
        case FontRole::Caption: return &typography.caption;
        case FontRole::Axis:    return &typography.axis;
        case FontRole::Mono:    return &typography.mono;
        case FontRole::Label:   return &typography.label;
    }
    return nullptr;
}

QFont::Weight FontManager::defaultWeight(FontRole role)
{
    switch (role) {
        case FontRole::Heading: return QFont::Bold;
        case FontRole::Title:   return QFont::DemiBold;
        case FontRole::Body:    return QFont::Normal;
        case FontRole::Caption: return QFont::Normal;
        case FontRole::Axis:    return QFont::Normal;
        case FontRole::Mono:    return QFont::Normal;
        case FontRole::Label:   return QFont::Medium;
    }
    return QFont::Normal;
}

}
