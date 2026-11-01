
#pragma once

#include "ColorPalette.h"
#include "ISettingsStorage.h"
#include "ThemeAssetResolver.h"
#include "ThemeColorCatalog.h"
#include "ThemePackageLoader.h"
#include "ThemeSnapshot.h"
#include "WidgetAppearance.h"

#include <QColor>
#include <QObject>
#include <QString>

#include <memory>
#include <optional>

class QPainter;
class QRect;

namespace VNX {

struct ThemeManagerOperationResult
{
    bool published = false;
    QList<ThemePackageDiagnostic> diagnostics;

    bool ok() const { return diagnostics.isEmpty(); }
};

struct ThemeAppearance
{
    QString themeId;
    QString paletteId;
    QMap<QString, ColorPaletteCustomizations> colors;

    bool operator==(const ThemeAppearance&) const = default;
};

struct ColorPaletteLoadResult
{
    std::optional<ColorPalette> palette;
    QList<ThemePackageDiagnostic> diagnostics;

    bool ok() const { return palette.has_value() && diagnostics.isEmpty(); }
};

class ThemeManager : public QObject
{
    Q_OBJECT

public:
    static ThemeManager& instance();

    ThemeManager(QString userThemesRoot,
                 std::unique_ptr<ISettingsStorage> storage,
                 QObject* parent = nullptr);
    ~ThemeManager() override = default;

    ThemeManager(const ThemeManager&) = delete;
    ThemeManager& operator=(const ThemeManager&) = delete;

    ThemeSnapshotPtr snapshot() const { return snapshot_; }
    QString currentThemeId() const { return currentThemeId_; }
    QString currentPaletteId() const { return currentPaletteId_; }
    QString userThemesRoot() const { return userThemesRoot_; }
    const QList<ThemePackageDescriptor>& availableThemes() const { return themes_; }
    QList<ColorPaletteDescriptor> availablePalettes() const;
    const QVector<ThemeColorRole>& colorRoles() const { return committedCandidate_.colorRoles; }
    const QList<ThemePackageDiagnostic>& lastDiagnostics() const
    {
        return lastDiagnostics_;
    }
    ThemeAssetResolver& assetResolver() { return assetResolver_; }
    WidgetAppearance& widgetAppearance() { return widgetAppearance_; }
    const WidgetAppearance& widgetAppearance() const { return widgetAppearance_; }
    ThemeIconResolution resolveIcon(
        const QString& logicalId,
        const ThemeAssetVariant& variant = {})
    {
        return assetResolver_.resolveIcon(snapshot_, logicalId, variant);
    }
    ThemeIconResolution resolveToggleIcon(
        const QString& onLogicalId,
        const QString& offLogicalId,
        const ThemeAssetVariant& variant = {})
    {
        return assetResolver_.resolveToggleIcon(
            snapshot_, onLogicalId, offLogicalId, variant);
    }
    ThemeImageResolution resolveImage(
        const QString& logicalId,
        const ThemeAssetVariant& variant = {})
    {
        return assetResolver_.resolveImage(snapshot_, logicalId, variant);
    }

    ThemePackageDiscoveryResult refreshThemes();
    ThemeManagerOperationResult selectTheme(const QString& themeId);
    ThemeManagerOperationResult reloadTheme();
    ThemeAppearance appearance() const { return {currentThemeId_, currentPaletteId_, customizations_}; }
    ThemePackageLoadResult loadBaseTheme(const QString& themeId) const;
    ColorPaletteLoadResult loadPalette(const QString& paletteId) const;
    ThemeManagerOperationResult previewAppearance(const ThemeAppearance& appearance);
    ThemeManagerOperationResult applyAppearance(const ThemeAppearance& appearance);
    void cancelAppearancePreview();
    const ThemeColors& colors() const { return snapshot_->ui.colors; }
    const ThemeMetrics& metrics() const { return snapshot_->ui.metrics; }

    QString accent() const { return colors().accent.name(QColor::HexArgb); }
    QString accentHover() const { return colors().accentHover.name(QColor::HexArgb); }
    QString bgPrimary() const { return colors().bgPrimary.name(QColor::HexArgb); }
    QString bgSecondary() const { return colors().bgSecondary.name(QColor::HexArgb); }
    QString bgTertiary() const { return colors().bgTertiary.name(QColor::HexArgb); }
    QString bgHover() const { return colors().bgHover.name(QColor::HexArgb); }
    QString bgSelected() const
    {
        const QColor& color = colors().bgSelected;
        return QString("rgba(%1, %2, %3, %4)")
            .arg(color.red())
            .arg(color.green())
            .arg(color.blue())
            .arg(color.alpha());
    }
    QString borderPrimary() const { return colors().borderPrimary.name(QColor::HexArgb); }
    QString borderSecondary() const { return colors().borderSecondary.name(QColor::HexArgb); }
    QString viewBorder() const { return colors().viewBorder.name(QColor::HexArgb); }
    QString textPrimary() const { return colors().textPrimary.name(QColor::HexArgb); }
    QString textSecondary() const { return colors().textSecondary.name(QColor::HexArgb); }
    QString textMuted() const { return colors().textMuted.name(QColor::HexArgb); }
    QString textDisabled() const { return colors().textDisabled.name(QColor::HexArgb); }
    enum class BindGlyph { StateLayers = 1 };
    void paintBindAffordance(QPainter& p, const QRect& rect, BindGlyph glyph,
                             bool active, bool hover, bool enabled) const;

signals:
    void themeChanged(VNX::ThemeSnapshotPtr snapshot);
    void appearanceChanged(VNX::ThemeSnapshotPtr snapshot);

private:
    explicit ThemeManager(QObject* parent = nullptr);

    void publishCandidate(const ThemePackageCandidate& candidate, bool notify,
                          bool reloadAssets = false);
    ThemeManagerOperationResult updateAppearance(const ThemeAppearance& appearance,
                                                  bool persist);
    ThemePackageLoadResult customizedCandidate(ThemeAppearance& appearance,
                                               bool reusePreviewBase = false);
    bool checkPublicationThread(ThemeManagerOperationResult& result) const;
    void applyApplicationTheme(bool structureChanged);
    bool persistAppearance(const ThemeAppearance& appearance,
                           QList<ThemePackageDiagnostic>& diagnostics);
    ThemeAppearance persistedAppearance() const;

    QString userThemesRoot_;
    std::unique_ptr<ISettingsStorage> storage_;
    ThemePackageLoadOptions loadOptions_;
    ThemeAssetResolver assetResolver_;
    WidgetAppearance widgetAppearance_;
    ThemeSnapshotPtr snapshot_;
    QString currentThemeId_;
    QString currentPaletteId_;
    QMap<QString, ColorPaletteCustomizations> customizations_;
    ThemePackageCandidate committedCandidate_;
    QString publishedThemeId_;
    ThemeAssetMapDto publishedAssets_;
    QMap<QString, QString> publishedAssetRoots_;
    std::optional<ThemePackageCandidate> previewBaseCandidate_;
    bool previewActive_ = false;
    QList<ThemePackageDescriptor> themes_;
    QList<ThemePackageDiagnostic> lastDiagnostics_;
    quint64 generation_ = 0;
};

}
