#pragma once

#include "Core/ThemeManager.h"
#include "Dialogs/Settings/SettingsPage.h"

#include <QPointer>

class QColorDialog;
class QComboBox;
class QHideEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QShowEvent;
class QTreeWidgetItem;

namespace VNX {

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
    void applyLanguage();
    void refreshLanguage();
    bool commitAppearance(const ThemeAppearance& appearance);
    void refreshPresets();
    bool loadBaseTheme();
    void refreshColors();
    void filterColors();
    void refreshSelectedColor();
    const ThemeColorRole* selectedRole() const;
    QColor effectiveColor(const ThemeColorRole& role) const;
    ThemeAppearance appearanceWithColor(const QString& key, const QColor& color) const;
    bool setColor(const QString& key, const QColor& color);
    void editSelectedColor();
    bool finishHexEdit();
    void resetSelectedColor();
    void resetColorGroup();
    void resetColors();
    void cancelPreview();
    void showError(const QList<ThemePackageDiagnostic>& diagnostics);

    ThemeManager* themeManager_ = nullptr;
    QListWidget* themePresets_ = nullptr;
    QComboBox* languageCombo_ = nullptr;
    QComboBox* colorGroup_ = nullptr;
    SearchField* colorSearch_ = nullptr;
    ProportionalTreeWidget* colorTree_ = nullptr;
    QLabel* colorDescription_ = nullptr;
    QLabel* colorSummary_ = nullptr;
    QLabel* errorLabel_ = nullptr;
    QLineEdit* colorHex_ = nullptr;
    QPushButton* colorEdit_ = nullptr;
    QPushButton* colorReset_ = nullptr;
    QPushButton* colorGroupReset_ = nullptr;
    QPushButton* colorsReset_ = nullptr;
    QPointer<QColorDialog> colorDialog_;

    ThemeSnapshotPtr baseTheme_;
    QMap<QString, ThemeSnapshotPtr> presetThemes_;
    ThemeAppearance appearance_;
    QString hexRoleKey_;
    QString filterGroup_;
    QString filterQuery_;
    bool updatingAppearance_ = false;
    bool previewActive_ = false;
};

}
