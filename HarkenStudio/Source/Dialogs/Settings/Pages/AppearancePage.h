#pragma once

#include "Core/ThemeManager.h"
#include "Dialogs/Settings/SettingsPage.h"

#include <QPointer>

class QComboBox;
class QHideEvent;
class QLabel;
class QPushButton;
class QShowEvent;
class QTimer;

namespace VNX {

class ColorEditorDialog;
class ProportionalTreeWidget;
class SearchField;
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
    bool commitAppearance(const ThemeAppearance& appearance);
    void refreshPresets(bool currentOnly = false);
    bool loadBasePalette();
    void refreshColors(const QString& changedKey = {});
    void filterColors();
    void refreshSelectedColor();
    const ThemeColorRole* selectedRole() const;
    QColor effectiveColor(const ThemeColorRole& role) const;
    ThemeAppearance appearanceWithColor(const QString& key, const QColor& color) const;
    bool setColor(const QString& key, const QColor& color);
    void editSelectedColor();
    void publishPreview();
    void resetSelectedColor();
    void resetColors();
    void cancelPreview();
    void showError(const QList<ThemePackageDiagnostic>& diagnostics);

    ThemeManager* themeManager_ = nullptr;
    QComboBox* themeCombo_ = nullptr;
    QComboBox* colorPresets_ = nullptr;
    QComboBox* languageCombo_ = nullptr;
    QComboBox* colorGroup_ = nullptr;
    SearchField* colorSearch_ = nullptr;
    ProportionalTreeWidget* colorTree_ = nullptr;
    QLabel* colorDescription_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QPushButton* colorReset_ = nullptr;
    QPushButton* colorsReset_ = nullptr;
    QPointer<ColorEditorDialog> colorDialog_;
    QTimer* previewTimer_ = nullptr;
    QString previewColorKey_;
    QColor pendingPreviewColor_;

    std::optional<ColorPalette> basePalette_;
    QMap<QString, ColorPalette> palettes_;
    ThemeAppearance appearance_;
    bool updatingAppearance_ = false;
    bool previewActive_ = false;
};

}
