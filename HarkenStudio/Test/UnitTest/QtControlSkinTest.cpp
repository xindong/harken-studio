#include "Core/BuiltinTheme.h"
#include "Core/QtControlSkin.h"
#include "Core/ThemeManager.h"
#include "Widgets/Common/SearchField.h"
#include "Widgets/Common/SegmentedControl.h"
#include "Widgets/Common/MultiSelectComboBox.h"
#include "Widgets/Common/TableWidget.h"
#include "Widgets/Common/ValueSegmentedControl.h"
#include "Widgets/Selector/ObjectRefWidget.h"

#include <gtest/gtest.h>

#include <QPalette>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEnterEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSpinBox>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleOptionButton>
#include <QStyleOptionHeader>
#include <QTableView>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include <memory>
#include <utility>

namespace {

VNX::QtControlSkin builtinSkin()
{
    return VNX::QtControlSkin(
        std::make_shared<const VNX::ThemeSnapshot>(VNX::createBuiltinDarkTheme()));
}

void expectPaletteColor(const QPalette& palette, QPalette::ColorRole role,
                        const char* expectedArgb)
{
    EXPECT_EQ(palette.color(QPalette::Active, role).name(QColor::HexArgb),
              QString::fromLatin1(expectedArgb));
}

void expectDisabledPaletteColor(const QPalette& palette, QPalette::ColorRole role,
                                const char* expectedArgb)
{
    EXPECT_EQ(palette.color(QPalette::Disabled, role).name(QColor::HexArgb),
              QString::fromLatin1(expectedArgb));
}

}

TEST(QtControlSkin, ApplicationPalettePreservesEveryExplicitRole)
{
    const QPalette palette = builtinSkin().applicationPalette();

    expectPaletteColor(palette, QPalette::Window,          "#ff0b0e14");
    expectPaletteColor(palette, QPalette::WindowText,      "#ffe0e0e0");
    expectPaletteColor(palette, QPalette::Base,            "#ff080a0f");
    expectPaletteColor(palette, QPalette::AlternateBase,   "#ff10141c");
    expectPaletteColor(palette, QPalette::ToolTipBase,     "#ff151922");
    expectPaletteColor(palette, QPalette::ToolTipText,     "#ffe0e0e0");
    expectPaletteColor(palette, QPalette::Text,            "#ffe0e0e0");
    expectPaletteColor(palette, QPalette::Button,          "#ff252a35");
    expectPaletteColor(palette, QPalette::ButtonText,      "#ffe0e0e0");
    expectPaletteColor(palette, QPalette::BrightText,      "#ffffffff");
    expectPaletteColor(palette, QPalette::Link,            "#ff00f0ff");
    expectPaletteColor(palette, QPalette::Highlight,       "#ff00f0ff");
    expectPaletteColor(palette, QPalette::HighlightedText, "#ff000000");
    expectPaletteColor(palette, QPalette::Light,           "#ffe0e0e0");
    expectPaletteColor(palette, QPalette::Dark,            "#ff888888");
    expectPaletteColor(palette, QPalette::Mid,             "#ff2a2e35");

    expectDisabledPaletteColor(palette, QPalette::Text,       "#ff555555");
    expectDisabledPaletteColor(palette, QPalette::ButtonText, "#ff555555");
    expectDisabledPaletteColor(palette, QPalette::Highlight,  "#ff3c3c3c");
}

TEST(QtControlSkin, StyleHelpersConsumeTheProvidedSnapshot)
{
    VNX::ThemeSnapshot theme = VNX::createBuiltinDarkTheme();
    theme.ui.colors.accent = QColor(QStringLiteral("#123456"));
    theme.ui.colors.accentHover = QColor(QStringLiteral("#654321"));
    theme.ui.metrics.radiusSmall = 13;

    const VNX::QtControlSkin skin(
        std::make_shared<const VNX::ThemeSnapshot>(std::move(theme)));

    // The application skin emits colours in #AARRGGBB form.
    const QString applicationStyleSheet = skin.applicationStyleSheet();
    EXPECT_TRUE(applicationStyleSheet.contains(QStringLiteral("#ff123456")));
    EXPECT_TRUE(applicationStyleSheet.contains(QStringLiteral("#ff654321")));
    EXPECT_TRUE(applicationStyleSheet.contains(QStringLiteral("border-radius: 13px")));
    EXPECT_TRUE(skin.colorSwatchStyle(QColor(QStringLiteral("#ffffff")))
                    .contains(QStringLiteral("color: #ff000000")));
    EXPECT_TRUE(skin.colorSwatchStyle(QColor(QStringLiteral("#000000")))
                    .contains(QStringLiteral("color: #ffffffff")));
    EXPECT_TRUE(skin.collapsibleIndicatorStyle(
                        QColor(QStringLiteral("#123456")), 99, true)
                    .contains(QStringLiteral("rgba(18, 52, 86, 99)")));
    EXPECT_EQ(VNX::QtControlSkin::rgba(QColor(1, 2, 3), 999),
              QStringLiteral("rgba(1, 2, 3, 255)"));
}

TEST(QtControlSkin, SegmentedFieldsKeepTextStableAndShareHoverOutlines)
{
    VNX::ThemeSnapshot theme = VNX::createBuiltinDarkTheme();
    theme.ui.colors.accent = QColor(QStringLiteral("#123456"));
    theme.ui.metrics.controlHeight = 32;
    theme.ui.metrics.spacingSmall = 5;
    theme.ui.metrics.spacingXSmall = 2;

    const QString styleSheet = VNX::QtControlSkin(
        std::make_shared<const VNX::ThemeSnapshot>(std::move(theme)))
        .applicationStyleSheet();

    EXPECT_TRUE(styleSheet.contains(QStringLiteral("padding: 0px 5px")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("max-height: 30px")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral(
        "QToolBar {\n"
        "    background-color: #ff0b0e14;\n"
        "    border: none;\n"
        "    border-top: 1px solid #ff2a3140;\n"
        "    spacing: 2px;\n"
        "    padding: 2px;\n"
        "    min-height: 24px;\n"
        "    max-height: 24px;\n"
        "}")));

    QWidget form;
    form.setStyleSheet(styleSheet);
    auto* layout = new QVBoxLayout(&form);
    auto* segments = new VNX::SegmentedControl(&form);
    segments->setOptions({{QStringLiteral("First"), 0}, {QStringLiteral("Middle"), 1},
                          {QStringLiteral("Last"), 2}});
    auto* values = new VNX::ValueSegmentedControl(&form);
    values->setSegments({{QString(), 42, true, 1, 100},
                         {QStringLiteral("Off"), -1}, {QStringLiteral("Infinite"), 0}});
    values->setValue(42);
    layout->addWidget(segments);
    layout->addWidget(values);
    form.resize(480, 120);
    form.show();
    QApplication::processEvents();

    const auto render = [](QWidget* widget) {
        QImage image(widget->size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        widget->render(&image);
        return image;
    };
    const auto containsColor = [](const QImage& image, const QColor& color) {
        for (int y = 1; y < image.height() - 1; ++y)
            for (int x = 1; x < image.width() - 1; ++x)
                if (image.pixelColor(x, y) == color) return true;
        return false;
    };
    const auto hover = [](QWidget* widget, bool active) {
        widget->setAttribute(Qt::WA_UnderMouse, active);
        if (active) {
            QEnterEvent event(QPointF(2, 2), QPointF(2, 2), widget->mapToGlobal(QPoint(2, 2)));
            QApplication::sendEvent(widget, &event);
        } else {
            QEvent event(QEvent::Leave);
            QApplication::sendEvent(widget, &event);
        }
    };
    for (auto* control : QList<QWidget*>{segments, values}) {
        for (auto* button : control->findChildren<QToolButton*>(QString(), Qt::FindDirectChildrenOnly)) {
            SCOPED_TRACE(button->text().toStdString());
            button->clearFocus();
            for (bool enabled : {true, false}) {
                button->setEnabled(enabled);
                hover(button, false);
                button->setChecked(false);
                const QColor textColor = button->palette().color(QPalette::ButtonText);
                for (bool checked : {false, true}) {
                    SCOPED_TRACE(testing::Message() << "enabled=" << enabled << " checked=" << checked);
                    button->setChecked(checked);
                    hover(button, false);
                    const auto normal = render(button);
                    EXPECT_TRUE(containsColor(normal, textColor));
                    hover(button, true);
                    const auto hovered = render(button);
                    const QRect interior = normal.rect().adjusted(1, 1, -1, -1);
                    EXPECT_EQ(hovered.copy(interior), normal.copy(interior));
                    for (const QPoint edge : {QPoint(0, button->height() / 2),
                            QPoint(button->width() - 1, button->height() / 2),
                            QPoint(button->width() / 2, 0),
                            QPoint(button->width() / 2, button->height() - 1)})
                        EXPECT_EQ(hovered.pixelColor(edge), enabled
                            ? QColor(QStringLiteral("#123456")) : normal.pixelColor(edge));
                }
            }
            hover(button, false);
            button->setEnabled(true);
            VNX::QtControlSkin::setSemanticProperty(button, "auditionOnly", true);
            const auto audition = render(button);
            EXPECT_EQ(audition.pixelColor(0, button->height() / 2), QColor(QStringLiteral("#123456")));
            VNX::QtControlSkin::setSemanticProperty(button, "auditionOnly", false);
        }
    }

    values->setValue(42);
    auto* editor = values->findChild<QLineEdit*>();
    ASSERT_NE(editor, nullptr);
    auto* numeric = editor->parentWidget();
    const QColor textColor = numeric->palette().color(QPalette::ButtonText);
    const QPointF point = numeric->rect().center();
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, point,
                            numeric->mapToGlobal(point.toPoint()),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(numeric, &doubleClick);
    ASSERT_TRUE(values->isEditing());
    editor->deselect();
    EXPECT_TRUE(containsColor(render(editor), textColor));
}

TEST(QtControlSkin, FieldEditorsShareOuterHeightInFormsAndTables)
{
    VNX::ThemeManager::instance();
    const QString previousStyle = qApp->styleSheet();
    const auto restore = qScopeGuard([&] { qApp->setStyleSheet(previousStyle); });
    const auto skin = VNX::QtControlSkin::current();
    qApp->setStyleSheet(skin.applicationStyleSheet());

    QWidget form;
    auto* layout = new QVBoxLayout(&form);
    layout->setAlignment(Qt::AlignTop);
    auto* text = new QLineEdit(QStringLiteral("Name"), &form);
    auto* search = new VNX::SearchField(&form);
    search->setText(QStringLiteral("Sound"));
    auto* integer = new QSpinBox(&form);
    integer->setProperty("fieldRole", QStringLiteral("inspectorValue"));
    auto* decimal = new QDoubleSpinBox(&form);
    decimal->setProperty("fieldRole", QStringLiteral("inspectorValue"));
    auto* compact = new QDoubleSpinBox(&form);
    compact->setProperty("fieldRole", QStringLiteral("compact"));
    auto* combo = new QComboBox(&form);
    combo->addItem(QStringLiteral("Random"));
    auto* multiCombo = new VNX::MultiSelectComboBox(&form);
    multiCombo->setOptions({{QStringLiteral("First"), 1}, {QStringLiteral("Second"), 2}});
    multiCombo->setSelectedData({1, 2});
    auto* editableCombo = new QComboBox(&form);
    editableCombo->setEditable(true);
    editableCombo->addItem(QStringLiteral("Value"));
    auto* ref = new VNX::ObjectRefWidget(&form);
    auto* compactRef = new VNX::ObjectRefWidget(&form);
    compactRef->setCompact(true);
    auto* segments = new VNX::SegmentedControl(&form);
    segments->setOptions({{QStringLiteral("Off"), 0}, {QStringLiteral("On"), 1}});
    auto* loop = new VNX::ValueSegmentedControl(&form);
    loop->setSegments({{QStringLiteral("Off"), 1},
                       {QStringLiteral("Infinite"), 0},
                       {QString(), 2, true, 2, 100}});
    loop->setValue(2);
    auto* swatch = new QPushButton(QStringLiteral("0xFF808080"), &form);
    swatch->setProperty("fieldRole", QStringLiteral("colorSwatch"));
    const QList<QWidget*> fields = {text, search, integer, decimal, compact,
                                    combo, multiCombo, editableCombo, ref, compactRef,
                                    segments, loop, swatch};
    for (auto* field : fields)
        layout->addWidget(field);

    auto* table = new VNX::TableWidget(&form);
    table->setColumns(QStringLiteral("FieldHeight"), {
        {QStringLiteral("Object"), QStringLiteral("Object"), 500},
        {QStringLiteral("Value"), QStringLiteral("Value"), 500}
    });
    auto* row = new QWidget(table->rowsWidget());
    auto* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    auto* tableRef = new VNX::ObjectRefWidget(row);
    tableRef->setCompact(true);
    auto* tableValue = new QSpinBox(row);
    tableValue->setProperty("fieldRole", QStringLiteral("inspectorValue"));
    rowLayout->addWidget(tableRef);
    rowLayout->addWidget(tableValue);
    table->registerRow(row, {tableRef, tableValue});
    table->rowsLayout()->addWidget(row);
    layout->addWidget(table);
    form.show();

    const int expectedHeight = skin.fieldHeight();
    EXPECT_EQ(expectedHeight, 24);
    for (const int width : {360, 760}) {
        form.resize(width, 680);
        QApplication::processEvents();
        table->refreshLayout();
        for (auto* field : fields) {
            SCOPED_TRACE(field->metaObject()->className());
            EXPECT_EQ(field->height(), expectedHeight);
        }
        EXPECT_EQ(tableRef->height(), expectedHeight);
        EXPECT_EQ(tableValue->height(), expectedHeight);
        for (auto* spin : {static_cast<QAbstractSpinBox*>(integer),
                           static_cast<QAbstractSpinBox*>(decimal),
                           static_cast<QAbstractSpinBox*>(compact)}) {
            auto* editor = spin->findChild<QLineEdit*>();
            ASSERT_NE(editor, nullptr);
            EXPECT_TRUE(spin->rect().contains(editor->geometry()));
            EXPECT_GE(editor->height(), editor->fontMetrics().height());
        }
        EXPECT_TRUE(editableCombo->rect().contains(editableCombo->lineEdit()->geometry()));
    }

    auto* inlineEditor = loop->findChild<QLineEdit*>();
    ASSERT_NE(inlineEditor, nullptr);
    auto* numericButton = inlineEditor->parentWidget();
    const QPointF local = numericButton->rect().center();
    QMouseEvent doubleClick(QEvent::MouseButtonDblClick, local,
                            numericButton->mapToGlobal(local.toPoint()),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(numericButton, &doubleClick);
    QApplication::processEvents();
    ASSERT_TRUE(loop->isEditing());
    EXPECT_EQ(loop->height(), expectedHeight);
    EXPECT_EQ(numericButton->height(), expectedHeight);
    EXPECT_TRUE(numericButton->rect().contains(inlineEditor->geometry()));
    EXPECT_GE(inlineEditor->height(), inlineEditor->fontMetrics().height());
    inlineEditor->setText(QStringLiteral("12"));
    loop->commitNumericEdit();
    QApplication::processEvents();
    EXPECT_FALSE(loop->isEditing());
    EXPECT_EQ(loop->height(), expectedHeight);
}

TEST(QtControlSkin, FieldHeightAccommodatesThemeMetricsAndLargerText)
{
    VNX::ThemeManager::instance();
    const QString previousStyle = qApp->styleSheet();
    const auto restore = qScopeGuard([&] { qApp->setStyleSheet(previousStyle); });
    QWidget form;
    auto* layout = new QVBoxLayout(&form);
    layout->setAlignment(Qt::AlignTop);
    auto* text = new QLineEdit(QStringLiteral("Name"), &form);
    auto* search = new VNX::SearchField(&form);
    auto* value = new QDoubleSpinBox(&form);
    auto* combo = new QComboBox(&form);
    combo->addItem(QStringLiteral("Value"));
    auto* segments = new VNX::SegmentedControl(&form);
    segments->setOptions({{QStringLiteral("Off"), 0}, {QStringLiteral("On"), 1}});
    const QList<QWidget*> fields = {text, search, value, combo, segments};
    for (auto* field : fields)
        layout->addWidget(field);

    for (const int variant : {0, 1, 2, 3, 0}) {
        SCOPED_TRACE(variant);
        auto theme = VNX::createBuiltinDarkTheme();
        if (variant == 1) theme.ui.metrics.controlHeight = 36;
        if (variant == 2) theme.ui.metrics.objectIconExtent = 30;
        if (variant == 3) theme.ui.typography.body.pointSize = 20;
        const VNX::QtControlSkin skin(
            std::make_shared<const VNX::ThemeSnapshot>(theme));
        qApp->setStyleSheet(skin.applicationStyleSheet());
        form.resize(400, 320);
        form.show();
        QApplication::processEvents();
        for (auto* field : fields) {
            SCOPED_TRACE(field->metaObject()->className());
            EXPECT_EQ(field->height(), skin.fieldHeight());
            EXPECT_GE(field->height(), field->fontMetrics().height() + 6);
        }
        EXPECT_GE(skin.fieldHeight(), theme.ui.metrics.controlHeight);
        EXPECT_GE(skin.fieldHeight(), theme.ui.metrics.objectIconExtent + 4);
    }
}

TEST(QtControlSkin, NativeHeadersUseSharedDefaultsWithoutTableRegistration)
{
    VNX::ThemeManager::instance();
    const QString previousStyle = qApp->styleSheet();
    const auto restore = qScopeGuard([&] { qApp->setStyleSheet(previousStyle); });
    const std::unique_ptr<QStyle> referenceStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    ASSERT_TRUE(referenceStyle);

    QTableView plainTable;
    QTreeView plainTree;
    VNX::TableWidget table(VNX::TableOptions::display());
    auto* adaptedView = table.createModelView(QStringLiteral("AdaptedHeader"), nullptr);
    const QList<QHeaderView*> headers = {
        plainTable.horizontalHeader(), plainTree.header(), adaptedView->horizontalHeader()
    };

    for (const int variant : {0, 1, 0}) {
        SCOPED_TRACE(variant);
        auto theme = VNX::createBuiltinDarkTheme();
        if (variant == 1) {
            theme.ui.colors.headerBackground = QColor(QStringLiteral("#edf1f4"));
            theme.ui.colors.textMuted = QColor(QStringLiteral("#273c42"));
            theme.ui.colors.borderPrimary = QColor(QStringLiteral("#81919b"));
            theme.ui.typography.body.pointSize = 14;
            theme.ui.metrics.table.cellPaddingX = 11;
            theme.ui.metrics.table.cellPaddingY = 5;
        }
        const VNX::QtControlSkin skin(std::make_shared<const VNX::ThemeSnapshot>(theme));
        qApp->setStyleSheet(skin.applicationStyleSheet());
        for (auto* header : headers) {
            SCOPED_TRACE(header->parentWidget()->metaObject()->className());
            header->ensurePolished();
            const auto render = [&](QStyle::ControlElement element, const QString& text,
                                    QStyle* reference = nullptr) {
                QStyleOptionHeader option;
                option.initFrom(header);
                option.rect = QRect(0, 0, 260, 48);
                option.state = QStyle::State_Enabled | QStyle::State_Horizontal;
                option.orientation = Qt::Horizontal;
                option.position = QStyleOptionHeader::Beginning;
                option.textAlignment = Qt::AlignLeft | Qt::AlignVCenter;
                option.text = text;
                QFont font = header->font();
                if (reference) {
                    font.setPointSize(theme.ui.typography.body.pointSize);
                    font.setWeight(QFont::Normal);
                    font.setCapitalization(QFont::MixedCase);
                    font.setLetterSpacing(QFont::AbsoluteSpacing, 0);
                    option.palette.setColor(QPalette::ButtonText, theme.ui.colors.textMuted);
                }
                QImage image(option.rect.size(), QImage::Format_ARGB32_Premultiplied);
                image.setDotsPerMeterX(qRound(header->logicalDpiX() / 0.0254));
                image.setDotsPerMeterY(qRound(header->logicalDpiY() / 0.0254));
                image.fill(Qt::transparent);
                QPainter painter(&image);
                painter.setFont(font);
                (reference ? reference : header->style())->drawControl(
                    element, &option, &painter, header);
                return image;
            };

            const auto section = render(QStyle::CE_HeaderSection, QString());
            EXPECT_EQ(section.pixelColor(4, 4), theme.ui.colors.headerBackground);
            EXPECT_EQ(section.pixelColor(4, section.height() - 1), theme.ui.colors.borderPrimary);
            EXPECT_EQ(section.pixelColor(section.width() - 1, 4), theme.ui.colors.borderPrimary);
            const auto label = render(QStyle::CE_HeaderLabel, QStringLiteral("Object Ref"));
            EXPECT_EQ(label, render(QStyle::CE_HeaderLabel, QStringLiteral("Object Ref"),
                                   referenceStyle.get()));
            EXPECT_NE(label, render(QStyle::CE_HeaderLabel, QStringLiteral("OBJECT REF")));
        }
    }
}

TEST(QtControlSkin, ApplicationStyleSheetSupportsHighContrastTokens)
{
    VNX::ThemeSnapshot theme = VNX::createBuiltinDarkTheme();
    theme.ui.colors.accent = QColor(QStringLiteral("#ffffff00"));
    theme.ui.colors.accentText = QColor(QStringLiteral("#ff000000"));
    theme.ui.colors.bgPrimary = QColor(QStringLiteral("#ff000000"));
    theme.ui.colors.bgSecondary = QColor(QStringLiteral("#ff080808"));
    theme.ui.colors.bgHover = QColor(QStringLiteral("#ff303030"));
    theme.ui.colors.textPrimary = QColor(QStringLiteral("#ffffffff"));
    theme.ui.colors.textDisabled = QColor(QStringLiteral("#ffb0b0b0"));
    theme.ui.colors.borderPrimary = QColor(QStringLiteral("#ffffffff"));
    theme.ui.colors.error = QColor(QStringLiteral("#40180020"));
    theme.qtPalette.window = theme.ui.colors.bgPrimary;

    const QString styleSheet = VNX::QtControlSkin(
        std::make_shared<const VNX::ThemeSnapshot>(std::move(theme)))
        .applicationStyleSheet();

    EXPECT_TRUE(styleSheet.contains(QStringLiteral("#ffffff00")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("#ff000000")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("#ffffffff")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("#ffb0b0b0")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral(
        "QPushButton[danger=\"true\"]:hover {\n"
        "    background-color: #ff180020;\n"
        "    color: #ffffffff;\n"
        "}")));
}

TEST(QtControlSkin, ApplicationStyleSheetIsDerivedFromSnapshot)
{
    VNX::ThemeSnapshot theme = VNX::createBuiltinDarkTheme();
    theme.colorScheme = VNX::ThemeColorScheme::Light;
    theme.ui.colors.accent = QColor(QStringLiteral("#ffcc2200"));
    theme.ui.colors.accentHover = QColor(QStringLiteral("#ffff6633"));
    theme.ui.colors.accentText = QColor(QStringLiteral("#ffffffff"));
    theme.ui.colors.bgPrimary = QColor(QStringLiteral("#fffff8e8"));
    theme.ui.colors.bgSecondary = QColor(QStringLiteral("#fff2e3c4"));
    theme.ui.colors.bgTertiary = QColor(QStringLiteral("#ffe4c996"));
    theme.ui.colors.bgHover = QColor(QStringLiteral("#ffffd28a"));
    theme.ui.colors.textPrimary = QColor(QStringLiteral("#ff201000"));
    theme.ui.colors.textMuted = QColor(QStringLiteral("#ff887766"));
    theme.ui.colors.textDisabled = QColor(QStringLiteral("#ffad967d"));
    theme.ui.colors.borderPrimary = QColor(QStringLiteral("#ff9b5c22"));
    theme.ui.colors.error = QColor(QStringLiteral("#40fff4cc"));
    theme.ui.colors.success = QColor(QStringLiteral("#ff118833"));
    theme.ui.colors.warning = QColor(QStringLiteral("#ffaa6611"));
    theme.ui.colors.info = QColor(QStringLiteral("#ff2255aa"));
    theme.ui.colors.sectionSeparator = QColor(QStringLiteral("#ff564738"));
    theme.domains.ai.brandAccent = QColor(QStringLiteral("#ff663399"));
    theme.domains.ai.userAccent = QColor(QStringLiteral("#ff336699"));
    theme.domains.monitor.cpuSafe = QColor(QStringLiteral("#ff228844"));
    theme.domains.monitor.cpuWarning = QColor(QStringLiteral("#ffaa7722"));
    theme.domains.monitor.cpuCritical = QColor(QStringLiteral("#ffbb2233"));
    theme.domains.monitor.bufferNormal = QColor(QStringLiteral("#ff2255bb"));
    theme.domains.monitor.bufferWarning = QColor(QStringLiteral("#ffcc7722"));
    theme.ui.metrics.radiusSmall = 11;
    theme.ui.metrics.spacingMedium = 17;
    theme.ui.typography.body.pointSize = 15;
    theme.qtPalette.window = theme.ui.colors.bgPrimary;
    theme.qtPalette.base = theme.ui.colors.bgSecondary;
    theme.qtPalette.alternateBase = theme.ui.colors.bgTertiary;

    const VNX::QtControlSkin skin(
        std::make_shared<const VNX::ThemeSnapshot>(std::move(theme)));
    const QString styleSheet = skin.applicationStyleSheet();

    ASSERT_FALSE(styleSheet.isEmpty());
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("background-color: #fffff8e8")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("color: #ff201000")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("border: 1px solid #ff9b5c22")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("background-color: #ffffd28a")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("color: #ffad967d")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("border-radius: 11px")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("font-size: 15pt")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral(
        "QPushButton[danger=\"true\"] {\n"
        "    background-color: transparent;\n"
        "    border: 1px solid #fffff4cc;\n"
        "    color: #fffff4cc;\n"
        "}")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral(
        "QPushButton[danger=\"true\"]:hover {\n"
        "    background-color: #fffff4cc;\n"
        "    color: #ff000000;\n"
        "}")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("color: #ffc0c0c0;")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("background-color: rgba(32, 16, 0, 24);")));
    EXPECT_FALSE(styleSheet.contains(QStringLiteral("@shellMenuText@")));
    EXPECT_FALSE(styleSheet.contains(QStringLiteral("@shellCloseHover@")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("QLabel[settingsRole=\"statusSuccess\"] { color: #ff118833; }")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("QLabel[settingsRole=\"statusWarning\"] { color: #ffaa6611; }")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("#ff564738")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("QLabel[textRole=\"muted\"] { color: #ff887766; }")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("#AIAssistantPanel QFrame#aiInputFrame[focused=\"true\"]")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("#ff663399")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("QFrame#listBox")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("QWidget#inspectorTabBar[sideBar=\"true\"]")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("VNX--ObjectRefWidget[dimmed=\"true\"]")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("QComboBox {\n    background-color: #fff2e3c4;\n")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("QComboBox::drop-down {")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("QComboBox::down-arrow {")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("QComboBox QAbstractItemView {\n    background-color: #ffe4c996;\n")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("#monitorConnectionLabel[connectionRole=\"live\"] { color: #ffcc2200; }")));
    EXPECT_FALSE(styleSheet.contains(QStringLiteral("#ff0d111a")));
    EXPECT_FALSE(styleSheet.contains(QStringLiteral("font-family:")));
    EXPECT_FALSE(styleSheet.contains(QRegularExpression(QStringLiteral("@[A-Za-z]+@"))));
    EXPECT_EQ(styleSheet.count(QStringLiteral("@harken-qtads-theme@")), 1);
}

TEST(QtControlSkin, ButtonStatesRemainDistinctAndDisabledIgnoresInteraction)
{
    const QString previousStyle = qApp->styleSheet();
    const auto restore = qScopeGuard([&]() { qApp->setStyleSheet(previousStyle); });
    const auto skin = builtinSkin();
    qApp->setStyleSheet(skin.applicationStyleSheet());
    for (bool isDefault : {false, true}) {
        SCOPED_TRACE(isDefault);
        QPushButton button;
        button.setDefault(isDefault);
        button.resize(120, 40);
        button.ensurePolished();
        const auto render = [&](QStyle::State state) {
            QImage image(button.size(), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QStyleOptionButton option;
            option.initFrom(&button);
            option.state = state;
            if (isDefault) option.features |= QStyleOptionButton::DefaultButton;
            QPainter painter(&image);
            button.style()->drawControl(QStyle::CE_PushButton, &option, &painter, &button);
            return image;
        };
        const auto normal = render(QStyle::State_Enabled);
        const auto hover = render(QStyle::State_Enabled | QStyle::State_MouseOver);
        const auto pressed = render(QStyle::State_Enabled | QStyle::State_MouseOver | QStyle::State_Sunken);
        const auto disabled = render(QStyle::State_None);
        EXPECT_NE(normal, hover);
        EXPECT_NE(hover, pressed);
        EXPECT_NE(normal, disabled);
        EXPECT_EQ(disabled, render(QStyle::State_MouseOver | QStyle::State_Sunken));
    }
}

TEST(QtControlSkin, MixedCheckboxStatesRemainDistinctAcrossRoles)
{
    const QString previousStyle = qApp->styleSheet();
    const auto restore = qScopeGuard([&]() { qApp->setStyleSheet(previousStyle); });
    auto theme = VNX::createBuiltinDarkTheme();
    theme.ui.colors.accent = QColor(QStringLiteral("#f02040"));
    theme.ui.colors.textPrimary = QColor(QStringLiteral("#4060f0"));
    theme.ui.colors.textDisabled = QColor(QStringLiteral("#30d070"));
    const VNX::QtControlSkin skin(
        std::make_shared<const VNX::ThemeSnapshot>(std::move(theme)));
    qApp->setStyleSheet(skin.applicationStyleSheet());

    for (const QString role : {QString(), QStringLiteral("inspector")}) {
        QCheckBox checkBox;
        if (!role.isEmpty())
            VNX::QtControlSkin::setSemanticProperty(&checkBox, "fieldRole", role);
        checkBox.resize(16, 16);
        checkBox.ensurePolished();
        const auto render = [&](QStyle::State state) {
            QImage image(checkBox.size(), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            QStyleOptionButton option;
            option.initFrom(&checkBox);
            option.state = state;
            QPainter painter(&image);
            checkBox.style()->drawPrimitive(
                QStyle::PE_IndicatorCheckBox, &option, &painter, &checkBox);
            return image;
        };
        for (const bool enabled : {true, false}) {
            for (const bool hovered : {false, true}) {
                SCOPED_TRACE(::testing::Message() << role.toStdString()
                             << " enabled=" << enabled << " hovered=" << hovered);
                QStyle::State state = QStyle::State_None;
                if (enabled) state |= QStyle::State_Enabled;
                if (hovered) state |= QStyle::State_MouseOver;
                const auto mixed = render(state | QStyle::State_NoChange);
                EXPECT_NE(mixed, render(state | QStyle::State_Off));
                EXPECT_NE(mixed, render(state | QStyle::State_On));
            }
        }
    }
}

TEST(QtControlSkin, MissingChevronExportKeepsTheRestOfTheSkin)
{
    VNX::ThemeSnapshot theme = VNX::createBuiltinDarkTheme();
    theme.ui.metrics.navigationChevronExtent = 0;
    const QString styleSheet = VNX::QtControlSkin(
        std::make_shared<const VNX::ThemeSnapshot>(std::move(theme)))
        .applicationStyleSheet();

    ASSERT_FALSE(styleSheet.isEmpty());
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("background-color: #ff0b0e14")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("image: url(\"@chevronDown@\")")));
    EXPECT_TRUE(styleSheet.contains(QStringLiteral("image: url(\"@chevronDownDisabled@\")")));
}
