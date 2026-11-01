#include "Dialogs/Settings/Pages/AppearancePage.h"

#include "Core/AppSettings.h"
#include "Core/ContextInfo.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeColorCatalog.h"
#include "Core/ThemeColorResolver.h"
#include "Dialogs/ColorEditorDialog.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/SearchField.h"
#include "Widgets/Common/TableItemDelegate.h"
#include "Widgets/Common/TableWidget.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QStylePainter>
#include <QTimer>

namespace VNX {

namespace {

constexpr QSize kColorSwatchSize(44, 20);

QString colorText(const QColor& color)
{
    return color.name(color.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb).toUpper();
}

QIcon colorIcon(const QColor& color, const QPalette& palette, const QColor& border,
                qreal devicePixelRatio)
{
    QPixmap swatch(kColorSwatchSize * devicePixelRatio);
    swatch.setDevicePixelRatio(devicePixelRatio);
    swatch.fill(Qt::transparent);
    QPainter painter(&swatch);
    const QRect colorRect(0, 0, kColorSwatchSize.width(), 16);
    painter.fillRect(colorRect, palette.color(QPalette::Base));
    for (int y = 0; y < colorRect.height(); y += 4)
        for (int x = 0; x < colorRect.width(); x += 4)
            if ((x / 4 + y / 4) % 2 == 0)
                painter.fillRect(QRect(x, y, 4, 4).intersected(colorRect), palette.color(QPalette::AlternateBase));
    painter.fillRect(colorRect, color);
    const QRect alphaRect(1, colorRect.bottom() + 1, kColorSwatchSize.width() - 2, 3);
    painter.fillRect(alphaRect, Qt::black);
    painter.fillRect(QRect(alphaRect.topLeft(), QSize(qRound(alphaRect.width() * color.alphaF()),
                                                     alphaRect.height())), Qt::white);
    painter.setPen(border);
    painter.drawRect(QRect(QPoint(), kColorSwatchSize).adjusted(0, 0, -1, -1));
    painter.drawLine(colorRect.bottomLeft(), colorRect.bottomRight());
    QIcon icon;
    for (QIcon::Mode mode : {QIcon::Normal, QIcon::Active, QIcon::Selected, QIcon::Disabled})
        icon.addPixmap(swatch, mode);
    return icon;
}

class ColorSwatchDelegate final : public TableItemDelegate
{
public:
    using TableItemDelegate::TableItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
    {
        option->decorationSize = kColorSwatchSize;
        TableItemDelegate::initStyleOption(option, index);
    }
};

QString roleText(const QString& text)
{
    return QCoreApplication::translate("ThemeColorCatalog", text.toUtf8().constData());
}

QPixmap presetSwatch(const ColorPalette& palette, const QVector<ThemeColorRole>& roles,
                     const ThemeColorOverrides& overrides)
{
    ThemeSnapshot snapshot = *palette.snapshot;
    auto values = palette.colors;
    for (auto value = overrides.cbegin(); value != overrides.cend(); ++value)
        values.insert(value.key(), value.value());
    resolveThemeColors(snapshot, roles, values);
    const auto& colors = snapshot.ui.colors;
    const QColor samples[]{colors.bgPrimary, colors.bgSecondary, colors.bgSelected,
        colors.accent, colors.textPrimary, colors.textSecondary, colors.success,
        colors.warning, colors.error, colors.info};
    QPixmap swatch(160, 8);
    QColor background = colors.bgPrimary;
    background.setAlpha(255);
    swatch.fill(background);
    QPainter painter(&swatch);
    for (int i = 0; i < 10; ++i) {
        const int left = i * swatch.width() / 10;
        const int right = (i + 1) * swatch.width() / 10;
        painter.fillRect(left, 0, right - left, swatch.height(), samples[i]);
    }
    return swatch;
}

QRect presetSwatchRect(const QRect& content)
{
    const QSize size(qMin(160, content.width() / 2), 8);
    return QStyle::alignedRect(Qt::LeftToRight, Qt::AlignRight | Qt::AlignVCenter, size, content);
}

class ColorPresetDelegate final : public QStyledItemDelegate
{
public:
    explicit ColorPresetDelegate(QComboBox* combo) : QStyledItemDelegate(combo), combo_(combo) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        const QRect swatchRect = presetSwatchRect(option.rect.adjusted(8, 0, -8, 0));
        QStyleOptionMenuItem item;
        item.menuHasCheckableItems = false;
        item.rect = option.rect;
        item.state = option.state;
        item.direction = option.direction;
        item.palette = option.palette;
        item.font = option.font;
        item.fontMetrics = option.fontMetrics;
        item.text = option.fontMetrics.elidedText(index.data().toString(), Qt::ElideRight,
            swatchRect.left() - option.rect.left() - 20).replace(QLatin1Char('&'), QStringLiteral("&&"));
        QtControlSkin::current().paintPopupItem(*painter, item, combo_);
        painter->drawPixmap(swatchRect, index.data(Qt::UserRole + 1).value<QPixmap>());
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionMenuItem item;
        item.menuHasCheckableItems = false;
        item.fontMetrics = option.fontMetrics;
        return QtControlSkin::current().popupItemSize(item,
            QSize(option.fontMetrics.horizontalAdvance(index.data().toString()) + 172,
                  option.fontMetrics.height()), combo_);
    }

private:
    QComboBox* combo_;
};

class ColorPresetComboBox final : public QComboBox
{
public:
    explicit ColorPresetComboBox(QWidget* parent) : QComboBox(parent)
    {
        setItemDelegate(new ColorPresetDelegate(this));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override
    {
        return QComboBox::sizeHint() + QSize(172, 0);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStylePainter painter(this);
        QStyleOptionComboBox option;
        initStyleOption(&option);
        const QRect content = style()->subControlRect(QStyle::CC_ComboBox, &option,
                                                      QStyle::SC_ComboBoxEditField, this);
        const QRect swatchRect = presetSwatchRect(content);
        option.currentText = fontMetrics().elidedText(option.currentText, Qt::ElideRight,
                                                     swatchRect.left() - content.left() - 12);
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        painter.drawControl(QStyle::CE_ComboBoxLabel, option);
        painter.drawPixmap(swatchRect, currentData(Qt::UserRole + 1).value<QPixmap>());
    }
};

}

AppearancePage::AppearancePage(ThemeManager& themeManager, QWidget* parent)
    : SettingsPage(parent)
    , themeManager_(&themeManager)
{
    auto* body = makePageBody();
    body->setContentsMargins(24, 16, 24, 16);
    body->setSpacing(12);
    auto* appearance = makeSection(tr("Appearance"));
    auto* appearanceForm = makeForm();
    auto* themeLabel = new QLabel(tr("Theme:"));
    themeCombo_ = new QComboBox();
    themeCombo_->setObjectName(QStringLiteral("appearanceThemeCombo"));
    const auto themeTopic = ContextTopic::forPanel(QStringLiteral("settings.appearance.theme"),
        {QDir::toNativeSeparators(themeManager_->userThemesRoot())});
    ContextInfo::tag(themeCombo_, themeTopic);
    ContextInfo::tag(themeLabel, themeTopic);
    themeLabel->setBuddy(themeCombo_);
    appearanceForm->addRow(themeLabel, themeCombo_);
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

    auto* colors = makeSection(tr("Theme Colors"));
    const auto colorsTopic = ContextTopic::forPanel(QStringLiteral("settings.appearance.colors"));
    ContextInfo::tag(colors, colorsTopic);
    auto* colorsLayout = static_cast<QVBoxLayout*>(colors->layout());
    colorsLayout->setSpacing(8);
    auto* colorsForm = makeForm();
    auto* presetLabel = new QLabel(tr("Color Palette:"));
    colorPresets_ = new ColorPresetComboBox(colors);
    colorPresets_->setObjectName(QStringLiteral("appearanceColorPresets"));
    colorPresets_->setAccessibleName(tr("Color palette"));
    ContextInfo::tag(colorPresets_, colorsTopic);
    ContextInfo::tag(presetLabel, colorsTopic);
    colorsForm->addRow(presetLabel, colorPresets_);
    colorsLayout->addLayout(colorsForm);

    colorGroup_ = new QComboBox(colors);
    colorGroup_->setObjectName(QStringLiteral("appearanceColorGroup"));
    colorGroup_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    colorGroup_->addItem(tr("All categories"), QString());
    colorSearch_ = new SearchField(colors);
    colorSearch_->setObjectName(QStringLiteral("appearanceColorSearch"));
    colorSearch_->setPlaceholderText(tr("Search colors and uses..."));
    const int filterRow = colorsForm->rowCount();
    colorsForm->addWidget(colorGroup_, filterRow, 0, Qt::AlignLeft);
    colorsForm->addWidget(colorSearch_, filterRow, 1);

    auto* table = new TableWidget(TableOptions::display(), colors, themeManager_);
    table->setColumnsMovable(false);
    table->setFixedHeight(260);
    colorTree_ = table->createItemView(QStringLiteral("appearanceColorTree"),
        {tr("Role"), tr("Color"), tr("Palette Default")}, {46, 27, 27});
    colorTree_->setRootIsDecorated(false);
    colorTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    colorTree_->setSelectionBehavior(QAbstractItemView::SelectRows);
    colorTree_->setAllColumnsShowFocus(true);
    colorTree_->setItemDelegate(new ColorSwatchDelegate(colorTree_, themeManager_));
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
    colorsReset_->setToolTip(tr("Restore the selected palette of this theme to its default colors."));
    actions->addStretch();
    actions->addWidget(colorReset_);
    actions->addWidget(colorsReset_);
    colorsLayout->addLayout(actions);

    colors->setToolTip(tr("Color edits are saved to the selected palette of this theme."));
    errorLabel_ = new QLabel(colors);
    errorLabel_->setObjectName(QStringLiteral("appearanceColorError"));
    errorLabel_->setProperty("textRole", QStringLiteral("error"));
    errorLabel_->setWordWrap(true);
    errorLabel_->hide();
    colorsLayout->addWidget(errorLabel_);
    body->addWidget(colors);
    body->addStretch();

    previewTimer_ = new QTimer(this);
    previewTimer_->setInterval(16);
    previewTimer_->setTimerType(Qt::PreciseTimer);
    connect(previewTimer_, &QTimer::timeout, this, &AppearancePage::publishPreview);

    connect(themeCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AppearancePage::selectTheme);
    connect(colorPresets_, QOverload<int>::of(&QComboBox::activated),
            this, &AppearancePage::selectColorPreset);
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
    connect(themeManager_, &ThemeManager::appearanceChanged, this, [this] {
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
        const QSignalBlocker blocker(themeCombo_);
        themeCombo_->clear();
        for (const ThemePackageDescriptor& theme : themeManager_->availableThemes()) {
            const QString label = theme.builtin
                ? tr("%1 (Built-in)").arg(theme.displayName) : theme.displayName;
            themeCombo_->addItem(label, theme.id);
            themeCombo_->setItemData(themeCombo_->count() - 1,
                tr("ID: %1\nVersion: %2").arg(theme.id, theme.version), Qt::ToolTipRole);
        }
        int currentIndex = themeCombo_->findData(appearance_.themeId);
        if (currentIndex < 0) {
            themeCombo_->addItem(appearance_.themeId, appearance_.themeId);
            currentIndex = themeCombo_->count() - 1;
        }
        themeCombo_->setCurrentIndex(currentIndex);
    }
    {
        const QSignalBlocker blocker(colorPresets_);
        colorPresets_->clear();
        palettes_.clear();
        for (const ColorPaletteDescriptor& palette : themeManager_->availablePalettes()) {
            colorPresets_->addItem(palette.displayName, palette.id);
            const auto loaded = themeManager_->loadPalette(palette.id);
            if (loaded.ok()) palettes_.insert(palette.id, *loaded.palette);
        }
    }
    rebuildColorRoles();
    refreshLanguage();
    loadBasePalette();
}

void AppearancePage::rebuildColorRoles()
{
    const auto& roles = themeManager_->colorRoles();
    if (colorRoles_ == roles) return;
    const QString selected = colorTree_->currentItem()
        ? colorTree_->currentItem()->data(0, Qt::UserRole).toString() : QString();
    const QString group = colorGroup_->currentData().toString();
    const QSignalBlocker treeBlocker(colorTree_);
    const QSignalBlocker groupBlocker(colorGroup_);
    colorRoles_ = roles;
    colorTree_->clear();
    colorGroup_->clear();
    colorGroup_->addItem(tr("All categories"), QString());
    for (const auto& role : colorRoles_) {
        if (colorGroup_->findData(role.group) < 0)
            colorGroup_->addItem(roleText(role.group), role.group);
        auto* item = new QTreeWidgetItem(colorTree_);
        item->setData(0, Qt::UserRole, role.key);
        item->setText(0, roleText(role.label));
        item->setToolTip(0, roleText(role.description));
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        if (role.key == selected) colorTree_->setCurrentItem(item);
    }
    const int groupIndex = colorGroup_->findData(group);
    colorGroup_->setCurrentIndex(groupIndex < 0 ? 0 : groupIndex);
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
    const QString themeId = themeCombo_->currentData().toString();
    if (themeId.isEmpty()) return;
    auto next = themeManager_->appearance();
    next.themeId = themeId;
    next.paletteId.clear();
    commitAppearance(next);
}

void AppearancePage::selectColorPreset()
{
    const QString paletteId = colorPresets_->currentData().toString();
    if (!palettes_.contains(paletteId)) return;
    auto next = themeManager_->appearance();
    next.paletteId = paletteId;
    commitAppearance(next);
}

bool AppearancePage::commitAppearance(const ThemeAppearance& appearance)
{
    previewTimer_->stop();
    pendingPreviewColor_ = QColor();
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

bool AppearancePage::loadBasePalette()
{
    const auto loaded = themeManager_->loadPalette(appearance_.paletteId);
    basePalette_ = loaded.palette;
    showError(loaded.diagnostics);
    refreshColors();
    return loaded.ok();
}

void AppearancePage::refreshPresets()
{
    const QSignalBlocker blocker(colorPresets_);
    auto* model = static_cast<QStandardItemModel*>(colorPresets_->model());
    for (int i = 0; i < colorPresets_->count(); ++i) {
        const QString id = colorPresets_->itemData(i).toString();
        const QString name = colorPresets_->itemText(i);
        const auto base = palettes_.constFind(id);
        const bool available = base != palettes_.cend();
        const auto overrides = appearance_.colors.value(appearance_.themeId).value(id);
        colorPresets_->setItemData(i, available ? presetSwatch(*base, colorRoles_, overrides) : QPixmap(), Qt::UserRole + 1);
        model->item(i)->setEnabled(available);
        colorPresets_->setItemData(i, available
            ? tr("Use the %1 palette of this theme. Edits are saved for this theme and palette.\nMain background · Controls · Selection · Accent · Primary text\nSecondary text · Success · Warning · Error · Information").arg(name)
            : tr("Unavailable"), Qt::ToolTipRole);
    }
    colorPresets_->setCurrentIndex(colorPresets_->findData(appearance_.paletteId));
    colorPresets_->update();
}

QColor AppearancePage::effectiveColor(const ThemeColorRole& role) const
{
    const QString key = role.key;
    return appearance_.colors.value(appearance_.themeId).value(appearance_.paletteId).value(key, basePalette_->colors.value(key));
}

void AppearancePage::refreshColors()
{
    refreshPresets();
    colorTree_->setEnabled(bool(basePalette_));
    colorGroup_->setEnabled(bool(basePalette_));
    colorSearch_->setEnabled(bool(basePalette_));
    colorsReset_->setEnabled(basePalette_ && !appearance_.colors.value(appearance_.themeId).value(appearance_.paletteId).isEmpty());
    for (int i = 0; i < colorTree_->topLevelItemCount(); ++i) {
        auto* item = colorTree_->topLevelItem(i);
        const QString key = item->data(0, Qt::UserRole).toString();
        const auto* role = findThemeColorRole(colorRoles_, key);
        if (!basePalette_) {
            item->setText(1, QString());
            item->setText(2, QString());
            item->setIcon(1, QIcon());
            item->setIcon(2, QIcon());
            continue;
        }
        const QColor value = effectiveColor(*role);
        const QColor original = basePalette_->colors.value(role->key);
        item->setText(1, colorText(value));
        item->setIcon(1, colorIcon(value, palette(), themeManager_->colors().borderSecondary,
                                  devicePixelRatioF()));
        item->setText(2, colorText(original));
        item->setIcon(2, colorIcon(original, palette(), themeManager_->colors().borderSecondary,
                                  devicePixelRatioF()));
        item->setToolTip(1, value == original ? tr("Palette default") : tr("Customized palette color"));
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
        const auto* role = findThemeColorRole(colorRoles_, item->data(0, Qt::UserRole).toString());
        const QString searchable = roleText(role->label) + QLatin1Char(' ')
            + roleText(role->group) + QLatin1Char(' ') + roleText(role->description)
            + QLatin1Char(' ') + role->key;
        const bool matches = (group.isEmpty() || group == role->group)
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
    return item && !item->isHidden() && basePalette_
        ? findThemeColorRole(colorRoles_, item->data(0, Qt::UserRole).toString()) : nullptr;
}

void AppearancePage::refreshSelectedColor()
{
    const auto* role = selectedRole();
    colorReset_->setEnabled(role && appearance_.colors.value(appearance_.themeId).value(appearance_.paletteId).contains(role->key));
    colorDescription_->setText(role ? roleText(role->description)
        : basePalette_ ? tr("No matching colors.") : tr("Palette colors are unavailable."));
}

ThemeAppearance AppearancePage::appearanceWithColor(const QString& key, const QColor& color) const
{
    auto next = themeManager_->appearance();
    if (color == basePalette_->colors.value(key)) {
        auto theme = next.colors.find(next.themeId);
        if (theme != next.colors.end()) {
            auto overrides = theme->find(next.paletteId);
            if (overrides != theme->end()) {
                overrides->remove(key);
                if (overrides->isEmpty()) theme->erase(overrides);
            }
            if (theme->isEmpty()) next.colors.erase(theme);
        }
    } else {
        next.colors[next.themeId][next.paletteId].insert(key, color);
    }
    return next;
}

bool AppearancePage::setColor(const QString& key, const QColor& color)
{
    if (!findThemeColorRole(colorRoles_, key) || !basePalette_ || !color.isValid()) return false;
    return commitAppearance(appearanceWithColor(key, color));
}

void AppearancePage::editSelectedColor()
{
    if (colorDialog_) return;
    const auto* role = selectedRole();
    if (!role) return;
    const QString key = role->key;
    previewColorKey_ = key;
    auto* dialog = new ColorEditorDialog(effectiveColor(*role), this);
    colorDialog_ = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setObjectName(QStringLiteral("appearanceColorDialog"));
    dialog->setWindowTitle(tr("Color: %1").arg(roleText(role->label)));
    connect(dialog, &ColorEditorDialog::currentColorChanged, this,
            [this](const QColor& color) {
        if (!color.isValid()) return;
        pendingPreviewColor_ = color;
        if (!previewTimer_->isActive()) previewTimer_->start();
    });
    connect(dialog, &QDialog::finished, this, [this, dialog, key](int result) {
        previewTimer_->stop();
        pendingPreviewColor_ = QColor();
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

void AppearancePage::publishPreview()
{
    if (!colorDialog_ || !pendingPreviewColor_.isValid()) {
        previewTimer_->stop();
        return;
    }
    const auto preview = appearanceWithColor(previewColorKey_, pendingPreviewColor_);
    pendingPreviewColor_ = QColor();
    const QScopedValueRollback guard(updatingAppearance_, true);
    const auto result = themeManager_->previewAppearance(preview);
    if (result.ok()) {
        previewActive_ = true;
        appearance_ = preview;
        refreshColors();
    }
    showError(result.diagnostics);
}

void AppearancePage::resetSelectedColor()
{
    if (const auto* role = selectedRole())
        setColor(role->key, basePalette_->colors.value(role->key));
}

void AppearancePage::resetColors()
{
    auto next = themeManager_->appearance();
    auto theme = next.colors.find(next.themeId);
    if (theme != next.colors.end()) {
        theme->remove(next.paletteId);
        if (theme->isEmpty()) next.colors.erase(theme);
    }
    commitAppearance(next);
}

void AppearancePage::cancelPreview()
{
    previewTimer_->stop();
    pendingPreviewColor_ = QColor();
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
