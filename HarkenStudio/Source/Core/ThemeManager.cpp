#include "ThemeManager.h"

#include "harken_buildinfo.h"
#include "BuiltinTheme.h"
#include "ThemeColorResolver.h"
#include "FontManager.h"
#include "HarkenStyle.h"
#include "QtAdsThemeAdapter.h"
#include "QtControlSkin.h"
#include "QSettingsStorage.h"

#include <QApplication>
#include <QDir>
#include <QPainter>
#include <QRect>
#include <QStandardPaths>
#include <QStyleHints>
#include <QThread>
#include <QWidget>

#include <utility>

namespace VNX {

ThemeManager& ThemeManager::instance()
{
    static ThemeManager instance;
    return instance;
}

ThemeManager::ThemeManager(QObject* parent)
    : ThemeManager(
          QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
              .filePath(QStringLiteral("themes")),
          SettingsStorageFactory::create(
              SettingsStorageFactory::StorageType::QSettings,
              QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")),
          parent)
{
}

ThemeManager::ThemeManager(QString userThemesRoot,
                           std::unique_ptr<ISettingsStorage> storage,
                           QObject* parent)
    : QObject(parent)
    , userThemesRoot_(std::move(userThemesRoot))
    , storage_(std::move(storage))
    , currentThemeId_(builtinThemeId())
{
    loadOptions_.harkenVersion = QString::fromUtf8(
        Harken::BuildInfo::Version.data(),
        static_cast<int>(Harken::BuildInfo::Version.size()));
    qRegisterMetaType<ThemeSnapshotPtr>();
    auto initial = loadBaseTheme(currentThemeId_);
    Q_ASSERT(initial.ok());
    committedCandidate_ = *initial.candidate;
    currentPaletteId_ = committedCandidate_.defaultPaletteId;
    snapshot_ = committedCandidate_.snapshot;
    refreshThemes();

    ThemeAppearance restored = persistedAppearance();
    auto loaded = customizedCandidate(restored);
    if (!loaded.ok())
    {
        lastDiagnostics_ = loaded.diagnostics;
        if (loaded.candidate && restored.paletteId != loaded.candidate->defaultPaletteId)
        {
            restored.paletteId.clear();
            loaded = customizedCandidate(restored);
        }
        if (!loaded.ok() && restored.themeId != builtinThemeId())
        {
            restored.themeId = builtinThemeId();
            restored.paletteId.clear();
            loaded = customizedCandidate(restored);
        }
    }
    customizations_ = restored.colors;
    if (loaded.ok())
    {
        currentThemeId_ = restored.themeId;
        currentPaletteId_ = restored.paletteId;
        committedCandidate_ = *loaded.candidate;
    }
    publishCandidate(committedCandidate_, false);
}

ThemePackageDiscoveryResult ThemeManager::refreshThemes()
{
    ThemePackageDiscoveryResult result = discoverThemePackages(
        userThemesRoot_, loadOptions_);
    themes_ = result.themes;
    lastDiagnostics_ = result.diagnostics;
    return result;
}

ThemeManagerOperationResult ThemeManager::selectTheme(const QString& themeId)
{
    auto desired = appearance();
    desired.themeId = themeId;
    desired.paletteId.clear();
    return updateAppearance(desired, true);
}

ThemeManagerOperationResult ThemeManager::reloadTheme()
{
    ThemeManagerOperationResult result;
    if (!checkPublicationThread(result))
        return result;
    auto desired = appearance();
    auto loaded = customizedCandidate(desired);
    if (!loaded.ok())
    {
        result.diagnostics = std::move(loaded.diagnostics);
        lastDiagnostics_ = result.diagnostics;
        return result;
    }
    committedCandidate_ = *loaded.candidate;
    previewActive_ = false;
    previewBaseCandidate_.reset();
    publishCandidate(committedCandidate_, true, true);
    result.published = true;
    lastDiagnostics_.clear();
    return result;
}

ThemePackageLoadResult ThemeManager::loadBaseTheme(const QString& themeId) const
{
    return loadThemePackageCandidate(userThemesRoot_, themeId, loadOptions_);
}

QList<ColorPaletteDescriptor> ThemeManager::availablePalettes() const
{
    QList<ColorPaletteDescriptor> palettes;
    for (const auto& palette : committedCandidate_.palettes)
        palettes.append({palette.id, palette.displayName});
    return palettes;
}

ColorPaletteLoadResult ThemeManager::loadPalette(const QString& paletteId) const
{
    ColorPaletteLoadResult result;
    for (const auto& palette : committedCandidate_.palettes)
    {
        if (palette.id == paletteId)
        {
            result.palette = palette;
            return result;
        }
    }
    result.diagnostics.append({ThemePackageErrorCode::InvalidValue,
        QStringLiteral("settings"), QStringLiteral("Appearance.PaletteId"),
        QStringLiteral("The selected palette does not belong to this theme.")});
    return result;
}

bool ThemeManager::checkPublicationThread(ThemeManagerOperationResult& result) const
{
    if (QThread::currentThread() == thread())
        return true;
    result.diagnostics.append(ThemePackageDiagnostic{
        ThemePackageErrorCode::RuntimeThreadViolation,
        QStringLiteral("ThemeManager"), QStringLiteral("$"),
        QStringLiteral("Theme publication must run on the manager thread.")});
    return false;
}

ThemePackageLoadResult ThemeManager::customizedCandidate(
    ThemeAppearance& appearance, bool reusePreviewBase)
{
    ThemePackageLoadResult result;
    if (reusePreviewBase && previewBaseCandidate_
        && previewBaseCandidate_->manifest.id == appearance.themeId)
        result.candidate = previewBaseCandidate_;
    else
        result = loadBaseTheme(appearance.themeId);
    if (!result.ok())
        return result;

    if (appearance.paletteId.isEmpty())
        appearance.paletteId = result.candidate->defaultPaletteId;
    const ColorPalette* selected = nullptr;
    for (const auto& palette : result.candidate->palettes)
        if (palette.id == appearance.paletteId)
            selected = &palette;
    if (!selected)
    {
        result.diagnostics.append({ThemePackageErrorCode::InvalidValue,
            QStringLiteral("settings"), QStringLiteral("Appearance.PaletteId"),
            QStringLiteral("The selected palette does not belong to this theme.")});
        return result;
    }
    auto themeColors = appearance.colors.find(appearance.themeId);
    if (themeColors != appearance.colors.end() && themeColors->contains(QString()))
    {
        auto colors = themeColors->take(QString());
        const auto existing = themeColors->value(result.candidate->defaultPaletteId);
        for (auto color = existing.cbegin(); color != existing.cend(); ++color)
            colors.insert(color.key(), color.value());
        themeColors->insert(result.candidate->defaultPaletteId, colors);
    }
    for (auto theme = appearance.colors.cbegin(); theme != appearance.colors.cend(); ++theme)
    {
        for (auto palette = theme->cbegin(); palette != theme->cend(); ++palette)
        {
            for (auto color = palette->cbegin(); color != palette->cend(); ++color)
            {
                const bool active = theme.key() == appearance.themeId
                    && palette.key() == appearance.paletteId;
                const bool known = !active
                    || findThemeColorRole(result.candidate->colorRoles, color.key());
                if (known && color->isValid())
                    continue;
                result.diagnostics.append({known ? ThemePackageErrorCode::InvalidColor
                                                : ThemePackageErrorCode::UnknownSemanticKey,
                    QStringLiteral("settings"),
                    QStringLiteral("Appearance.ThemePaletteColors.%1.%2.%3")
                        .arg(theme.key(), palette.key(), color.key()),
                    known ? QStringLiteral("The custom color is invalid.")
                          : QStringLiteral("This theme does not define the custom color role.")});
            }
        }
    }
    if (!result.diagnostics.isEmpty())
        return result;
    if (reusePreviewBase)
        previewBaseCandidate_ = result.candidate;

    ThemeSnapshot customized = *selected->snapshot;
    auto colors = selected->colors;
    const auto overrides = appearance.colors.value(appearance.themeId).value(appearance.paletteId);
    for (auto color = overrides.cbegin(); color != overrides.cend(); ++color)
        colors.insert(color.key(), color.value());
    if (!resolveThemeColors(customized, result.candidate->colorRoles, colors))
    {
        result.diagnostics.append({ThemePackageErrorCode::UnknownSemanticKey,
            QStringLiteral("settings"), QStringLiteral("Appearance.ThemePaletteColors"),
            QStringLiteral("The theme color bindings could not be resolved.")});
        return result;
    }
    result.candidate->snapshot = std::make_shared<const ThemeSnapshot>(std::move(customized));
    return result;
}

ThemeManagerOperationResult ThemeManager::previewAppearance(const ThemeAppearance& appearance)
{
    return updateAppearance(appearance, false);
}

ThemeManagerOperationResult ThemeManager::applyAppearance(const ThemeAppearance& appearance)
{
    return updateAppearance(appearance, true);
}

ThemeManagerOperationResult ThemeManager::updateAppearance(
    const ThemeAppearance& appearance, bool persist)
{
    ThemeManagerOperationResult result;
    if (!checkPublicationThread(result))
        return result;
    auto desired = appearance;
    if (desired.themeId == currentThemeId_ && desired.paletteId.isEmpty())
        desired.paletteId = committedCandidate_.defaultPaletteId;
    if (persist && desired == this->appearance())
    {
        cancelAppearancePreview();
        lastDiagnostics_.clear();
        return result;
    }
    auto loaded = customizedCandidate(desired, !persist);
    if (!loaded.ok())
    {
        result.diagnostics = loaded.diagnostics;
        lastDiagnostics_ = result.diagnostics;
        return result;
    }
    if (persist && !persistAppearance(desired, result.diagnostics))
    {
        lastDiagnostics_ = result.diagnostics;
        return result;
    }
    if (persist)
    {
        currentThemeId_ = desired.themeId;
        currentPaletteId_ = desired.paletteId;
        customizations_ = desired.colors;
        committedCandidate_ = *loaded.candidate;
        previewBaseCandidate_.reset();
    }
    previewActive_ = !persist;
    publishCandidate(*loaded.candidate, true);
    result.published = true;
    lastDiagnostics_.clear();
    return result;
}

void ThemeManager::cancelAppearancePreview()
{
    if (!previewActive_ || QThread::currentThread() != thread())
        return;
    previewActive_ = false;
    previewBaseCandidate_.reset();
    publishCandidate(committedCandidate_, true);
    lastDiagnostics_.clear();
}

void ThemeManager::publishCandidate(const ThemePackageCandidate& candidate,
                                    bool notify, bool reloadAssets)
{
    const auto previous = snapshot_;
    const bool assetsChanged = reloadAssets || publishedThemeId_ != candidate.manifest.id
        || publishedAssets_ != candidate.assets || publishedAssetRoots_ != candidate.assetRoots;
    ThemeSnapshot structure = *candidate.snapshot;
    copyThemeColors(structure, *previous);
    structure.generation = previous->generation;
    const bool structureChanged = assetsChanged || structure != *previous;
    ThemeSnapshot published = *candidate.snapshot;
    published.generation = ++generation_;
    snapshot_ = std::make_shared<const ThemeSnapshot>(std::move(published));
    assetResolver_.registerSnapshot(snapshot_, candidate.assets,
                                    candidate.assetRoots, assetsChanged ? ThemeSnapshotPtr{} : previous);
    publishedThemeId_ = candidate.manifest.id;
    publishedAssets_ = candidate.assets;
    publishedAssetRoots_ = candidate.assetRoots;
    applyApplicationTheme(structureChanged);
    if (notify) {
        if (structureChanged) emit themeChanged(snapshot_);
        emit appearanceChanged(snapshot_);
    }
}

void ThemeManager::applyApplicationTheme(bool structureChanged)
{
    auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (!application)
        return;
    auto& fontManager = FontManager::instance();
    const QtControlSkin skin(snapshot_, &assetResolver_);
    application->styleHints()->setColorScheme(
        snapshot_->colorScheme == ThemeColorScheme::Light
            ? Qt::ColorScheme::Light
            : Qt::ColorScheme::Dark);
    bool refreshWidgetPalettes = structureChanged;
    const QString baseStyleSheet = skin.applicationStyleSheet();
    if (!baseStyleSheet.isNull())
    {
        const auto styleSheet = QtAdsThemeAdapter(snapshot_).composeApplicationStyleSheet(baseStyleSheet);
        refreshWidgetPalettes |= widgetAppearance_.updateStyleSheet(styleSheet);
        const auto& nativeStyleSheet = widgetAppearance_.nativeStyleSheet();
        if (application->styleSheet() != nativeStyleSheet) {
            application->setStyleSheet(nativeStyleSheet);
            refreshWidgetPalettes = true;
        }
    }
    const auto palette = skin.applicationPalette();
    if (application->palette() != palette) {
        application->setPalette(palette);
        refreshWidgetPalettes = true;
    }
    const auto font = fontManager.getFont(snapshot_, FontRole::Body);
    if (application->font() != font) application->setFont(font);
    if (structureChanged) QtAdsThemeAdapter::applyIcons(assetResolver_, snapshot_);
    fontManager.applyTheme(snapshot_);
    if (refreshWidgetPalettes)
        for (auto* widget : application->allWidgets())
            HarkenStyle::refreshWidgetAppearance(widget, *this);
    for (auto* window : application->topLevelWidgets()) window->update();
}

bool ThemeManager::persistAppearance(
    const ThemeAppearance& appearance,
    QList<ThemePackageDiagnostic>& diagnostics)
{
    if (!storage_ || !storage_->isWritable())
    {
        diagnostics.append(ThemePackageDiagnostic{
            ThemePackageErrorCode::PersistenceUnavailable,
            QStringLiteral("settings"), QStringLiteral("Appearance"),
            QStringLiteral("Appearance settings storage is not writable.")});
        return false;
    }
    storage_->beginGroup(QStringLiteral("Appearance"));
    storage_->setValue(QStringLiteral("ThemeId"), appearance.themeId);
    storage_->setValue(QStringLiteral("PaletteId"), appearance.paletteId);
    QVariantMap customizations;
    for (auto theme = appearance.colors.cbegin(); theme != appearance.colors.cend(); ++theme)
    {
        QVariantMap palettes;
        for (auto palette = theme->cbegin(); palette != theme->cend(); ++palette)
        {
            QVariantMap overrides;
            for (auto color = palette->cbegin(); color != palette->cend(); ++color)
                overrides.insert(color.key(), color->name(QColor::HexArgb));
            if (!overrides.isEmpty())
                palettes.insert(palette.key(), overrides);
        }
        if (!palettes.isEmpty())
            customizations.insert(theme.key(), palettes);
    }
    storage_->setValue(QStringLiteral("ThemePaletteColors"), customizations);
    storage_->remove(QStringLiteral("Colors"));
    storage_->remove(QStringLiteral("PaletteColors"));
    storage_->endGroup();
    storage_->sync();
    return true;
}

ThemeAppearance ThemeManager::persistedAppearance() const
{
    ThemeAppearance appearance{builtinThemeId(), {}, {}};
    if (!storage_)
        return appearance;
    const QString oldDefault = QStringLiteral("harken.builtin.dark");
    const QString oldAmber = QStringLiteral("harken.builtin.amber");
    const auto themeId = [&](const QString& id) {
        return id == oldDefault ? builtinThemeId() : id;
    };
    const auto readColors = [](const QVariantMap& values) {
        ThemeColorOverrides colors;
        for (auto color = values.cbegin(); color != values.cend(); ++color)
        {
            const QColor parsed(color->toString());
            if (parsed.isValid())
                colors.insert(color.key(), parsed);
        }
        return colors;
    };
    storage_->beginGroup(QStringLiteral("Appearance"));
    const QString id = storage_->value(QStringLiteral("ThemeId")).toString();
    if (!id.isEmpty() && id != oldAmber)
        appearance.themeId = themeId(id);
    appearance.paletteId = storage_->value(QStringLiteral("PaletteId")).toString();
    if (storage_->contains(QStringLiteral("ThemePaletteColors")))
    {
        const auto themes = storage_->value(QStringLiteral("ThemePaletteColors")).toMap();
        for (auto theme = themes.cbegin(); theme != themes.cend(); ++theme)
        {
            const auto palettes = theme->toMap();
            for (auto palette = palettes.cbegin(); palette != palettes.cend(); ++palette)
                appearance.colors[theme.key()].insert(palette.key(), readColors(palette->toMap()));
        }
    }
    else
    {
        const bool global = storage_->contains(QStringLiteral("PaletteColors"));
        const auto values = storage_->value(global ? QStringLiteral("PaletteColors")
                                                   : QStringLiteral("Colors")).toMap();
        for (auto item = values.cbegin(); item != values.cend(); ++item)
        {
            if (global && !item.key().startsWith(QStringLiteral("theme:")))
                continue;
            const QString owner = global ? item.key().mid(6) : item.key();
            if (owner == oldAmber)
                continue;
            const QString ownerId = themeId(owner);
            const auto loaded = loadBaseTheme(ownerId);
            const QString paletteId = loaded.ok() ? loaded.candidate->defaultPaletteId : QString();
            appearance.colors[ownerId].insert(paletteId, readColors(item->toMap()));
        }
        appearance.paletteId.clear();
    }
    storage_->endGroup();
    auto studioColors = appearance.colors.find(builtinThemeId());
    if (studioColors != appearance.colors.end())
    {
        for (auto& colors : *studioColors)
        {
            const auto muted = colors.find(QStringLiteral("text.muted"));
            if (muted == colors.end())
                continue;
            if (!colors.contains(QStringLiteral("text.secondary")))
                colors.insert(QStringLiteral("text.secondary"), *muted);
            colors.remove(QStringLiteral("text.muted"));
        }
    }
    return appearance;
}

void ThemeManager::paintBindAffordance(QPainter& p, const QRect& rect, BindGlyph,
                                       bool active, bool hover, bool enabled) const
{
    p.setRenderHint(QPainter::Antialiasing, true);

    const auto& themeColors = colors();
    QColor fg = !enabled ? themeColors.textDisabled
                         : (active ? themeColors.accent : themeColors.textMuted);
    if (hover && enabled)
        fg = active ? themeColors.accentHover : themeColors.textSecondary;

    if (hover && enabled) {
        QColor bg = themeColors.accent;
        bg.setAlpha(active ? 46 : 30);
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawRoundedRect(QRectF(rect).adjusted(1.0, 1.0, -1.0, -1.0), 4.0, 4.0);
    }

    const QRectF r(rect);
    const double s = 7.0;
    const double off = 2.0;
    const QPointF c = r.center();
    const QRectF back (c.x() - s / 2 - off, c.y() - s / 2 - off, s, s);
    const QRectF front(c.x() - s / 2 + off, c.y() - s / 2 + off, s, s);
    QPen pen(fg, 1.3);
    pen.setJoinStyle(Qt::RoundJoin);

    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(back, 1.6, 1.6);

    p.setPen(Qt::NoPen);
    p.setBrush(themeColors.bgPrimary);
    p.drawRoundedRect(front, 1.6, 1.6);
    if (active) {
        QColor fill = themeColors.accent;
        fill.setAlpha(hover ? 235 : 200);
        p.setBrush(fill);
        p.drawRoundedRect(front, 1.6, 1.6);
    }
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(front, 1.6, 1.6);
}

}
