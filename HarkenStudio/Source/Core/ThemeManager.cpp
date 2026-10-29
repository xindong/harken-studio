#include "ThemeManager.h"

#include "harken_buildinfo.h"
#include "BuiltinTheme.h"
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

    const QString restoredId = persistedThemeId();
    if (!restoredId.isEmpty() && restoredId != builtinThemeId())
    {
        ThemeManagerOperationResult restored = loadAndPublish(
            restoredId, false, true);
        if (!restored.ok())
        {
            lastDiagnostics_ = restored.diagnostics;
            QList<ThemePackageDiagnostic> ignored;
            persistThemeId(builtinThemeId(), ignored);
        }
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

ThemeManagerOperationResult ThemeManager::loadAndPublish(
    const QString& themeId,
    bool persistSelection,
    bool allowCurrentTheme)
{
    ThemeManagerOperationResult result;
    if (QThread::currentThread() != thread())
    {
        result.diagnostics.append(ThemePackageDiagnostic{
            ThemePackageErrorCode::RuntimeThreadViolation,
            QStringLiteral("ThemeManager"), QStringLiteral("$"),
            QStringLiteral("Theme publication must run on the manager thread.")});
        return result;
    }
    if (!allowCurrentTheme && themeId == currentThemeId_)
    {
        lastDiagnostics_.clear();
        return result;
    }

    ThemePackageLoadResult loaded = loadThemePackageCandidate(
        userThemesRoot_, themeId, loadOptions_);
    if (!loaded.ok())
    {
        result.diagnostics = std::move(loaded.diagnostics);
        lastDiagnostics_ = result.diagnostics;
        return result;
    }
    if (persistSelection
        && !persistThemeId(themeId, result.diagnostics))
    {
        lastDiagnostics_ = result.diagnostics;
        return result;
    }

    currentThemeId_ = themeId;
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

bool ThemeManager::persistThemeId(
    const QString& themeId,
    QList<ThemePackageDiagnostic>& diagnostics)
{
    if (!storage_ || !storage_->isWritable())
    {
        diagnostics.append(ThemePackageDiagnostic{
            ThemePackageErrorCode::PersistenceUnavailable,
            QStringLiteral("settings"), QStringLiteral("Appearance.ThemeId"),
            QStringLiteral("Theme selection storage is not writable.")});
        return false;
    }
    storage_->beginGroup(QStringLiteral("Appearance"));
    storage_->setValue(QStringLiteral("ThemeId"), themeId);
    storage_->endGroup();
    storage_->sync();
    return true;
}

QString ThemeManager::persistedThemeId() const
{
    if (!storage_)
        return {};
    storage_->beginGroup(QStringLiteral("Appearance"));
    const QString id = storage_->value(QStringLiteral("ThemeId")).toString();
    storage_->endGroup();
    return id;
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
