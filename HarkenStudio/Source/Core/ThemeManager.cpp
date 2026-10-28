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
    if (restored.themeId != builtinThemeId() || !restored.colors.isEmpty())
    {
        auto loaded = customizedCandidate(restored);
        if (!loaded.ok() && restored.themeId != builtinThemeId())
        {
            lastDiagnostics_ = loaded.diagnostics;
            restored.themeId = builtinThemeId();
            loaded = customizedCandidate(restored);
        }
        if (loaded.ok())
        {
            currentThemeId_ = restored.themeId;
            customizations_ = restored.colors;
            committedCandidate_ = *loaded.candidate;
            publishCandidate(committedCandidate_, false);
        }
        else
            lastDiagnostics_ = loaded.diagnostics;
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
    return loadAndPublish(themeId, true, false);
}

ThemeManagerOperationResult ThemeManager::reloadTheme()
{
    return loadAndPublish(currentThemeId_, false, true);
}

ThemePackageLoadResult ThemeManager::loadBaseTheme(const QString& themeId) const
{
    return loadThemePackageCandidate(userThemesRoot_, themeId, loadOptions_);
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
    const ThemeAppearance& appearance) const
{
    ThemePackageLoadResult result;
    for (auto theme = appearance.colors.cbegin(); theme != appearance.colors.cend(); ++theme)
    {
        for (auto color = theme->cbegin(); color != theme->cend(); ++color)
        {
            const bool known = findThemeColorRole(color.key()) != nullptr;
            if (known && color->isValid())
                continue;
            result.diagnostics.append(ThemePackageDiagnostic{
                known ? ThemePackageErrorCode::InvalidValue
                      : ThemePackageErrorCode::UnknownSemanticKey,
                QStringLiteral("settings"),
                QStringLiteral("Appearance.Colors.%1.%2").arg(theme.key(), color.key()),
                known ? QStringLiteral("The custom color is invalid.")
                      : QStringLiteral("This color role cannot be customized.")});
        }
    }
    if (!result.diagnostics.isEmpty())
        return result;

    result = loadBaseTheme(appearance.themeId);
    if (!result.ok())
        return result;
    const auto overrides = appearance.colors.value(appearance.themeId);
    if (overrides.isEmpty())
        return result;
    ThemeSnapshot customized = *result.candidate->snapshot;
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
    if (persist && appearance == this->appearance())
    {
        cancelAppearancePreview();
        lastDiagnostics_.clear();
        return result;
    }
    auto loaded = customizedCandidate(appearance);
    if (!loaded.ok())
    {
        result.diagnostics = loaded.diagnostics;
        lastDiagnostics_ = result.diagnostics;
        return result;
    }
    if (persist && !persistAppearance(appearance, result.diagnostics))
    {
        lastDiagnostics_ = result.diagnostics;
        return result;
    }
    if (persist)
    {
        currentThemeId_ = appearance.themeId;
        customizations_ = appearance.colors;
        committedCandidate_ = *loaded.candidate;
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
    publishCandidate(committedCandidate_, true);
    lastDiagnostics_.clear();
}

ThemeManagerOperationResult ThemeManager::loadAndPublish(
    const QString& themeId,
    bool persistSelection,
    bool allowCurrentTheme)
{
    ThemeManagerOperationResult result;
    if (!checkPublicationThread(result))
        return result;
    if (!allowCurrentTheme && themeId == currentThemeId_)
    {
        cancelAppearancePreview();
        lastDiagnostics_.clear();
        return result;
    }

    const ThemeAppearance desired{themeId, customizations_};
    ThemePackageLoadResult loaded = customizedCandidate(desired);
    if (!loaded.ok())
    {
        result.diagnostics = std::move(loaded.diagnostics);
        lastDiagnostics_ = result.diagnostics;
        return result;
    }
    if (persistSelection
        && !persistAppearance(desired, result.diagnostics))
    {
        lastDiagnostics_ = result.diagnostics;
        return result;
    }

    currentThemeId_ = themeId;
    committedCandidate_ = *loaded.candidate;
    previewActive_ = false;
    publishCandidate(*loaded.candidate, true);
    result.published = true;
    lastDiagnostics_.clear();
    return result;
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
    QVariantMap customizations;
    for (auto theme = appearance.colors.cbegin(); theme != appearance.colors.cend(); ++theme)
    {
        QVariantMap overrides;
        for (auto color = theme->cbegin(); color != theme->cend(); ++color)
            overrides.insert(color.key(), color->name(QColor::HexArgb));
        if (!overrides.isEmpty())
            customizations.insert(theme.key(), overrides);
    }
    storage_->setValue(QStringLiteral("Colors"), customizations);
    storage_->endGroup();
    storage_->sync();
    return true;
}

ThemeAppearance ThemeManager::persistedAppearance() const
{
    ThemeAppearance appearance{builtinThemeId(), {}};
    if (!storage_)
        return appearance;
    storage_->beginGroup(QStringLiteral("Appearance"));
    const QString id = storage_->value(QStringLiteral("ThemeId")).toString();
    if (!id.isEmpty())
        appearance.themeId = id;
    const QVariantMap customizations = storage_->value(QStringLiteral("Colors")).toMap();
    for (auto theme = customizations.cbegin(); theme != customizations.cend(); ++theme)
    {
        const QVariantMap overrides = theme->toMap();
        for (auto color = overrides.cbegin(); color != overrides.cend(); ++color)
            appearance.colors[theme.key()].insert(color.key(), QColor(color->toString()));
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
