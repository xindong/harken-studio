#include "ThemeManager.h"

#include "harken_buildinfo.h"
#include "BuiltinTheme.h"
#include "AppSettings.h"
#include "ThemeScale.h"
#include "ThemeMetricsDerivation.h"
#include "ThemeColorResolver.h"
#include "FontManager.h"
#include "ThemeFontAssets.h"
#include "HarkenStyle.h"
#include "QtAdsThemeAdapter.h"
#include "QtControlSkin.h"
#include "QSettingsStorage.h"

#include <QApplication>
#include <QDir>
#include <QJsonValue>
#include <QPainter>
#include <QRect>
#include <QStandardPaths>
#include <QStyleHints>
#include <QThread>
#include <QToolTip>
#include <QWidget>

#include <algorithm>
#include <utility>

namespace harken::studio {
namespace {

const QStringList retiredHoverRoles{QStringLiteral("accent.hover"), QStringLiteral("background.hover")};

void writeStyleOverrides(ISettingsStorage& storage, const ThemeAppearance& appearance)
{
    QVariantMap metrics;
    for (const auto& [owner, values] : appearance.metrics.asKeyValueRange())
    {
        QVariantMap entries;
        for (const auto& [key, value] : values.asKeyValueRange())
            entries.insert(key, value);
        if (!entries.isEmpty()) metrics.insert(owner, entries);
    }
    QVariantMap fonts;
    for (const auto& [owner, values] : appearance.fonts.asKeyValueRange())
    {
        QVariantMap entries;
        for (const auto& [key, font] : values.asKeyValueRange())
        {
            QVariantMap fields;
            if (font.family) {
                fields.insert(QStringLiteral("family"), *font.family);
                fields.insert(QStringLiteral("source"), font.source == ThemeFontSource::Theme
                    ? QStringLiteral("theme") : QStringLiteral("system"));
            }
            if (font.pointSize) fields.insert(QStringLiteral("pointSize"), *font.pointSize);
            if (font.weight) fields.insert(QStringLiteral("weight"), *font.weight);
            if (!fields.isEmpty()) entries.insert(key, fields);
        }
        if (!entries.isEmpty()) fonts.insert(owner, entries);
    }
    storage.setValue(QStringLiteral("ThemeMetrics"), metrics);
    storage.setValue(QStringLiteral("ThemeFonts"), fonts);
}

void readStyleOverrides(ISettingsStorage& storage, ThemeAppearance& appearance)
{
    const auto metrics = storage.value(QStringLiteral("ThemeMetrics")).toMap();
    for (const auto& [owner, values] : metrics.asKeyValueRange())
    {
        const auto entries = values.toMap();
        for (const auto& [key, value] : entries.asKeyValueRange())
        {
            const auto* token = findThemeMetricToken(key);
            const int number = QJsonValue::fromVariant(value).toInt(-1);
            if (token && token->accepts(number))
                appearance.metrics[owner].insert(key, number);
        }
    }
    const auto fonts = storage.value(QStringLiteral("ThemeFonts")).toMap();
    for (const auto& [owner, values] : fonts.asKeyValueRange())
    {
        const auto entries = values.toMap();
        for (const auto& [key, value] : entries.asKeyValueRange())
        {
            if (!findThemeFontToken(key))
                continue;
            const auto fields = value.toMap();
            ThemeFontOverride font;
            const auto family = fields.value(QStringLiteral("family"));
            const auto source = fields.value(QStringLiteral("source"), QStringLiteral("system")).toString();
            const auto fontSource = source == QStringLiteral("theme") ? ThemeFontSource::Theme : ThemeFontSource::System;
            if (family.metaType().id() == QMetaType::QString
                && (source == QStringLiteral("system") || source == QStringLiteral("theme"))
                && ThemeFontOverride::acceptsFamily(family.toString(), fontSource)) {
                font.family = family.toString();
                font.source = fontSource;
            }
            const int size = QJsonValue::fromVariant(fields.value(QStringLiteral("pointSize"))).toInt(-1);
            if (size >= ThemeFontOverride::minimumPointSize && size <= ThemeFontOverride::maximumPointSize)
                font.pointSize = size;
            const int weight = QJsonValue::fromVariant(fields.value(QStringLiteral("weight"))).toInt(-1);
            if (weight >= ThemeFontOverride::minimumWeight && weight <= ThemeFontOverride::maximumWeight)
                font.weight = weight;
            if (!font.empty())
                appearance.fonts[owner].insert(key, std::move(font));
        }
    }
}

bool applyStyleOverrides(ThemeAppearance& appearance, ThemeSnapshot& snapshot,
                         QList<ThemePackageDiagnostic>& diagnostics)
{
    for (const auto& [owner, values] : std::as_const(appearance.metrics).asKeyValueRange())
    {
        for (const auto& [key, value] : values.asKeyValueRange())
        {
            const auto* token = findThemeMetricToken(key);
            if (!token || !token->accepts(value))
            {
                diagnostics.append({token ? ThemePackageErrorCode::MetricOutOfRange
                                          : ThemePackageErrorCode::UnknownSemanticKey,
                    QStringLiteral("settings"), QStringLiteral("Appearance.ThemeMetrics.%1.%2").arg(owner, key),
                    QStringLiteral("Invalid theme metric override.")});
                return false;
            }
            if (owner == appearance.themeId)
                token->setValue(snapshot.ui.metrics, value);
        }
    }
    for (auto& values : appearance.fonts) {
        values.removeIf([](auto it) { return it.value().empty(); });
        for (auto& fields : values) {
            if (fields.family && !fields.source)
                fields.source = ThemeFontSource::System;
        }
    }
    appearance.fonts.removeIf([](auto it) { return it.value().isEmpty(); });
    appearance.metrics.removeIf([](auto it) { return it.value().isEmpty(); });
    std::optional<QStringList> installedFamilies;
    for (const auto& [owner, values] : std::as_const(appearance.fonts).asKeyValueRange())
    {
        for (const auto& [key, font] : values.asKeyValueRange())
        {
            const auto* token = findThemeFontToken(key);
            const char* field = font.invalidField();
            if (!token || field)
            {
                QString path = QStringLiteral("Appearance.ThemeFonts.%1.%2").arg(owner, key);
                if (field) path += QLatin1Char('.') + QLatin1StringView(field);
                diagnostics.append({token ? ThemePackageErrorCode::InvalidValue
                                          : ThemePackageErrorCode::UnknownSemanticKey,
                    QStringLiteral("settings"), path, QStringLiteral("Invalid font override.")});
                return false;
            }
            if (owner == appearance.themeId) {
                auto effective = font;
                if (font.family && !font.family->isEmpty()) {
                    bool available;
                    if (font.source == ThemeFontSource::Theme) {
                        available = std::any_of(snapshot.fontAssets.cbegin(), snapshot.fontAssets.cend(),
                            [&](const auto& asset) { return asset->name.compare(*font.family, Qt::CaseInsensitive) == 0; });
                    } else {
                        if (!installedFamilies) installedFamilies = installedSystemFontFamilies();
                        available = installedFamilies->contains(*font.family, Qt::CaseInsensitive);
                    }
                    if (!available) effective.family.reset();
                }
                effective.applyTo(snapshot.ui.typography.*token->member);
            }
        }
    }
    if (appearance.metrics.contains(appearance.themeId))
        deriveThemeMetrics(snapshot);
    return true;
}

}

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
{
    loadOptions_.harkenVersion = QString::fromUtf8(
        Harken::BuildInfo::Version.data(),
        static_cast<int>(Harken::BuildInfo::Version.size()));
    qRegisterMetaType<ThemeSnapshotPtr>();
    committedAppearance_.themeId = builtinThemeId();
    auto initial = loadBaseTheme(committedAppearance_.themeId);
    Q_ASSERT(initial.ok());
    committedCandidate_ = *initial.candidate;
    committedAppearance_.paletteId = committedCandidate_.defaultPaletteId;
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
    if (loaded.ok())
        committedCandidate_ = *loaded.candidate;
    else
    {
        restored.themeId = committedAppearance_.themeId;
        restored.paletteId = committedAppearance_.paletteId;
    }
    committedAppearance_ = std::move(restored);
    uiScalePercent_ = AppSettings::instance().globalUIScale();
    publishCandidate(committedCandidate_, false);
    connect(&AppSettings::instance(), &AppSettings::globalUIScaleChanged,
            this, &ThemeManager::setUIScale);
}

void ThemeManager::setUIScale(qreal percent)
{
    Q_ASSERT(QThread::currentThread() == thread());
    percent = qBound(AppSettings::uiScaleForZoomLevel(AppSettings::MIN_GLOBAL_ZOOM_LEVEL),
        percent, AppSettings::uiScaleForZoomLevel(AppSettings::MAX_GLOBAL_ZOOM_LEVEL));
    if (uiScalePercent_ == percent)
        return;
    uiScalePercent_ = percent;
    publishCandidate(publishedCandidate_, true);
}

ThemePackageDiscoveryResult ThemeManager::refreshThemes()
{
    ThemePackageDiscoveryResult result = discoverThemePackages(
        userThemesRoot_, loadOptions_);
    themes_ = result.themes;
    lastDiagnostics_ = result.diagnostics;
    return result;
}

QMetaObject::Connection ThemeManager::bindMetrics(QObject* context, std::function<void(const ThemeMetrics&)> apply)
{
    apply(metrics());
    return connect(this, &ThemeManager::themeChanged, context,
            [apply = std::move(apply)](const ThemeSnapshotPtr& snapshot) {
                apply(snapshot->ui.metrics);
            });
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
    ThemeAppearance& appearance, bool reusePreviewBase, const ThemePackageCandidate* previous)
{
    ThemePackageLoadResult result;
    if (reusePreviewBase && previewBaseCandidate_
        && previewBaseCandidate_->manifest.id == appearance.themeId)
        result.candidate = previewBaseCandidate_;
    else
        result = loadBaseTheme(appearance.themeId);
    if (!result.ok())
        return result;

    if (!prepareThemeFonts(*result.candidate, result.diagnostics, previous))
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
    for (auto [owner, palettes] : appearance.colors.asKeyValueRange())
    {
        const ThemePackageCandidate* base = &*result.candidate;
        ThemePackageLoadResult other;
        if (owner != appearance.themeId)
        {
            if (palettes == committedAppearance_.colors.value(owner) ||
                std::none_of(palettes.cbegin(), palettes.cend(), [](const auto& colors) {
                    const auto color = colors.constFind(QStringLiteral("accent.secondary"));
                    return color != colors.cend() && color->alphaF() != 1.0f;
                }))
                continue;
            other = loadBaseTheme(owner);
            if (!other.ok())
                continue;
            base = &*other.candidate;
        }
        for (const auto& palette : base->palettes)
        {
            auto overrides = palettes.find(palette.id);
            if (overrides == palettes.end())
                continue;
            if (base->usesStudioDefaultColorRoles && owner == appearance.themeId)
            {
                const QString info = QStringLiteral("status.info");
                const QString secondary = QStringLiteral("accent.secondary");
                const auto committed = committedAppearance_.colors.value(owner);
                auto saved = palette.id == base->defaultPaletteId ? committed.value(QString())
                                                                 : ThemeColorOverrides{};
                saved.insert(committed.value(palette.id));
                for (const auto& key : retiredHoverRoles)
                    if (saved.contains(key) && overrides->value(key) == saved.value(key))
                        overrides->remove(key);
                const auto oldInfo = saved.constFind(info);
                if (oldInfo != saved.cend() && overrides->value(info) == *oldInfo)
                {
                    if (!overrides->contains(secondary))
                        overrides->insert(secondary, *oldInfo);
                    overrides->remove(info);
                }
                const auto prior = saved.value(secondary, saved.value(info));
                if (prior.isValid() && prior.alphaF() != 1.0f && overrides->value(secondary) == prior)
                    normalizeOpaqueThemeColors(base->colorRoles, *overrides, palette.snapshot->ui.colors.bgPrimary);
            }
            normalizeOpaqueThemeColors(base->colorRoles, *overrides);
        }
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
    ThemeSnapshot customized = *selected->snapshot;
    if (!applyStyleOverrides(appearance, customized, result.diagnostics))
        return result;
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
    if (reusePreviewBase)
        previewBaseCandidate_ = result.candidate;
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
    if (desired.themeId == committedAppearance_.themeId && desired.paletteId.isEmpty())
        desired.paletteId = committedCandidate_.defaultPaletteId;
    if (persist && desired == this->appearance())
    {
        cancelAppearancePreview();
        lastDiagnostics_.clear();
        return result;
    }
    auto loaded = customizedCandidate(desired, !persist, &publishedCandidate_);
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
        committedAppearance_ = desired;
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
        || publishedCandidate_.preparedFonts != candidate.preparedFonts
        || publishedAssets_ != candidate.assets || publishedAssetRoots_ != candidate.assetRoots;
    ThemeSnapshot published = scaledThemeSnapshot(*candidate.snapshot, uiScalePercent_ / 100.0);
    ThemeSnapshot structure = published;
    copyThemeColors(structure, *previous);
    structure.generation = previous->generation;
    const bool structureChanged = assetsChanged || structure != *previous;
    published.generation = ++generation_;
    snapshot_ = std::make_shared<const ThemeSnapshot>(std::move(published));
    publishedCandidate_ = candidate;
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
    const auto font = fontManager.getFont(snapshot_, FontRole::Body);
    if (application->font() != font) application->setFont(font);
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
    application->setFont(font);
    QToolTip::setFont(fontManager.getFont(snapshot_, FontRole::Caption));
    if (structureChanged) QtAdsThemeAdapter::applyIcons(assetResolver_, snapshot_);
    fontManager.applyTheme(snapshot_);
    for (auto* widget : application->allWidgets()) {
        if (refreshWidgetPalettes)
            HarkenStyle::refreshWidgetAppearance(widget, *this);
        widget->update();
    }
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
    storage_->setValue(QStringLiteral("ThemePaletteColors"), storedPaletteColors(appearance));
    writeStyleOverrides(*storage_, appearance);
    storage_->remove(QStringLiteral("Colors"));
    storage_->remove(QStringLiteral("PaletteColors"));
    storage_->endGroup();
    storage_->sync();
    return true;
}

QVariantMap ThemeManager::storedPaletteColors(const ThemeAppearance& appearance) const
{
    QVariantMap customizations;
    for (auto theme = appearance.colors.cbegin(); theme != appearance.colors.cend(); ++theme)
    {
        const QString secondary = QStringLiteral("accent.secondary");
        bool inherited = theme.key() == builtinThemeId();
        if (!inherited && std::any_of(theme->cbegin(), theme->cend(), [&](const auto& entries) {
                return entries.contains(secondary);
            }))
        {
            const auto loaded = loadBaseTheme(theme.key());
            inherited = loaded.ok() && loaded.candidate->usesStudioDefaultColorRoles;
        }
        QVariantMap palettes;
        for (auto palette = theme->cbegin(); palette != theme->cend(); ++palette)
        {
            QVariantMap overrides;
            for (auto color = palette->cbegin(); color != palette->cend(); ++color)
                overrides.insert(color.key(), color->name(inherited && color.key() == secondary
                    && color->alphaF() == 1.0f ? QColor::HexRgb : QColor::HexArgb));
            if (!overrides.isEmpty())
                palettes.insert(palette.key(), overrides);
        }
        if (!palettes.isEmpty())
            customizations.insert(theme.key(), palettes);
    }
    return customizations;
}

ThemeAppearance ThemeManager::persistedAppearance() const
{
    ThemeAppearance appearance;
    appearance.themeId = builtinThemeId();
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
    readStyleOverrides(*storage_, appearance);
    const bool currentFormat = storage_->contains(QStringLiteral("ThemePaletteColors"));
    const auto storedThemes = storage_->value(QStringLiteral("ThemePaletteColors")).toMap();
    if (currentFormat)
    {
        for (auto theme = storedThemes.cbegin(); theme != storedThemes.cend(); ++theme)
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
    const QString retiredInfo = QStringLiteral("status.info");
    const QString secondary = QStringLiteral("accent.secondary");
    bool normalized = false;
    for (auto [owner, palettes] : appearance.colors.asKeyValueRange())
    {
        if (std::none_of(palettes.cbegin(), palettes.cend(), [&](const auto& colors) {
                return colors.contains(retiredInfo) || colors.contains(secondary)
                    || std::any_of(retiredHoverRoles.cbegin(), retiredHoverRoles.cend(),
                                   [&](const QString& key) { return colors.contains(key); });
            }))
            continue;
        const auto base = loadBaseTheme(owner);
        if (!base.ok() || !base.candidate->usesStudioDefaultColorRoles)
            continue;
        for (auto& colors : palettes)
        {
            for (const auto& key : retiredHoverRoles)
                if (colors.remove(key))
                    normalized = true;
            auto info = colors.find(retiredInfo);
            if (info == colors.end())
                continue;
            if (!colors.contains(secondary))
                colors.insert(secondary, *info);
            colors.remove(retiredInfo);
            normalized = true;
        }
        for (const auto& palette : base.candidate->palettes)
        {
            auto overrides = palettes.find(palette.id);
            if (overrides == palettes.end() || !overrides->contains(secondary))
                continue;
            normalizeOpaqueThemeColors(base.candidate->colorRoles, *overrides, palette.snapshot->ui.colors.bgPrimary);
            const auto stored = storedThemes.value(owner).toMap().value(palette.id).toMap();
            if (stored.value(secondary).toString() != overrides->value(secondary).name(QColor::HexRgb))
                normalized = true;
        }
    }
    auto studioColors = appearance.colors.find(builtinThemeId());
    if (studioColors != appearance.colors.end())
    {
        for (auto& colors : *studioColors)
        {
            colors.remove(QStringLiteral("knob.indicator"));
            colors.remove(QStringLiteral("meter.lufsMomentary"));
            colors.remove(QStringLiteral("meter.lufsShortTerm"));
            colors.remove(QStringLiteral("meter.lufsIntegrated"));
            colors.remove(QStringLiteral("bus.background"));
            colors.remove(QStringLiteral("timeline.clipBorder"));
            colors.remove(QStringLiteral("timeline.clipSelectedBorder"));
            const auto view = colors.find(QStringLiteral("border.view"));
            if (view != colors.end())
            {
                if (!colors.contains(QStringLiteral("border.tick")))
                    colors.insert(QStringLiteral("border.tick"), *view);
                colors.remove(QStringLiteral("border.view"));
            }
            const auto muted = colors.find(QStringLiteral("text.muted"));
            if (muted == colors.end())
                continue;
            if (!colors.contains(QStringLiteral("text.secondary")))
                colors.insert(QStringLiteral("text.secondary"), *muted);
            colors.remove(QStringLiteral("text.muted"));
        }
    }
    if (normalized && storage_->isWritable())
    {
        storage_->setValue(QStringLiteral("Appearance/ThemePaletteColors"), storedPaletteColors(appearance));
        if (!currentFormat)
            storage_->remove(QStringLiteral("Appearance/PaletteId"));
        storage_->remove(QStringLiteral("Appearance/Colors"));
        storage_->remove(QStringLiteral("Appearance/PaletteColors"));
        storage_->sync();
    }
    return appearance;
}

void ThemeManager::paintBindAffordance(QPainter& p, const QRect& rect, BindGlyph,
                                       bool active, bool hover, bool enabled) const
{
    p.setRenderHint(QPainter::Antialiasing, true);

    const auto& themeColors = colors();
    const auto& geometry = metrics();
    const QColor fg = !enabled ? themeColors.textDisabled
                               : (active ? themeColors.accent : themeColors.textPrimary);

    if (hover && enabled) {
        p.setPen(Qt::NoPen);
        p.setBrush(themeColors.hoverOverlay);
        const qreal inset = geometry.borderWidth;
        p.drawRoundedRect(QRectF(rect).adjusted(inset, inset, -inset, -inset),
                          geometry.radiusSmall, geometry.radiusSmall);
    }

    const QRectF r(rect);
    const double s = geometry.control.smallIconExtent * 7.0 / 16.0;
    const double off = geometry.control.smallIconExtent / 8.0;
    const double radius = geometry.control.smallIconExtent / 10.0;
    const QPointF c = r.center();
    const QRectF back (c.x() - s / 2 - off, c.y() - s / 2 - off, s, s);
    const QRectF front(c.x() - s / 2 + off, c.y() - s / 2 + off, s, s);
    QPen pen(fg, 1.3 * snapshot_->uiScale);
    pen.setJoinStyle(Qt::RoundJoin);

    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(back, radius, radius);

    p.setPen(Qt::NoPen);
    p.setBrush(themeColors.bgPrimary);
    p.drawRoundedRect(front, radius, radius);
    if (active) {
        QColor fill = themeColors.accent;
        fill.setAlphaF(fill.alphaF() * 200 / 255.0);
        p.setBrush(fill);
        p.drawRoundedRect(front, radius, radius);
    }
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(front, radius, radius);
}

}
