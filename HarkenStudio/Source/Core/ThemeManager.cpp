#include "ThemeManager.h"

#include "harken_buildinfo.h"
#include "BuiltinTheme.h"
#include "ThemeColorResolver.h"
#include "FontManager.h"
#include "QtAdsThemeAdapter.h"
#include "QtControlSkin.h"
#include "QSettingsStorage.h"

#include <QApplication>
#include <QDir>
#include <QPainter>
#include <QPixmapCache>
#include <QRect>
#include <QStandardPaths>
#include <QStyleHints>
#include <QThread>

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
    , currentPaletteId_(defaultPaletteId(currentThemeId_))
{
    loadOptions_.harkenVersion = QString::fromUtf8(
        Harken::BuildInfo::Version.data(),
        static_cast<int>(Harken::BuildInfo::Version.size()));
    qRegisterMetaType<ThemeSnapshotPtr>();
    ThemeSnapshot initial = createBuiltinDarkTheme();
    initial.generation = ++generation_;
    snapshot_ = std::make_shared<const ThemeSnapshot>(std::move(initial));
    assetResolver_.registerSnapshot(snapshot_, {}, {});
    refreshThemes();

    committedCandidate_.snapshot = snapshot_;
    committedCandidate_.manifest.id = currentThemeId_;
    ThemeAppearance restored = persistedAppearance();
    auto loaded = customizedCandidate(restored);
    if (!loaded.ok())
    {
        lastDiagnostics_ = loaded.diagnostics;
        if (!loaded.candidate && restored.themeId != builtinThemeId())
        {
            restored.themeId = builtinThemeId();
            loaded = customizedCandidate(restored);
        }
        if (!loaded.ok() && restored.paletteId != defaultPaletteId(restored.themeId))
        {
            restored.paletteId = defaultPaletteId(restored.themeId);
            loaded = customizedCandidate(restored);
        }
    }
    if (loaded.ok())
    {
        currentThemeId_ = restored.themeId;
        currentPaletteId_ = restored.paletteId;
        customizations_ = restored.colors;
        committedCandidate_ = *loaded.candidate;
        publishCandidate(committedCandidate_, false);
    }
    applyApplicationTheme();
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
    return updateAppearance(desired, true);
}

ThemeManagerOperationResult ThemeManager::reloadTheme()
{
    ThemeManagerOperationResult result;
    if (!checkPublicationThread(result))
        return result;
    auto loaded = customizedCandidate(appearance());
    if (!loaded.ok())
    {
        result.diagnostics = std::move(loaded.diagnostics);
        lastDiagnostics_ = result.diagnostics;
        return result;
    }
    committedCandidate_ = *loaded.candidate;
    previewActive_ = false;
    previewBaseCandidate_.reset();
    previewBasePalette_.reset();
    publishCandidate(committedCandidate_, true);
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
    for (const auto& theme : themes_)
        palettes.append({defaultPaletteId(theme.id), theme.displayName});
    const auto amber = createBuiltinAmberPalette();
    palettes.append({amber.id, amber.displayName});
    return palettes;
}

ColorPaletteLoadResult ThemeManager::loadPalette(const QString& paletteId) const
{
    ColorPaletteLoadResult result;
    if (paletteId == builtinAmberPaletteId())
    {
        result.palette = createBuiltinAmberPalette();
        return result;
    }
    const QString prefix = QStringLiteral("theme:");
    if (!paletteId.startsWith(prefix) || paletteId.size() == prefix.size())
    {
        result.diagnostics.append({ThemePackageErrorCode::InvalidValue,
            QStringLiteral("settings"), QStringLiteral("Appearance.PaletteId"),
            QStringLiteral("The selected color palette is unknown.")});
        return result;
    }
    const QString themeId = paletteId.mid(prefix.size());
    const auto loaded = loadBaseTheme(themeId);
    if (!loaded.ok())
    {
        result.diagnostics = loaded.diagnostics;
        return result;
    }
    result.palette = colorPaletteFromTheme(themeId, loaded.candidate->manifest.displayName,
                                          *loaded.candidate->snapshot);
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
    const ThemeAppearance& appearance, bool reusePreviewBase)
{
    ThemePackageLoadResult result;
    for (auto palette = appearance.colors.cbegin(); palette != appearance.colors.cend(); ++palette)
    {
        for (auto color = palette->cbegin(); color != palette->cend(); ++color)
        {
            const bool known = findThemeColorRole(color.key()) != nullptr;
            if (known && color->isValid())
                continue;
            result.diagnostics.append(ThemePackageDiagnostic{
                known ? ThemePackageErrorCode::InvalidValue
                      : ThemePackageErrorCode::UnknownSemanticKey,
                QStringLiteral("settings"),
                QStringLiteral("Appearance.PaletteColors.%1.%2").arg(palette.key(), color.key()),
                known ? QStringLiteral("The custom color is invalid.")
                      : QStringLiteral("This color role cannot be customized.")});
        }
    }
    if (!result.diagnostics.isEmpty())
        return result;

    if (reusePreviewBase && previewBaseCandidate_
        && previewBaseCandidate_->manifest.id == appearance.themeId)
        result.candidate = previewBaseCandidate_;
    else
        result = loadBaseTheme(appearance.themeId);
    if (!result.ok())
        return result;

    ColorPaletteLoadResult palette;
    if (reusePreviewBase && previewBasePalette_
        && previewBasePalette_->id == appearance.paletteId)
        palette.palette = previewBasePalette_;
    else if (appearance.paletteId == defaultPaletteId(appearance.themeId))
        palette.palette = colorPaletteFromTheme(appearance.themeId,
            result.candidate->manifest.displayName, *result.candidate->snapshot);
    else
        palette = loadPalette(appearance.paletteId);
    if (!palette.ok())
    {
        result.diagnostics = palette.diagnostics;
        return result;
    }
    if (reusePreviewBase)
    {
        previewBaseCandidate_ = result.candidate;
        previewBasePalette_ = palette.palette;
    }

    ThemeSnapshot customized = *result.candidate->snapshot;
    applyColorPalette(customized, *palette.palette);
    const auto overrides = appearance.colors.value(appearance.paletteId);
    for (auto color = overrides.cbegin(); color != overrides.cend(); ++color)
        findThemeColorRole(color.key())->write(customized, color.value());
    resolveThemeColors(customized);
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
    if (desired.themeId != currentThemeId_ || desired.paletteId.isEmpty())
        desired.paletteId = defaultPaletteId(desired.themeId);
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
        previewBasePalette_.reset();
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
    previewBasePalette_.reset();
    publishCandidate(committedCandidate_, true);
    lastDiagnostics_.clear();
}

void ThemeManager::publishCandidate(const ThemePackageCandidate& candidate,
                                    bool notify)
{
    ThemeSnapshot published = *candidate.snapshot;
    published.generation = ++generation_;
    snapshot_ = std::make_shared<const ThemeSnapshot>(std::move(published));
    assetResolver_.registerSnapshot(snapshot_, candidate.assets,
                                    candidate.assetRoots);
    applyApplicationTheme();
    if (notify)
        emit themeChanged(snapshot_);
}

void ThemeManager::applyApplicationTheme()
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
    application->setPalette(skin.applicationPalette());
    const QString baseStyleSheet = skin.applicationStyleSheet();
    if (!baseStyleSheet.isNull())
    {
        application->setStyleSheet(
            QtAdsThemeAdapter(snapshot_)
                .composeApplicationStyleSheet(baseStyleSheet));
    }
    application->setFont(fontManager.getFont(snapshot_, FontRole::Body));
    QtAdsThemeAdapter::applyIcons(assetResolver_, snapshot_);
    QPixmapCache::clear();
    fontManager.applyTheme(snapshot_);
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
    for (auto palette = appearance.colors.cbegin(); palette != appearance.colors.cend(); ++palette)
    {
        QVariantMap overrides;
        for (auto color = palette->cbegin(); color != palette->cend(); ++color)
            overrides.insert(color.key(), color->name(QColor::HexArgb));
        if (!overrides.isEmpty())
            customizations.insert(palette.key(), overrides);
    }
    storage_->setValue(QStringLiteral("PaletteColors"), customizations);
    storage_->remove(QStringLiteral("Colors"));
    storage_->endGroup();
    storage_->sync();
    return true;
}

ThemeAppearance ThemeManager::persistedAppearance() const
{
    ThemeAppearance appearance{builtinThemeId(), defaultPaletteId(builtinThemeId()), {}};
    if (!storage_)
        return appearance;
    storage_->beginGroup(QStringLiteral("Appearance"));
    const QString id = storage_->value(QStringLiteral("ThemeId")).toString();
    if (!id.isEmpty())
        appearance.themeId = id;
    const bool legacy = !storage_->contains(QStringLiteral("PaletteColors"));
    const QString legacyAmberId = QStringLiteral("harken.builtin.amber");
    appearance.paletteId = storage_->value(QStringLiteral("PaletteId")).toString();
    if (legacy && appearance.themeId == legacyAmberId)
    {
        appearance.themeId = builtinThemeId();
        appearance.paletteId = builtinAmberPaletteId();
    }
    if (appearance.paletteId.isEmpty())
        appearance.paletteId = defaultPaletteId(appearance.themeId);
    const QVariantMap customizations = storage_->value(
        legacy ? QStringLiteral("Colors") : QStringLiteral("PaletteColors")).toMap();
    for (auto palette = customizations.cbegin(); palette != customizations.cend(); ++palette)
    {
        QString paletteId = palette.key();
        if (legacy)
            paletteId = paletteId == legacyAmberId
                ? builtinAmberPaletteId() : defaultPaletteId(paletteId);
        const QVariantMap overrides = palette->toMap();
        for (auto color = overrides.cbegin(); color != overrides.cend(); ++color)
            appearance.colors[paletteId].insert(color.key(), QColor(color->toString()));
    }
    storage_->endGroup();
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
