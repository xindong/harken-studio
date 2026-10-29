#include "Dialogs/Settings/Pages/AppearancePage.h"

#include "Core/AppSettings.h"
#include "Core/ContextInfo.h"
#include "Core/ThemeColorCatalog.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/SearchField.h"
#include "Widgets/Common/TableItemDelegate.h"
#include "Widgets/Common/TableWidget.h"

#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QShowEvent>
#include <QSignalBlocker>

namespace VNX {

namespace {

QString colorText(const QColor& color)
{
    return color.name(color.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb).toUpper();
}

QIcon colorIcon(const QColor& color, const QPalette& palette)
{
    QPixmap swatch(24, 16);
    swatch.fill(palette.color(QPalette::Base));
    QPainter painter(&swatch);
    for (int y = 0; y < swatch.height(); y += 4)
        for (int x = 0; x < swatch.width(); x += 4)
            if ((x / 4 + y / 4) % 2 == 0)
                painter.fillRect(x, y, 4, 4, palette.color(QPalette::AlternateBase));
    painter.fillRect(swatch.rect(), color);
    painter.setPen(palette.color(QPalette::Mid));
    painter.drawRect(swatch.rect().adjusted(0, 0, -1, -1));
    QIcon icon;
    for (QIcon::Mode mode : {QIcon::Normal, QIcon::Active, QIcon::Selected, QIcon::Disabled})
        icon.addPixmap(swatch, mode);
    return icon;
}

QString roleText(const char* text)
{
    return QCoreApplication::translate("ThemeColorCatalog", text);
}

QIcon presetIcon(const ThemeSnapshot& theme, const ThemeColorOverrides& overrides)
{
    const QStringList keys{QStringLiteral("background.primary"),
        QStringLiteral("background.secondary"), QStringLiteral("background.selected"),
        QStringLiteral("accent.primary"), QStringLiteral("text.primary"),
        QStringLiteral("text.secondary"), QStringLiteral("status.success"),
        QStringLiteral("status.warning"), QStringLiteral("status.error"),
        QStringLiteral("status.info")};
    QPixmap swatch(190, 44);
    QColor background = overrides.value(keys.first(), theme.ui.colors.bgPrimary);
    background.setAlpha(255);
    swatch.fill(background);
    QPainter painter(&swatch);
    for (int i = 0; i < keys.size(); ++i) {
        const auto* role = findThemeColorRole(keys.at(i));
        const QColor color = overrides.value(keys.at(i), role->read(theme));
        const int left = i * swatch.width() / keys.size();
        const int right = (i + 1) * swatch.width() / keys.size();
        painter.fillRect(left, 0, right - left, swatch.height(), color);
    }
    QIcon icon;
    for (QIcon::Mode mode : {QIcon::Normal, QIcon::Active, QIcon::Selected, QIcon::Disabled})
        icon.addPixmap(swatch, mode);
    return icon;
}

}

AppearancePage::AppearancePage(ThemeManager& themeManager, QWidget* parent)
    : SettingsPage(parent)
    , themeManager_(&themeManager)
{
    auto* body = makePageBody();
    body->setContentsMargins(24, 16, 24, 16);
    body->setSpacing(12);
    auto* appearance = makeSection(tr("Color Presets"));
    auto* appearanceForm = makeForm();
    themePresets_ = new QListWidget(appearance);
    themePresets_->setObjectName(QStringLiteral("appearanceThemePresets"));
    themePresets_->setAccessibleName(tr("Color preset"));
    themePresets_->setViewMode(QListView::IconMode);
    themePresets_->setFlow(QListView::LeftToRight);
    themePresets_->setWrapping(false);
    themePresets_->setMovement(QListView::Static);
    themePresets_->setResizeMode(QListView::Adjust);
    themePresets_->setWordWrap(true);
    themePresets_->setSelectionMode(QAbstractItemView::SingleSelection);
    themePresets_->setIconSize(QSize(190, 44));
    themePresets_->setGridSize(QSize(220, 92));
    themePresets_->setSpacing(4);
    themePresets_->setFixedHeight(116);
    themePresets_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    const auto themeTopic = ContextTopic::forPanel(QStringLiteral("settings.appearance.theme"),
        {QDir::toNativeSeparators(themeManager_->userThemesRoot())});
    ContextInfo::tag(themePresets_, themeTopic);
    static_cast<QVBoxLayout*>(appearance->layout())->addWidget(themePresets_);

    auto* languageLabel = new QLabel(tr("Information Language:"));
    languageCombo_ = new QComboBox();
    languageCombo_->setObjectName(QStringLiteral("appearanceLanguageCombo"));
    const auto languageTopic = ContextTopic::forPanel(QStringLiteral("settings.appearance.language"));
    ContextInfo::tag(languageCombo_, languageTopic);
    ContextInfo::tag(languageLabel, languageTopic);
    languageCombo_->addItem(tr("English"), QStringLiteral("en"));
    languageCombo_->addItem(tr("Chinese (Simplified)"), QStringLiteral("zh-CN"));
    languageLabel->setBuddy(languageCombo_);
    appearanceForm->addRow(languageLabel, languageCombo_);
    static_cast<QVBoxLayout*>(appearance->layout())->addLayout(appearanceForm);
    body->addWidget(appearance);

    auto* colors = makeSection(tr("Colors"));
    auto* colorsLayout = static_cast<QVBoxLayout*>(colors->layout());
    colorsLayout->setSpacing(8);
    auto* filters = new QHBoxLayout();
    colorGroup_ = new QComboBox(colors);
    colorGroup_->setObjectName(QStringLiteral("appearanceColorGroup"));
    colorGroup_->addItem(tr("All categories"), QString());
    for (const auto& role : themeColorRoles()) {
        const QString group = QString::fromLatin1(role.group);
        if (colorGroup_->findData(group) < 0)
            colorGroup_->addItem(roleText(role.group), group);
    }
    colorSearch_ = new SearchField(colors);
    colorSearch_->setObjectName(QStringLiteral("appearanceColorSearch"));
    colorSearch_->setPlaceholderText(tr("Search colors and uses..."));
    filters->addWidget(colorGroup_);
    filters->addWidget(colorSearch_, 1);
    colorsLayout->addLayout(filters);

    auto* table = new TableWidget(TableOptions::display(), colors, themeManager_);
    table->setColumnsMovable(false);
    table->setFixedHeight(260);
    colorTree_ = table->createItemView(QStringLiteral("appearanceColorTree"),
        {tr("Role"), tr("Color"), tr("Preset Default")}, {46, 27, 27});
    colorTree_->setRootIsDecorated(false);
    colorTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    colorTree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    colorTree_->setAllColumnsShowFocus(true);
    colorTree_->setIconSize(QSize(24, 16));
    for (const auto& role : themeColorRoles()) {
        auto* item = new QTreeWidgetItem(colorTree_);
        item->setData(0, Qt::UserRole, QString::fromLatin1(role.key));
        item->setText(0, roleText(role.label));
        item->setToolTip(0, roleText(role.description));
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    }
    colorsLayout->addWidget(table);

    colorDescription_ = new QLabel(colors);
    colorDescription_->setObjectName(QStringLiteral("appearanceColorDescription"));
    colorDescription_->setProperty("textRole", QStringLiteral("secondary"));
    colorDescription_->setWordWrap(true);
    colorDescription_->setMinimumHeight(34);
    colorsLayout->addWidget(colorDescription_);

    auto* actions = new QHBoxLayout();
    colorReset_ = new QPushButton(tr("Reset Selected"), colors);
    colorReset_->setObjectName(QStringLiteral("appearanceColorReset"));
    colorsReset_ = new QPushButton(tr("Reset All"), colors);
    colorsReset_->setObjectName(QStringLiteral("appearanceColorsReset"));
    colorsReset_->setToolTip(tr("Restore all colors of the selected preset to its defaults."));
    actions->addStretch();
    actions->addWidget(colorReset_);
    actions->addWidget(colorsReset_);
    colorsLayout->addLayout(actions);

    colors->setToolTip(tr("Custom colors are saved separately for each preset. Completed edits are saved immediately."));
    errorLabel_ = new QLabel(colors);
    errorLabel_->setObjectName(QStringLiteral("appearanceColorError"));
    errorLabel_->setProperty("textRole", QStringLiteral("error"));
    errorLabel_->setWordWrap(true);
    errorLabel_->hide();
    colorsLayout->addWidget(errorLabel_);
    body->addWidget(colors);
    body->addStretch();

    connect(themePresets_, &QListWidget::currentItemChanged,
            this, &AppearancePage::selectTheme);
    connect(languageCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AppearancePage::applyLanguage);
    connect(colorGroup_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AppearancePage::filterColors);
    connect(colorSearch_, &QLineEdit::textChanged, this, &AppearancePage::filterColors);
    connect(colorTree_, &QTreeWidget::currentItemChanged,
            this, &AppearancePage::refreshSelectedColor);
    connect(colorTree_, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem*, int) { editSelectedColor(); });
    connect(colorReset_, &QPushButton::clicked, this, &AppearancePage::resetSelectedColor);
    connect(colorsReset_, &QPushButton::clicked, this, &AppearancePage::resetColors);
    connect(themeManager_, &ThemeManager::themeChanged, this, [this] {
        if (updatingAppearance_) return;
        if (colorDialog_) colorDialog_->reject();
        load();
    });
    connect(&AppSettings::instance(), &AppSettings::languageChanged,
            this, &AppearancePage::refreshLanguage);
}

AppearancePage::~AppearancePage()
{
    if (colorDialog_) disconnect(colorDialog_, nullptr, this, nullptr);
    cancelPreview();
}

QString AppearancePage::pageId() const
{
    return QString::fromLatin1(kSettingsPageAppearance);
}

QString AppearancePage::categoryLabel() const
{
    return tr("Appearance");
}

void AppearancePage::load()
{
    if (updatingAppearance_ || colorDialog_) return;
    themeManager_->refreshThemes();
    appearance_ = themeManager_->appearance();
    {
        const QSignalBlocker blocker(themePresets_);
        themePresets_->clear();
        presetThemes_.clear();
        QListWidgetItem* selected = nullptr;
        for (const ThemePackageDescriptor& theme : themeManager_->availableThemes()) {
            auto* item = new QListWidgetItem(themePresets_);
            item->setData(Qt::UserRole, theme.id);
            item->setData(Qt::UserRole + 1, theme.displayName);
            item->setTextAlignment(Qt::AlignHCenter);
            const auto loaded = themeManager_->loadBaseTheme(theme.id);
            if (loaded.ok()) presetThemes_.insert(theme.id, loaded.candidate->snapshot);
            if (theme.id == appearance_.themeId) selected = item;
        }
        if (!selected) {
            selected = new QListWidgetItem(themePresets_);
            selected->setData(Qt::UserRole, appearance_.themeId);
            selected->setData(Qt::UserRole + 1, appearance_.themeId);
            selected->setTextAlignment(Qt::AlignHCenter);
        }
        themePresets_->setCurrentItem(selected);
        themePresets_->scrollToItem(selected);
    }
    refreshLanguage();
    loadBaseTheme();
    themePresets_->scrollToItem(themePresets_->currentItem());
}

void AppearancePage::showEvent(QShowEvent* event)
{
    SettingsPage::showEvent(event);
    load();
}

void AppearancePage::hideEvent(QHideEvent* event)
{
    if (colorDialog_) colorDialog_->reject();
    SettingsPage::hideEvent(event);
}

void AppearancePage::applyLanguage()
{
    const QString language = languageCombo_->currentData().toString();
    if (!language.isEmpty()) AppSettings::instance().setLanguage(language);
}

void AppearancePage::refreshLanguage()
{
    const QSignalBlocker blocker(languageCombo_);
    const int index = languageCombo_->findData(AppSettings::instance().language());
    languageCombo_->setCurrentIndex(index < 0 ? 0 : index);
}

void AppearancePage::selectTheme()
{
    if (!themePresets_->currentItem()) return;
    const QString themeId = themePresets_->currentItem()->data(Qt::UserRole).toString();
    auto next = themeManager_->appearance();
    next.themeId = themeId;
    commitAppearance(next);
}

bool AppearancePage::commitAppearance(const ThemeAppearance& appearance)
{
    ThemeManagerOperationResult result;
    {
        const QScopedValueRollback guard(updatingAppearance_, true);
        result = themeManager_->applyAppearance(appearance);
        if (!result.ok()) themeManager_->cancelAppearancePreview();
        previewActive_ = false;
    }
    load();
    showError(result.diagnostics);
    return result.ok();
}

bool AppearancePage::loadBaseTheme()
{
    const ThemePackageLoadResult loaded = themeManager_->loadBaseTheme(appearance_.themeId);
    baseTheme_ = loaded.ok() ? loaded.candidate->snapshot : ThemeSnapshotPtr{};
    if (baseTheme_) presetThemes_.insert(appearance_.themeId, baseTheme_);
    else presetThemes_.remove(appearance_.themeId);
    showError(loaded.diagnostics);
    refreshColors();
    return loaded.ok();
}

void AppearancePage::refreshPresets()
{
    for (int i = 0; i < themePresets_->count(); ++i) {
        auto* item = themePresets_->item(i);
        const QString id = item->data(Qt::UserRole).toString();
        const QString name = item->data(Qt::UserRole + 1).toString();
        const auto base = presetThemes_.value(id);
        const auto overrides = appearance_.colors.value(id);
        item->setText(name + QLatin1Char('\n')
            + (!base ? tr("Unavailable") : overrides.isEmpty() ? tr("Default colors") : tr("Customized")));
        item->setIcon(base ? presetIcon(*base, overrides) : QIcon());
        item->setToolTip(tr("%1\nMain background · Controls · Selection · Accent · Primary text\nSecondary text · Success · Warning · Error · Information\n%2 custom colors")
            .arg(name).arg(overrides.size()));
    }
}

QColor AppearancePage::effectiveColor(const ThemeColorRole& role) const
{
    return appearance_.colors.value(appearance_.themeId).value(QString::fromLatin1(role.key), role.read(*baseTheme_));
}

void AppearancePage::refreshColors()
{
    refreshPresets();
    colorTree_->setEnabled(bool(baseTheme_));
    colorGroup_->setEnabled(bool(baseTheme_));
    colorSearch_->setEnabled(bool(baseTheme_));
    colorsReset_->setEnabled(baseTheme_ && !appearance_.colors.value(appearance_.themeId).isEmpty());
    for (int i = 0; i < colorTree_->topLevelItemCount(); ++i) {
        auto* item = colorTree_->topLevelItem(i);
        const auto* role = findThemeColorRole(item->data(0, Qt::UserRole).toString());
        if (!baseTheme_) {
            item->setText(1, QString());
            item->setText(2, QString());
            item->setIcon(1, QIcon());
            item->setIcon(2, QIcon());
            continue;
        }
        const QColor value = effectiveColor(*role);
        const QColor original = role->read(*baseTheme_);
        item->setText(1, colorText(value));
        item->setText(2, colorText(original));
        item->setIcon(1, colorIcon(value, palette()));
        item->setIcon(2, colorIcon(original, palette()));
        item->setToolTip(1, value == original ? tr("Preset default") : tr("Customized for this preset"));
        item->setData(1, TableTextRole, static_cast<int>(
            value == original ? TableTextState::Normal : TableTextState::Custom));
    }
    filterColors();
}

void AppearancePage::filterColors()
{
    const QString group = colorGroup_->currentData().toString();
    const QString query = colorSearch_->text().trimmed();
    QTreeWidgetItem* first = nullptr;
    for (int i = 0; i < colorTree_->topLevelItemCount(); ++i) {
        auto* item = colorTree_->topLevelItem(i);
        const auto* role = findThemeColorRole(item->data(0, Qt::UserRole).toString());
        const QString searchable = roleText(role->label) + QLatin1Char(' ')
            + roleText(role->group) + QLatin1Char(' ') + roleText(role->description)
            + QLatin1Char(' ') + QString::fromLatin1(role->key);
        const bool matches = (group.isEmpty() || group == QLatin1String(role->group))
            && searchable.contains(query, Qt::CaseInsensitive);
        item->setHidden(!matches);
        if (matches && !first) first = item;
    }
    if (!colorTree_->currentItem() || colorTree_->currentItem()->isHidden())
        colorTree_->setCurrentItem(first);
    refreshSelectedColor();
}

const ThemeColorRole* AppearancePage::selectedRole() const
{
    auto* item = colorTree_->currentItem();
    return item && !item->isHidden() && baseTheme_
        ? findThemeColorRole(item->data(0, Qt::UserRole).toString()) : nullptr;
}

void AppearancePage::refreshSelectedColor()
{
    const auto* role = selectedRole();
    colorReset_->setEnabled(role && appearance_.colors.value(appearance_.themeId).contains(QString::fromLatin1(role->key)));
    colorDescription_->setText(role ? roleText(role->description)
        : baseTheme_ ? tr("No matching colors.") : tr("Theme colors are unavailable."));
}

ThemeAppearance AppearancePage::appearanceWithColor(const QString& key, const QColor& color) const
{
    auto next = themeManager_->appearance();
    const auto* role = findThemeColorRole(key);
    if (color == role->read(*baseTheme_)) {
        auto overrides = next.colors.find(next.themeId);
        if (overrides != next.colors.end()) {
            overrides->remove(key);
            if (overrides->isEmpty()) next.colors.erase(overrides);
        }
    } else {
        next.colors[next.themeId].insert(key, color);
    }
    return next;
}

bool AppearancePage::setColor(const QString& key, const QColor& color)
{
    if (!findThemeColorRole(key) || !baseTheme_ || !color.isValid()) return false;
    return commitAppearance(appearanceWithColor(key, color));
}

void AppearancePage::editSelectedColor()
{
    if (colorDialog_) return;
    const auto* role = selectedRole();
    if (!role) return;
    const QString key = QString::fromLatin1(role->key);
    auto* dialog = new QColorDialog(effectiveColor(*role), this);
    colorDialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("appearanceColorDialog"));
    dialog->setWindowTitle(tr("Color: %1").arg(roleText(role->label)));
    dialog->setOptions(QColorDialog::DontUseNativeDialog | QColorDialog::ShowAlphaChannel);
    connect(dialog, &QColorDialog::currentColorChanged, this,
            [this, key](const QColor& color) {
        if (!color.isValid()) return;
        const auto preview = appearanceWithColor(key, color);
        const QScopedValueRollback guard(updatingAppearance_, true);
        const auto result = themeManager_->previewAppearance(preview);
        if (result.ok()) {
            previewActive_ = true;
            appearance_ = preview;
            refreshColors();
        }
        showError(result.diagnostics);
    });
    connect(dialog, &QDialog::finished, this, [this, dialog, key](int result) {
        colorDialog_.clear();
        if (result == QDialog::Accepted) {
            setColor(key, dialog->currentColor());
        } else {
            cancelPreview();
            load();
        }
    });
    dialog->open();
}

void AppearancePage::resetSelectedColor()
{
    if (const auto* role = selectedRole())
        setColor(QString::fromLatin1(role->key), role->read(*baseTheme_));
}

void AppearancePage::resetColors()
{
    auto next = themeManager_->appearance();
    next.colors.remove(next.themeId);
    commitAppearance(next);
}

void AppearancePage::cancelPreview()
{
    if (!previewActive_) return;
    const QScopedValueRollback guard(updatingAppearance_, true);
    themeManager_->cancelAppearancePreview();
    previewActive_ = false;
}

void AppearancePage::showError(const QList<ThemePackageDiagnostic>& diagnostics)
{
    errorLabel_->setText(diagnostics.isEmpty() ? QString()
        : tr("The appearance could not be updated: %1").arg(diagnostics.first().message));
    errorLabel_->setVisible(!diagnostics.isEmpty());
}

}
