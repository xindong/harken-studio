#include "Dialogs/Settings/Pages/AppearancePage.h"
#include "Core/ThemeLayout.h"

#include "Core/AppSettings.h"
#include "Core/ContextInfo.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeColorCatalog.h"
#include "Core/ThemeColorResolver.h"
#include "Dialogs/ColorEditorDialog.h"
#include "Widgets/Common/InspectorTabContainer.h"
#include "Widgets/Common/ProportionalTreeWidget.h"
#include "Widgets/Common/SearchField.h"
#include "Widgets/Common/TableItemDelegate.h"
#include "Widgets/Common/TableWidget.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QHideEvent>
#include <QIconEngine>
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

namespace harken::studio {

namespace {

QSize colorSwatchSize(const ThemeMetrics& metrics)
{
    return {metrics.colorPreview.swatchWidth,
            metrics.colorPreview.swatchHeight + metrics.colorPreview.alphaHeight + metrics.borderWidth};
}

class SwatchIcon final : public QIconEngine
{
public:
    SwatchIcon(const QColor& color, const QPalette& palette, const QColor& border, const ThemeMetrics& metrics)
        : color_(color), palette_(palette), border_(border), size_(colorSwatchSize(metrics)),
          colorHeight_(metrics.colorPreview.swatchHeight), alphaHeight_(metrics.colorPreview.alphaHeight),
          borderWidth_(metrics.borderWidth) {}

    QIconEngine* clone() const override { return new SwatchIcon(*this); }
    QList<QSize> availableSizes(QIcon::Mode, QIcon::State) override { return {size_}; }
    QSize actualSize(const QSize& requested, QIcon::Mode, QIcon::State) override
    {
        return size_.scaled(size_.boundedTo(requested), Qt::KeepAspectRatio);
    }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap swatch(size);
        swatch.fill(Qt::transparent);
        QPainter painter(&swatch);
        paint(&painter, swatch.rect(), mode, state);
        return swatch;
    }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode, QIcon::State) override
    {
        painter->save();
        painter->translate(rect.topLeft());
        painter->scale(qreal(rect.width()) / size_.width(), qreal(rect.height()) / size_.height());
        const QRect colorRect(0, 0, size_.width(), colorHeight_);
        painter->fillRect(colorRect, palette_.color(QPalette::Base));
        const int cell = qMax(1, colorHeight_ / 4);
        for (int y = 0; y < colorRect.height(); y += cell)
            for (int x = 0; x < colorRect.width(); x += cell)
                if ((x / cell + y / cell) % 2 == 0)
                    painter->fillRect(QRect(x, y, cell, cell).intersected(colorRect), palette_.color(QPalette::AlternateBase));
        painter->fillRect(colorRect, color_);
        const QRect alphaRect(borderWidth_, colorHeight_, qMax(0, size_.width() - borderWidth_ * 2), alphaHeight_);
        painter->fillRect(alphaRect, Qt::black);
        painter->fillRect(QRect(alphaRect.topLeft(), QSize(qRound(alphaRect.width() * color_.alphaF()),
                                                          alphaRect.height())), Qt::white);
        if (borderWidth_ > 0) {
            painter->setPen(QPen(border_, borderWidth_));
            painter->setBrush(Qt::NoBrush);
            const qreal inset = borderWidth_ / 2.0;
            painter->drawRect(QRectF(QPoint(), size_).adjusted(inset, inset, -inset, -inset));
            painter->drawLine(colorRect.bottomLeft(), colorRect.bottomRight());
        }
        painter->restore();
    }

private:
    QColor color_;
    QPalette palette_;
    QColor border_;
    QSize size_;
    int colorHeight_;
    int alphaHeight_;
    int borderWidth_;
};

class ColorSwatchDelegate final : public TableItemDelegate
{
public:
    ColorSwatchDelegate(QObject* parent, ThemeManager* theme)
        : TableItemDelegate(parent, theme), theme_(theme) {}

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
    {
        option->decorationSize = colorSwatchSize(theme_->metrics());
        if (option->rect.isValid()) option->decorationSize = option->decorationSize.boundedTo(option->rect.size());
        TableItemDelegate::initStyleOption(option, index);
    }

private:
    ThemeManager* theme_;
};

QString roleText(const QString& text)
{
    return QCoreApplication::translate("ThemeColorCatalog", text.toUtf8().constData());
}

QPixmap presetSwatch(const ColorPalette& palette, const QVector<ThemeColorRole>& roles,
                     const ThemeColorOverrides& overrides, const ThemeMetrics& metrics)
{
    ThemeSnapshot snapshot = *palette.snapshot;
    auto values = palette.colors;
    for (auto value = overrides.cbegin(); value != overrides.cend(); ++value)
        values.insert(value.key(), value.value());
    resolveThemeColors(snapshot, roles, values);
    const auto& colors = snapshot.ui.colors;
    const QColor samples[]{colors.bgPrimary, colors.bgSecondary, colors.bgSelected,
        colors.accent, colors.textPrimary, colors.textSecondary, colors.success,
        colors.warning, colors.error, colors.accentSecondary};
    QPixmap swatch(metrics.colorPreview.paletteWidth, metrics.colorPreview.paletteHeight);
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

QRect presetSwatchRect(const QRect& content, int textWidth, const ThemeMetrics& metrics)
{
    const int available = qMax(0, content.width() - textWidth - metrics.layout.controlGap);
    const QSize size(qMin(metrics.colorPreview.paletteWidth, available), metrics.colorPreview.paletteHeight);
    return QStyle::alignedRect(Qt::LeftToRight, Qt::AlignRight | Qt::AlignVCenter, size, content);
}

class ColorPresetDelegate final : public QStyledItemDelegate
{
public:
    ColorPresetDelegate(QComboBox* combo, ThemeManager* theme)
        : QStyledItemDelegate(combo), combo_(combo), theme_(theme) {}

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        const auto& metrics = theme_->metrics();
        const QString name = index.data().toString();
        const QRect swatchRect = presetSwatchRect(option.rect.adjusted(metrics.control.textPaddingX, 0, -metrics.control.textPaddingX, 0),
                                                  option.fontMetrics.horizontalAdvance(name), metrics);
        QStyleOptionMenuItem item;
        item.menuHasCheckableItems = false;
        item.rect = option.rect;
        item.state = option.state;
        item.direction = option.direction;
        item.palette = option.palette;
        item.font = option.font;
        item.fontMetrics = option.fontMetrics;
        item.text = option.fontMetrics.elidedText(name, Qt::ElideRight,
            swatchRect.left() - option.rect.left() - metrics.control.textPaddingX - metrics.layout.controlGap).replace(QLatin1Char('&'), QStringLiteral("&&"));
        QtControlSkin(theme_->snapshot()).paintPopupItem(*painter, item, combo_);
        painter->drawPixmap(swatchRect, index.data(Qt::UserRole + 1).value<QPixmap>());
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionMenuItem item;
        item.menuHasCheckableItems = false;
        item.fontMetrics = option.fontMetrics;
        return QtControlSkin(theme_->snapshot()).popupItemSize(item,
            QSize(option.fontMetrics.horizontalAdvance(index.data().toString()) + theme_->metrics().colorPreview.paletteWidth + theme_->metrics().spacingMedium,
                  option.fontMetrics.height()), combo_);
    }

private:
    QComboBox* combo_;
    ThemeManager* theme_;
};

class ColorPresetComboBox final : public QComboBox
{
public:
    ColorPresetComboBox(QWidget* parent, ThemeManager* theme) : QComboBox(parent), theme_(theme)
    {
        setItemDelegate(new ColorPresetDelegate(this, theme));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override
    {
        return QComboBox::sizeHint() + QSize(theme_->metrics().colorPreview.paletteWidth, 0);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QStylePainter painter(this);
        QStyleOptionComboBox option;
        initStyleOption(&option);
        const QRect content = style()->subControlRect(QStyle::CC_ComboBox, &option,
                                                      QStyle::SC_ComboBoxEditField, this);
        const QRect swatchRect = presetSwatchRect(content, fontMetrics().horizontalAdvance(option.currentText), theme_->metrics());
        option.currentText = fontMetrics().elidedText(option.currentText, Qt::ElideRight,
                                                     swatchRect.left() - content.left() - theme_->metrics().layout.controlGap);
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        painter.drawControl(QStyle::CE_ComboBoxLabel, option);
        painter.drawPixmap(swatchRect, currentData(Qt::UserRole + 1).value<QPixmap>());
    }

private:
    ThemeManager* theme_;
};

}

AppearancePage::AppearancePage(ThemeManager& themeManager, QWidget* parent)
    : SettingsPage(parent)
    , themeManager_(&themeManager)
    , uiScale_(themeManager.snapshot()->uiScale)
{
    ContextInfo::tag(this, ContextTopic::forPanel(QStringLiteral("settings.appearance")));
    auto* body = new QVBoxLayout(this);
    ThemeLayout::margins(body, ThemeLayout::Space::Panel, *themeManager_);
    ThemeLayout::spacing(body, ThemeLayout::Space::Section, *themeManager_);
    auto* appearance = makeSection(tr("Appearance"));
    auto* appearanceForm = makeForm();
    auto* themeLabel = new QLabel(tr("Theme:"));
    themeCombo_ = new QComboBox();
    themeCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    themeCombo_->setObjectName(QStringLiteral("appearanceThemeCombo"));
    const auto themeTopic = ContextTopic::forPanel(QStringLiteral("settings.appearance.theme"),
        {QDir::toNativeSeparators(themeManager_->userThemesRoot())});
    ContextInfo::tag(themeCombo_, themeTopic);
    ContextInfo::tag(themeLabel, themeTopic);
    themeLabel->setBuddy(themeCombo_);
    appearanceForm->addRow(themeLabel, themeCombo_);
    auto* scaleLabel = new QLabel(tr("Editor Scale:"));
    uiScaleCombo_ = new QComboBox();
    uiScaleCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    uiScaleCombo_->setObjectName(QStringLiteral("appearanceUIScaleCombo"));
    uiScaleCombo_->setAccessibleName(tr("Editor scale"));
    for (int level = AppSettings::MIN_GLOBAL_ZOOM_LEVEL; level <= AppSettings::MAX_GLOBAL_ZOOM_LEVEL; ++level) {
        const int percent = qRound(AppSettings::uiScaleForZoomLevel(level));
        uiScaleCombo_->addItem(level == 0 ? tr("%1% (Default)").arg(percent)
                                        : tr("%1%").arg(percent), level);
    }
    uiScaleCombo_->setToolTip(tr("Adjust the size of the editor immediately. This setting is shared by all themes."));
    scaleLabel->setBuddy(uiScaleCombo_);
    appearanceForm->addRow(scaleLabel, uiScaleCombo_);
    auto* languageLabel = new QLabel(tr("Information Language:"));
    languageCombo_ = new QComboBox();
    languageCombo_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    languageCombo_->setObjectName(QStringLiteral("appearanceLanguageCombo"));
    const auto languageTopic = ContextTopic::forPanel(QStringLiteral("settings.appearance.language"));
    ContextInfo::tag(languageCombo_, languageTopic);
    ContextInfo::tag(languageLabel, languageTopic);
    languageCombo_->addItem(tr("English"), QStringLiteral("en"));
    languageCombo_->addItem(tr("Chinese (Simplified)"), QStringLiteral("zh-CN"));
    languageLabel->setBuddy(languageCombo_);
    appearanceForm->addRow(languageLabel, languageCombo_);
    static_cast<QVBoxLayout*>(appearance->layout())->addLayout(appearanceForm);
    body->addWidget(appearance, 0, Qt::AlignTop);

    auto* tabs = new InspectorTabContainer(this, InspectorTabContainer::FramePlacement::Group);
    body->addWidget(makeSection(tr("Theme Configuration"), tabs));
    auto* colors = makeTab(tabs, tr("Colors"));
    const auto colorsTopic = ContextTopic::forPanel(QStringLiteral("settings.appearance.colors"));
    ContextInfo::tag(colors, colorsTopic);
    auto* colorsLayout = static_cast<QVBoxLayout*>(colors->layout());
    ThemeLayout::spacing(colorsLayout, ThemeLayout::Space::Section, *themeManager_);
    auto* colorsForm = makeForm();
    auto* presetLabel = new QLabel(tr("Color Palette:"));
    colorPresets_ = new ColorPresetComboBox(colors, themeManager_);
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
    themeManager_->bindMetrics(table, [table](const ThemeMetrics& metrics) {
        table->setFixedHeight(metrics.panel.previewHeight);
    });
    colorTree_ = table->createItemView(QStringLiteral("appearanceColorTree"),
        {tr("Role"), tr("Color"), tr("Palette Default")}, {1, 1, 1});
    colorTree_->setRootIsDecorated(false);
    colorTree_->setSelectionMode(QAbstractItemView::SingleSelection);
    colorTree_->setAllColumnsShowFocus(true);
    colorTree_->setContextMenuPolicy(Qt::PreventContextMenu);
    colorTree_->setItemDelegate(new ColorSwatchDelegate(colorTree_, themeManager_));
    colorTableMessage_ = table->bodyMessage();
    colorsLayout->addWidget(table);

    auto* actions = new QHBoxLayout();
    ThemeLayout::spacing(actions, ThemeLayout::Space::Tight, *themeManager_);
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
    colorsLayout->addStretch();

    auto* typography = makeTab(tabs, tr("Typography"));
    ContextInfo::tag(typography, ContextTopic::forPanel(QStringLiteral("settings.appearance.typography")));
    buildTypography(typography);
    static_cast<QVBoxLayout*>(typography->layout())->addStretch();

    auto* metrics = makeTab(tabs, tr("Spacing & Sizing"));
    ContextInfo::tag(metrics, ContextTopic::forPanel(QStringLiteral("settings.appearance.metrics")));
    buildMetrics(metrics);
    static_cast<QVBoxLayout*>(metrics->layout())->addStretch();
    connect(tabs, &InspectorTabContainer::currentChanged, this, [this](int index) {
        if (index != 0 && colorDialog_)
            colorDialog_->reject();
        if (index != 1 && systemFontDialog_)
            systemFontDialog_->reject();
    });

    errorLabel_ = new QLabel(this);
    errorLabel_->setObjectName(QStringLiteral("appearanceColorError"));
    errorLabel_->setProperty("textRole", QStringLiteral("error"));
    errorLabel_->setWordWrap(true);
    errorLabel_->hide();
    body->addWidget(errorLabel_);
    body->addStretch(1);

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
    connect(uiScaleCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (index >= 0) AppSettings::instance().setGlobalUIZoomLevel(uiScaleCombo_->itemData(index).toInt());
    });
    connect(colorGroup_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &AppearancePage::filterColors);
    connect(colorSearch_, &QLineEdit::textChanged, this, &AppearancePage::filterColors);
    connect(colorTree_, &QTreeWidget::itemActivated, this, &AppearancePage::editColor);
    connect(colorTree_, &QTreeWidget::itemSelectionChanged, this, &AppearancePage::refreshColorActions);
    connect(colorReset_, &QPushButton::clicked, this, [this] {
        const auto rows = colorTree_->selectedItems();
        if (!basePalette_ || rows.isEmpty()) return;
        const auto key = rows.front()->data(0, Qt::UserRole).toString();
        setColor(key, basePalette_->colors.value(key));
    });
    connect(colorsReset_, &QPushButton::clicked, this, &AppearancePage::resetColors);
    connect(themeManager_, &ThemeManager::appearanceChanged, this, [this](ThemeSnapshotPtr snapshot) {
        if (systemFontDialog_ && themeManager_->currentThemeId() != appearance_.themeId)
            systemFontDialog_->reject();
        const bool scaleChanged = uiScale_ != snapshot->uiScale;
        uiScale_ = snapshot->uiScale;
        if (updatingAppearance_) return;
        if (scaleChanged) {
            refreshColors();
            return;
        }
        if (colorDialog_) colorDialog_->reject();
        load();
    });
    connect(&AppSettings::instance(), &AppSettings::languageChanged,
            this, &AppearancePage::refreshLanguage);
    connect(&AppSettings::instance(), &AppSettings::globalUIScaleChanged,
            this, &AppearancePage::refreshUIScale);
    refreshUIScale();
}

AppearancePage::~AppearancePage()
{
    if (colorDialog_) disconnect(colorDialog_, nullptr, this, nullptr);
    if (systemFontDialog_) disconnect(systemFontDialog_, nullptr, this, nullptr);
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
    refreshUIScale();
    loadBasePalette();
    refreshTypography();
    refreshMetrics();
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
        const QString description = roleText(role.description);
        for (int column = 0; column < colorTree_->columnCount(); ++column)
            item->setToolTip(column, description);
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
    if (systemFontDialog_) systemFontDialog_->reject();
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

void AppearancePage::refreshUIScale()
{
    const QSignalBlocker blocker(uiScaleCombo_);
    uiScaleCombo_->setCurrentIndex(uiScaleCombo_->findData(AppSettings::instance().globalUIZoomLevel()));
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
        colorPresets_->setItemData(i, available ? presetSwatch(*base, colorRoles_, overrides, themeManager_->metrics()) : QPixmap(), Qt::UserRole + 1);
        model->item(i)->setEnabled(available);
        colorPresets_->setItemData(i, available
            ? tr("Use the %1 palette of this theme. Edits are saved for this theme and palette.\nMain background · Controls · Selection · Primary accent · Primary foreground\nSecondary foreground · Success · Warning · Error · Secondary accent").arg(name)
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
        item->setText(1, ColorEditorDialog::hexText(value));
        item->setIcon(1, QIcon(new SwatchIcon(value, palette(), themeManager_->colors().borderSecondary, themeManager_->metrics())));
        item->setText(2, ColorEditorDialog::hexText(original));
        item->setIcon(2, QIcon(new SwatchIcon(original, palette(), themeManager_->colors().borderSecondary, themeManager_->metrics())));
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
    colorTableMessage_->setText(basePalette_ ? tr("No matching colors.") : tr("Palette colors are unavailable."));
    colorTableMessage_->setVisible(!first || !basePalette_);
    refreshColorActions();
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

void AppearancePage::editColor(QTreeWidgetItem* item)
{
    if (!item || item->isHidden() || !basePalette_ || colorDialog_) return;
    const auto* role = findThemeColorRole(colorRoles_, item->data(0, Qt::UserRole).toString());
    if (!role) return;
    const QString key = role->key;
    previewColorKey_ = key;
    auto* dialog = new ColorEditorDialog(effectiveColor(*role), this, role->opaque);
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

void AppearancePage::refreshColorActions()
{
    const auto edits = appearance_.colors.value(appearance_.themeId).value(appearance_.paletteId);
    const auto rows = colorTree_->selectedItems();
    colorReset_->setEnabled(basePalette_ && !rows.isEmpty() && edits.contains(rows.front()->data(0, Qt::UserRole).toString()));
    colorsReset_->setEnabled(basePalette_ && !edits.isEmpty());
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
