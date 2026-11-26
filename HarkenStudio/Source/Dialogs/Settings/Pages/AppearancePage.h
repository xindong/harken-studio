#pragma once

#include "Core/ThemeManager.h"
#include "Dialogs/Settings/SettingsPage.h"

#include <QPointer>

class QComboBox;
class QDialog;
class QHideEvent;
class QLabel;
class QPushButton;
class QShowEvent;
class QSpinBox;
class QTimer;
class QTreeWidgetItem;

namespace harken::studio {

class ColorEditorDialog;
class MenuComboBox;
class ProportionalTreeWidget;
class SearchField;
class TableWidget;
struct ThemeColorRole;

inline constexpr const char* kSettingsPageAppearance = "appearance";

class AppearancePage : public SettingsPage
{
    Q_OBJECT

public:
    explicit AppearancePage(ThemeManager& themeManager, QWidget* parent = nullptr);
    ~AppearancePage() override;

    QString pageId() const override;
    QString categoryLabel() const override;
    void load() override;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void selectTheme();
    void selectColorPreset();
    void applyLanguage();
    void refreshLanguage();
    void refreshUIScale();
    bool commitAppearance(const ThemeAppearance& appearance);
    void refreshPresets();
    void rebuildColorRoles();
    bool loadBasePalette();
    void refreshColors();
    void filterColors();
    QColor effectiveColor(const ThemeColorRole& role) const;
    ThemeAppearance appearanceWithColor(const QString& key, const QColor& color) const;
    bool setColor(const QString& key, const QColor& color);
    void editColor(QTreeWidgetItem* item);
    void refreshColorActions();
    void publishPreview();
    void resetColors();
    void cancelPreview();
    void showError(const QList<ThemePackageDiagnostic>& diagnostics);
    void buildTypography(QWidget* section);
    void refreshTypography();
    void chooseSystemFont(FontRole role);
    void refreshTypographyReset();
    void buildMetrics(QWidget* section);
    void refreshMetrics();
    void refreshMetricActions();

    ThemeManager* themeManager_ = nullptr;
    QComboBox* themeCombo_ = nullptr;
    QComboBox* colorPresets_ = nullptr;
    QComboBox* languageCombo_ = nullptr;
    QComboBox* uiScaleCombo_ = nullptr;
    QComboBox* colorGroup_ = nullptr;
    SearchField* colorSearch_ = nullptr;
    ProportionalTreeWidget* colorTree_ = nullptr;
    QLabel* colorTableMessage_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QPushButton* colorReset_ = nullptr;
    QPushButton* colorsReset_ = nullptr;
    struct TypographyRow {
        MenuComboBox* family;
        QSpinBox* size;
        QSpinBox* weight;
    };
    QMap<QString, TypographyRow> fontRows_;
    TableWidget* typographyTable_ = nullptr;
    ProportionalTreeWidget* fontTree_ = nullptr;
    QPushButton* fontRoleReset_ = nullptr;
    QPushButton* typographyReset_ = nullptr;
    ProportionalTreeWidget* metricTree_ = nullptr;
    QPushButton* metricsResetSelected_ = nullptr;
    QPushButton* metricsReset_ = nullptr;
    QPointer<ColorEditorDialog> colorDialog_;
    QPointer<QDialog> systemFontDialog_;
    QTimer* previewTimer_ = nullptr;
    QString previewColorKey_;
    QColor pendingPreviewColor_;

    QVector<ThemeColorRole> colorRoles_;
    std::optional<ColorPalette> basePalette_;
    QMap<QString, ColorPalette> palettes_;
    ThemeAppearance appearance_;
    qreal uiScale_ = 1.0;
    bool updatingAppearance_ = false;
    bool previewActive_ = false;
};

}
