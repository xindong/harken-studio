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
    const QStringList sansCandidates = { QStringLiteral("Inter"), QStringLiteral("Segoe UI") };
    const QStringList availableFamilies = QFontDatabase::families();
    systemSansFamily_.clear();
    for (const auto& candidate : sansCandidates) {
        if (availableFamilies.contains(candidate)) {
            systemSansFamily_ = candidate;
            break;
        }
    }
    if (systemSansFamily_.isEmpty()) {
        QFont defaultFont;
        defaultFont.setStyleHint(QFont::SansSerif);
        systemSansFamily_ = defaultFont.defaultFamily();
    }
#ifdef Q_OS_WIN
    systemMonoFamily_ = QStringLiteral("Consolas");
#else
    systemMonoFamily_ = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
#endif
    sansFamily_ = systemSansFamily_;
    monoFamily_ = systemMonoFamily_;
}

QFont FontManager::getFont(FontRole role, FontEmphasis emphasis) const
{
    return getFont(snapshot_, role, emphasis);
}

QFont FontManager::getFont(FontRole role, int sizePtOverride) const
{
    QFont font = getFont(role);
    if (sizePtOverride > 0)
        font.setPointSizeF(qMax(6.0, sizePtOverride * double(scaleFactor_)));
    return font;
}

QFont FontManager::getFont(const ThemeSnapshotPtr& snapshot, FontRole role,
                         FontEmphasis emphasis) const
{
    const ThemeTypography defaults;
    const auto& typography = snapshot ? snapshot->ui.typography : defaults;
    const ThemeFontStyle* style = &typography.body;
    switch (role) {
        case FontRole::Title: style = &typography.title; break;
        case FontRole::Body: break;
        case FontRole::Caption: style = &typography.caption; break;
        case FontRole::Mono: style = &typography.mono; break;
    }
    const bool mono = role == FontRole::Mono;
    const QString family = !style->family.isEmpty() ? style->family
        : mono ? systemMonoFamily_
        : !typography.body.family.isEmpty() ? typography.body.family : systemSansFamily_;
    QFont font(family);
    const int size = style->pointSize > 0 ? style->pointSize : typography.body.pointSize;
    font.setPointSizeF(qMax(6.0, size * double(scaleFactor_)));
    font.setWeight(static_cast<QFont::Weight>(emphasis == FontEmphasis::Strong
        ? typography.title.weight : style->weight > 0 ? style->weight : typography.body.weight));
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
    if (snapshot_ == snapshot)
        return;
    const bool changed = !snapshot_ || !snapshot
        || snapshot_->ui.typography != snapshot->ui.typography;
    snapshot_ = std::move(snapshot);
    if (!changed)
        return;
    sansFamily_ = getFont(FontRole::Body).family();
    monoFamily_ = getFont(FontRole::Mono).family();
    emit fontChanged();
}

void FontManager::setScaleFactor(float factor)
{
    factor = qBound(0.5f, factor, 3.0f);
    if (qFuzzyCompare(scaleFactor_, factor))
        return;
    scaleFactor_ = factor;
    emit fontChanged();
}

}
