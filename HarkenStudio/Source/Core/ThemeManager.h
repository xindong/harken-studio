
#pragma once

#include "ISettingsStorage.h"
#include "ThemeAssetResolver.h"
#include "ThemePackageLoader.h"
#include "ThemeSnapshot.h"

#include <QColor>
#include <QObject>
#include <QString>

#include <memory>

class QPainter;
class QRect;

namespace VNX {

struct ThemeManagerOperationResult
{
    bool published = false;
    QList<ThemePackageDiagnostic> diagnostics;

    bool ok() const { return diagnostics.isEmpty(); }
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
    QString userThemesRoot() const { return userThemesRoot_; }
    const QList<ThemePackageDescriptor>& availableThemes() const { return themes_; }
    const QList<ThemePackageDiagnostic>& lastDiagnostics() const
    {
        return lastDiagnostics_;
    }
    ThemeAssetResolver& assetResolver() { return assetResolver_; }
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
    const ThemeColors& colors() const { return snapshot_->ui.colors; }
    const ThemeMetrics& metrics() const { return snapshot_->ui.metrics; }

    QString accent() const { return colors().accent.name(); }
    QString accentHover() const { return colors().accentHover.name(); }
    QString bgPrimary() const { return colors().bgPrimary.name(); }
    QString bgSecondary() const { return colors().bgSecondary.name(); }
    QString bgTertiary() const { return colors().bgTertiary.name(); }
    QString bgHover() const { return colors().bgHover.name(); }
    QString bgSelected() const
    {
        const QColor& color = colors().bgSelected;
        return QString("rgba(%1, %2, %3, %4)")
            .arg(color.red())
            .arg(color.green())
            .arg(color.blue())
            .arg(color.alpha());
    }
    QString borderPrimary() const { return colors().borderPrimary.name(); }
    QString borderSecondary() const { return colors().borderSecondary.name(); }
    QString viewBorder() const { return colors().viewBorder.name(); }
    QString textPrimary() const { return colors().textPrimary.name(); }
    QString textSecondary() const { return colors().textSecondary.name(); }
    QString textMuted() const { return colors().textMuted.name(); }
    QString textDisabled() const { return colors().textDisabled.name(); }
    enum class BindGlyph { StateLayers = 1 };
    void paintBindAffordance(QPainter& p, const QRect& rect, BindGlyph glyph,
                             bool active, bool hover, bool enabled) const;

signals:
    void themeChanged(VNX::ThemeSnapshotPtr snapshot);

private:
    explicit ThemeManager(QObject* parent = nullptr);

    ThemeManagerOperationResult loadAndPublish(const QString& themeId,
                                               bool persistSelection,
                                               bool allowCurrentTheme);
    void publishCandidate(const ThemePackageCandidate& candidate, bool notify);
    void applyApplicationTheme();
    bool persistThemeId(const QString& themeId,
                        QList<ThemePackageDiagnostic>& diagnostics);
    QString persistedThemeId() const;

    QString userThemesRoot_;
    std::unique_ptr<ISettingsStorage> storage_;
    ThemePackageLoadOptions loadOptions_;
    ThemeAssetResolver assetResolver_;
    ThemeSnapshotPtr snapshot_;
    QString currentThemeId_;
    QList<ThemePackageDescriptor> themes_;
    QList<ThemePackageDiagnostic> lastDiagnostics_;
    quint64 generation_ = 0;
};

}
